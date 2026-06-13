#include "RtScene.h"
#include "RtCore.h"
#include "RtLog.h"
#include "GeometryCapture.h"
#include "GeometryReplay.h"
#include "D3D11Hook.h"
#include "TerrainTess.h"
#include "DustAPI.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>

namespace RtScene
{

// ---------------------------------------------------------------------------
// pools / limits
// ---------------------------------------------------------------------------
static const uint32_t kMaxPoolVerts   = 5 * 1024 * 1024;   // 60 MB float3
static const uint32_t kMaxPoolIndices = 15 * 1024 * 1024;  // 60 MB u32
static const uint32_t kMaxInstances   = 8192;
static const uint32_t kMaxBlasPerFrame = 64;
static const uint32_t kMaxPairReadbacksPerFrame = 2;
static const uint32_t kFrameSlots     = 3;

static ID3D12Resource* sPosPool = nullptr;
static ID3D12Resource* sIdxPool = nullptr;
static uint32_t        sPoolVertsUsed = 0;
static uint32_t        sPoolIdxUsed = 0;
static bool            sPoolFullLogged = false;

static RtCore::Tlas    sTlas;
static uint32_t        sInstanceCount = 0;

struct RtInstInfoCpu { uint32_t ibBase, vbBase, flags, pad; };
static ID3D12Resource* sInstInfo[kFrameSlots] = {};
static RtInstInfoCpu*  sInstInfoMapped[kFrameSlots] = {};

static Stats sStats;

// ---------------------------------------------------------------------------
// input layout registry (POSITION location per layout)
// ---------------------------------------------------------------------------
struct LayoutInfo
{
    uint32_t posOffset; DXGI_FORMAT posFormat; uint32_t posSlot;
    // Any per-instance element means the world transform may come from the
    // instance stream, NOT the CB — raw VB positions would then be model-local
    // and an identity/CB transform places them wrong (pile at origin).
    bool hasPerInstance;
};
static std::unordered_map<ID3D11InputLayout*, LayoutInfo> sLayouts;

void OnInputLayoutCreated(const D3D11_INPUT_ELEMENT_DESC* descs, UINT count,
                          ID3D11InputLayout* layout)
{
    if (!descs || !layout) return;
    LayoutInfo li = {};
    bool havePos = false;
    for (UINT i = 0; i < count; ++i)
    {
        if (descs[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA)
            li.hasPerInstance = true;
        if (!havePos && descs[i].SemanticName &&
            _stricmp(descs[i].SemanticName, "POSITION") == 0 && descs[i].SemanticIndex == 0)
        {
            li.posOffset = descs[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                           ? 0 : descs[i].AlignedByteOffset;
            li.posFormat = descs[i].Format;
            li.posSlot   = descs[i].InputSlot;
            havePos = true;
        }
    }
    if (!havePos) return;
    layout->AddRef();   // pin so the pointer key can't be recycled
    sLayouts[layout] = li;
}

// ---------------------------------------------------------------------------
// mesh ranges
// ---------------------------------------------------------------------------
struct MeshKey
{
    ID3D11Buffer* vb; ID3D11Buffer* ib;
    uint32_t vbOffset, stride, ibOffset;
    uint32_t startIndex, indexCount;
    int32_t  baseVertex;
    uint32_t ibFmt;
    bool operator==(const MeshKey& o) const { return memcmp(this, &o, sizeof(MeshKey)) == 0; }
};
struct MeshKeyHash
{
    size_t operator()(const MeshKey& k) const
    {
        const uint8_t* p = (const uint8_t*)&k;
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(MeshKey); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
};

enum class MeshState : uint8_t { WaitingData, ReadyToUpload, Built, Failed, Aliased };

struct MeshRecord
{
    MeshState state = MeshState::WaitingData;
    // CPU-side parsed data (cleared after upload)
    std::vector<float>    positions;   // xyz per vertex
    std::vector<uint32_t> indices;     // rebased to the range's vertex window
    // pool placement
    uint32_t vbBase = 0, vertexCount = 0;
    uint32_t ibBase = 0, indexCount = 0;
    ID3D12Resource* blas = nullptr;
    // Aliased: identical geometry CONTENT already exists — OGRE recycles
    // buffer bindings so the same mesh reappears under new (vb,ib,offset)
    // keys every frame; without content dedup the pools fill in seconds.
    int alias = -1;
    // local-space bounds (diagnostic: world-space-baked meshes have huge,
    // far-from-origin AABBs; model-local ones are small and origin-centered)
    float aabbMin[3] = {}, aabbMax[3] = {};
    // parse inputs (copied from the captured draw at registration)
    uint32_t vbOffset = 0, stride = 0, ibOffset = 0;
    uint32_t startIndex = 0;
    int32_t  baseVertex = 0;
    DXGI_FORMAT ibFmt = DXGI_FORMAT_R16_UINT;
    uint32_t posOffset = 0;
};

static std::unordered_map<MeshKey, int, MeshKeyHash> sMeshIndex;
static std::vector<MeshRecord> sMeshes;
// content hash (positions+indices bytes) -> canonical mesh index
static std::unordered_map<uint64_t, int> sContentIndex;
// churn diagnostics: distinct buffer pairs vs range keys ever seen
static std::unordered_set<uint64_t> sPairsSeen;
static uint32_t sDedupHits = 0;

// ---------------------------------------------------------------------------
// pending buffer-pair readbacks
// ---------------------------------------------------------------------------
struct PendingPair
{
    ID3D11Buffer* vb = nullptr;          // identity only — never dereferenced
    ID3D11Buffer* ib = nullptr;
    ID3D11Buffer* vbStaging = nullptr;
    ID3D11Buffer* ibStaging = nullptr;
    uint32_t vbSize = 0, ibSize = 0;
    int framesWaited = 0;
    std::vector<int> records;
};
static std::vector<PendingPair> sPending;
static bool sAutoFlushPending = false;
static uint32_t sPoolFlushes = 0;
static bool sPlacementDumped = false;

// this frame's instances: mesh record index + DXR 3x4 transform
struct PendingInstance { int mesh; float t[12]; };
static std::vector<PendingInstance> sFrameInstances;

// ---------------------------------------------------------------------------
void Init()
{
    sStats = {};
}

static ID3D11Buffer* CreateStagingCopy(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                                       ID3D11Buffer* src, uint32_t* outSize)
{
    D3D11_BUFFER_DESC d = {};
    src->GetDesc(&d);
    *outSize = d.ByteWidth;
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth = d.ByteWidth;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    if (FAILED(dev->CreateBuffer(&sd, nullptr, &staging)))
        return nullptr;
    ctx->CopyResource(staging, src);
    return staging;
}

// Parse one mesh range out of mapped VB/IB bytes into the record's CPU arrays.
static bool ParseRange(MeshRecord& r, const uint8_t* vbData, uint32_t vbSize,
                       const uint8_t* ibData, uint32_t ibSize)
{
    const uint32_t idxSize = (r.ibFmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
    const uint64_t ibStart = (uint64_t)r.ibOffset + (uint64_t)r.startIndex * idxSize;
    if (ibStart + (uint64_t)r.indexCount * idxSize > ibSize) return false;
    if (r.stride < 12 || r.indexCount < 3) return false;

    // pass 1: vertex window
    uint32_t minV = 0xFFFFFFFFu, maxV = 0;
    const uint8_t* ip = ibData + ibStart;
    for (uint32_t i = 0; i < r.indexCount; ++i)
    {
        uint32_t idx = (idxSize == 4) ? ((const uint32_t*)ip)[i] : ((const uint16_t*)ip)[i];
        int64_t v = (int64_t)r.baseVertex + idx;
        if (v < 0) return false;
        if ((uint32_t)v < minV) minV = (uint32_t)v;
        if ((uint32_t)v > maxV) maxV = (uint32_t)v;
    }
    const uint32_t vertCount = maxV - minV + 1;
    if (vertCount > 4 * 1024 * 1024) return false;   // implausible range — bad capture
    const uint64_t lastByte = (uint64_t)r.vbOffset + (uint64_t)maxV * r.stride + r.posOffset + 12;
    if (lastByte > vbSize) return false;

    // pass 2: copy positions + rebased indices
    r.positions.resize((size_t)vertCount * 3);
    for (int k = 0; k < 3; ++k) { r.aabbMin[k] = 3.4e38f; r.aabbMax[k] = -3.4e38f; }
    for (uint32_t v = 0; v < vertCount; ++v)
    {
        const float* p = (const float*)(vbData + (uint64_t)r.vbOffset +
                                        (uint64_t)(minV + v) * r.stride + r.posOffset);
        r.positions[(size_t)v * 3 + 0] = p[0];
        r.positions[(size_t)v * 3 + 1] = p[1];
        r.positions[(size_t)v * 3 + 2] = p[2];
        for (int k = 0; k < 3; ++k)
        {
            if (p[k] < r.aabbMin[k]) r.aabbMin[k] = p[k];
            if (p[k] > r.aabbMax[k]) r.aabbMax[k] = p[k];
        }
    }
    // NaN / absurd-coordinate guard: a wrong stride/offset reads garbage
    for (int k = 0; k < 3; ++k)
        if (!(r.aabbMin[k] == r.aabbMin[k]) || !(r.aabbMax[k] == r.aabbMax[k]) ||
            r.aabbMax[k] > 1.0e7f || r.aabbMin[k] < -1.0e7f)
            return false;
    r.indices.resize(r.indexCount);
    for (uint32_t i = 0; i < r.indexCount; ++i)
    {
        uint32_t idx = (idxSize == 4) ? ((const uint32_t*)ip)[i] : ((const uint16_t*)ip)[i];
        r.indices[i] = (uint32_t)((int64_t)r.baseVertex + idx) - minV;
    }
    r.vertexCount = vertCount;
    return true;
}

static void ProcessPending(ID3D11DeviceContext* ctx)
{
    for (size_t pi = 0; pi < sPending.size(); )
    {
        PendingPair& p = sPending[pi];
        p.framesWaited++;
        // stall-free map; after a generous grace period allow a blocking map
        const UINT flag = (p.framesWaited < 8) ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0;

        D3D11_MAPPED_SUBRESOURCE mv = {}, mi = {};
        HRESULT hv = ctx->Map(p.vbStaging, 0, D3D11_MAP_READ, flag, &mv);
        if (hv == DXGI_ERROR_WAS_STILL_DRAWING) { ++pi; continue; }
        HRESULT hi = SUCCEEDED(hv) ? ctx->Map(p.ibStaging, 0, D3D11_MAP_READ, flag, &mi)
                                   : E_FAIL;
        if (hi == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            ctx->Unmap(p.vbStaging, 0);
            ++pi;
            continue;
        }

        if (SUCCEEDED(hv) && SUCCEEDED(hi))
        {
            for (int rec : p.records)
            {
                MeshRecord& r = sMeshes[rec];
                if (r.state != MeshState::WaitingData) continue;
                if (ParseRange(r, (const uint8_t*)mv.pData, p.vbSize,
                               (const uint8_t*)mi.pData, p.ibSize))
                {
                    // content identity: the same mesh reappears under fresh
                    // buffer keys every frame (OGRE recycling) — dedup by the
                    // parsed bytes so pools/BLASes hold each mesh exactly once
                    uint64_t h = 1469598103934665603ull;
                    auto mix64 = [&h](const void* data, size_t bytes)
                    {
                        const uint64_t* q = (const uint64_t*)data;
                        size_t n = bytes / 8;
                        for (size_t i = 0; i < n; ++i) { h ^= q[i]; h *= 1099511628211ull; }
                        const uint8_t* tail = (const uint8_t*)data + n * 8;
                        for (size_t i = 0; i < (bytes & 7); ++i) { h ^= tail[i]; h *= 1099511628211ull; }
                    };
                    mix64(r.positions.data(), r.positions.size() * 4);
                    mix64(r.indices.data(), r.indices.size() * 4);

                    auto hit = sContentIndex.find(h);
                    if (hit != sContentIndex.end())
                    {
                        r.alias = hit->second;
                        r.state = MeshState::Aliased;
                        r.positions.clear(); r.positions.shrink_to_fit();
                        r.indices.clear();   r.indices.shrink_to_fit();
                        sDedupHits++;
                    }
                    else if (sPoolVertsUsed + r.vertexCount > kMaxPoolVerts ||
                             sPoolIdxUsed + r.indexCount > kMaxPoolIndices)
                    {
                        if (!sPoolFullLogged)
                        {
                            Log("RtScene: geometry pools full (%u verts, %u idx) — auto-flush scheduled",
                                sPoolVertsUsed, sPoolIdxUsed);
                            sPoolFullLogged = true;
                        }
                        sAutoFlushPending = true;
                        r.state = MeshState::Failed;
                        sStats.parseFailures++;
                    }
                    else
                    {
                        sContentIndex[h] = rec;
                        r.vbBase = sPoolVertsUsed;
                        r.ibBase = sPoolIdxUsed;
                        sPoolVertsUsed += r.vertexCount;
                        sPoolIdxUsed += r.indexCount;
                        r.state = MeshState::ReadyToUpload;
                    }
                }
                else
                {
                    r.state = MeshState::Failed;
                    sStats.parseFailures++;
                }
            }
            ctx->Unmap(p.vbStaging, 0);
            ctx->Unmap(p.ibStaging, 0);
        }
        else
        {
            // map failed outright — fail the records so they don't wait forever
            for (int rec : p.records)
                if (sMeshes[rec].state == MeshState::WaitingData)
                {
                    sMeshes[rec].state = MeshState::Failed;
                    sStats.parseFailures++;
                }
            if (SUCCEEDED(hv)) ctx->Unmap(p.vbStaging, 0);
        }

        p.vbStaging->Release();
        p.ibStaging->Release();
        sPending[pi] = sPending.back();
        sPending.pop_back();
    }
}

void Harvest(ID3D11DeviceContext* ctx, ID3D11Device* device)
{
    if (!RtCore::IsAvailable() || !ctx || !device) return;

    if (sAutoFlushPending)
    {
        sAutoFlushPending = false;
        sPoolFlushes++;
        Flush();
    }

    ProcessPending(ctx);

    const GeometryReplay::ReplayDrawInfo* infos = nullptr;
    uint32_t count = 0;
    if (!GeometryReplay::GetFrameCache(ctx, &infos, &count))
    {
        sFrameInstances.clear();
        return;
    }

    const auto& captures = GeometryCapture::GetCaptures();
    sFrameInstances.clear();
    uint32_t newPairsThisFrame = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& info = infos[i];
        if (info.category != DUST_SHADER_OBJECTS &&
            info.category != DUST_SHADER_DISTANT_TOWN &&
            info.category != DUST_SHADER_TRIPLANAR)
            continue;
        if (info.drawIndex >= captures.size()) continue;
        const CapturedDraw& d = captures[info.drawIndex];

        if (d.instanceCount > 1) { sStats.skippedInstanced++; continue; }
        if (!d.vertexBuffers[0] || !d.indexBuffer) continue;
        if (d.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) continue;

        MeshKey key = {};
        key.vb = d.vertexBuffers[0];
        key.ib = d.indexBuffer;
        key.vbOffset = d.vbOffsets[0];
        key.stride = d.vbStrides[0];
        key.ibOffset = d.ibOffset;
        key.startIndex = d.startIndexLocation;
        key.indexCount = d.indexCount;
        key.baseVertex = d.baseVertexLocation;
        key.ibFmt = (uint32_t)d.indexFormat;

        int meshIdx = -1;
        auto it = sMeshIndex.find(key);
        if (it != sMeshIndex.end())
        {
            meshIdx = it->second;
        }
        else
        {
            // POSITION location from the layout registry; fall back to offset 0
            uint32_t posOffset = 0;
            bool posOk = true;
            auto lit = sLayouts.find(d.inputLayout);
            if (lit != sLayouts.end())
            {
                // per-instance layouts: world may live in the instance stream,
                // not the CB — placing these from raw VB data is wrong, skip
                // (counted; instance-stream parsing is the follow-up)
                if (lit->second.hasPerInstance) { sStats.skippedInstanced++; continue; }
                if (lit->second.posSlot != 0 ||
                    (lit->second.posFormat != DXGI_FORMAT_R32G32B32_FLOAT &&
                     lit->second.posFormat != DXGI_FORMAT_R32G32B32A32_FLOAT))
                    posOk = false;
                else
                    posOffset = lit->second.posOffset;
            }
            if (!posOk) continue;

            // an in-flight readback of the same buffers can carry this range too
            // (the staging holds the whole buffer, so any range parses from it)
            PendingPair* ride = nullptr;
            for (auto& p : sPending)
                if (p.vb == key.vb && p.ib == key.ib) { ride = &p; break; }

            if (!ride && newPairsThisFrame >= kMaxPairReadbacksPerFrame)
                continue;   // budget exhausted — try again next frame

            sPairsSeen.insert((uint64_t)(uintptr_t)key.vb ^ ((uint64_t)(uintptr_t)key.ib << 1));

            MeshRecord rec;
            rec.vbOffset = key.vbOffset;
            rec.stride = key.stride;
            rec.ibOffset = key.ibOffset;
            rec.startIndex = key.startIndex;
            rec.indexCount = key.indexCount;
            rec.baseVertex = key.baseVertex;
            rec.ibFmt = d.indexFormat;
            rec.posOffset = posOffset;
            sMeshes.push_back(std::move(rec));
            meshIdx = (int)sMeshes.size() - 1;
            sMeshIndex[key] = meshIdx;

            if (ride)
            {
                ride->records.push_back(meshIdx);
            }
            else
            {
                uint32_t vbSize = 0, ibSize = 0;
                ID3D11Buffer* vbS = CreateStagingCopy(ctx, device, key.vb, &vbSize);
                ID3D11Buffer* ibS = vbS ? CreateStagingCopy(ctx, device, key.ib, &ibSize) : nullptr;
                if (!vbS || !ibS)
                {
                    if (vbS) vbS->Release();
                    sMeshes[meshIdx].state = MeshState::Failed;
                    sStats.parseFailures++;
                }
                else
                {
                    PendingPair p;
                    p.vb = key.vb; p.ib = key.ib;
                    p.vbStaging = vbS; p.ibStaging = ibS;
                    p.vbSize = vbSize; p.ibSize = ibSize;
                    p.records.push_back(meshIdx);
                    sPending.push_back(std::move(p));
                    newPairsThisFrame++;
                }
            }
        }

        if (meshIdx < 0) continue;
        // resolve content-dedup aliases to the canonical record
        if (sMeshes[meshIdx].state == MeshState::Aliased && sMeshes[meshIdx].alias >= 0)
            meshIdx = sMeshes[meshIdx].alias;
        if (sFrameInstances.size() >= kMaxInstances) break;

        PendingInstance inst;
        inst.mesh = meshIdx;
        if (info.placement == 1)
        {
            // row-vector world (pos·W) -> DXR row-major 3x4 (M·pos): M[r][c] = W[c][r]
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    inst.t[r * 4 + c] = info.worldM[c * 4 + r];
        }
        else
        {
            memset(inst.t, 0, sizeof(inst.t));
            inst.t[0] = inst.t[5] = inst.t[10] = 1.0f;
        }
        sFrameInstances.push_back(inst);
    }
}

// ---------------------------------------------------------------------------
bool RecordBuild(ID3D12GraphicsCommandList4* cmd, uint32_t frameSlot)
{
    if (!RtCore::IsAvailable() || !cmd) return false;

    // lazy GPU-side allocations
    if (!sPosPool)
    {
        sPosPool = RtCore::CreateBuffer((uint64_t)kMaxPoolVerts * 12, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_STATE_COMMON);
        sIdxPool = RtCore::CreateBuffer((uint64_t)kMaxPoolIndices * 4, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_STATE_COMMON);
        if (!sTlas.Create(kMaxInstances)) return false;
        for (uint32_t i = 0; i < kFrameSlots; ++i)
        {
            sInstInfo[i] = RtCore::CreateBuffer(sizeof(RtInstInfoCpu) * (uint64_t)kMaxInstances,
                                                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!sInstInfo[i] || FAILED(sInstInfo[i]->Map(0, nullptr, (void**)&sInstInfoMapped[i])))
                return false;
        }
        if (!sPosPool || !sIdxPool) return false;
    }

    // ---- upload newly parsed meshes -------------------------------------
    uint64_t uploadBytes = 0;
    std::vector<int> ready;
    for (int i = 0; i < (int)sMeshes.size(); ++i)
    {
        if (sMeshes[i].state == MeshState::ReadyToUpload)
        {
            ready.push_back(i);
            uploadBytes += sMeshes[i].positions.size() * 4 + sMeshes[i].indices.size() * 4;
            if (ready.size() >= kMaxBlasPerFrame) break;
        }
    }

    if (!ready.empty())
    {
        std::vector<uint8_t> blob;
        blob.reserve((size_t)uploadBytes);
        struct CopyOp { uint64_t srcOff, dstOff, size; ID3D12Resource* dst; };
        std::vector<CopyOp> ops;
        for (int i : ready)
        {
            MeshRecord& r = sMeshes[i];
            uint64_t posBytes = r.positions.size() * 4;
            uint64_t idxBytes = r.indices.size() * 4;
            CopyOp op1 = { blob.size(), (uint64_t)r.vbBase * 12, posBytes, sPosPool };
            blob.insert(blob.end(), (uint8_t*)r.positions.data(), (uint8_t*)r.positions.data() + posBytes);
            CopyOp op2 = { blob.size(), (uint64_t)r.ibBase * 4, idxBytes, sIdxPool };
            blob.insert(blob.end(), (uint8_t*)r.indices.data(), (uint8_t*)r.indices.data() + idxBytes);
            ops.push_back(op1);
            ops.push_back(op2);
        }
        ID3D12Resource* upload = RtCore::CreateUploadBuffer(blob.data(), blob.size());
        if (upload)
        {
            for (const auto& op : ops)
                cmd->CopyBufferRegion(op.dst, op.dstOff, upload, op.srcOff, op.size);
            RtCore::DeferRelease(upload);

            // copies must land before the BLAS builds read the pools
            D3D12_RESOURCE_BARRIER uav[2] = {};
            uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav[0].UAV.pResource = nullptr;
            cmd->ResourceBarrier(1, uav);

            for (int i : ready)
            {
                MeshRecord& r = sMeshes[i];
                r.blas = RtCore::BuildBLAS(cmd,
                    sPosPool->GetGPUVirtualAddress() + (uint64_t)r.vbBase * 12, r.vertexCount, 12,
                    sIdxPool->GetGPUVirtualAddress() + (uint64_t)r.ibBase * 4, r.indexCount,
                    DXGI_FORMAT_R32_UINT);
                if (r.blas)
                {
                    r.state = MeshState::Built;
                    sStats.blasBuilt++;
                }
                else
                {
                    r.state = MeshState::Failed;
                    sStats.parseFailures++;
                }
                r.positions.clear(); r.positions.shrink_to_fit();
                r.indices.clear();   r.indices.shrink_to_fit();
            }

            // BLAS builds must finish before the TLAS consumes them
            cmd->ResourceBarrier(1, uav);
        }
    }

    // ---- TLAS -------------------------------------------------------------
    frameSlot %= kFrameSlots;
    uint32_t n = 0;
    for (const auto& inst : sFrameInstances)
    {
        const MeshRecord& r = sMeshes[inst.mesh];
        if (r.state != MeshState::Built || !r.blas) continue;
        if (n >= sTlas.capacity) break;

        D3D12_RAYTRACING_INSTANCE_DESC& d = sTlas.mapped[n];
        memcpy(d.Transform, inst.t, sizeof(inst.t));
        d.InstanceID = n;
        d.InstanceMask = 0xFF;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE |
                  D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        d.AccelerationStructure = r.blas->GetGPUVirtualAddress();

        RtInstInfoCpu& ii = sInstInfoMapped[frameSlot][n];
        ii.ibBase = r.ibBase;
        ii.vbBase = r.vbBase;
        ii.flags = 0; ii.pad = 0;
        n++;
    }
    // One-shot placement dump once a real population exists: camera vs the
    // instances' world translations and mesh AABBs. The single most useful
    // log block when "nothing casts": misplacement shows as translations or
    // AABB centers kilometers from the camera (or everything at the origin).
    if (!sPlacementDumped && n >= 16)
    {
        sPlacementDumped = true;
        float cam[3] = {};
        // render-world camera (VB space); the deferred matrix translation is
        // in the rebased lighting frame and only logged for comparison
        bool camOk = TerrainTess::GetCameraPos(cam);
        float ivCam[3] = {};
        D3D11Hook::GetCameraWorldPos(ivCam);
        if (!camOk) { cam[0] = ivCam[0]; cam[1] = ivCam[1]; cam[2] = ivCam[2]; }
        Log("RtScene PLACEMENT: cam=(%.0f, %.0f, %.0f)%s ivCam=(%.0f, %.0f, %.0f) instances=%u",
            cam[0], cam[1], cam[2], camOk ? "" : " [iv fallback]",
            ivCam[0], ivCam[1], ivCam[2], n);
        for (uint32_t i = 0; i < n && i < 8; ++i)
        {
            const PendingInstance& inst = sFrameInstances[i];
            const MeshRecord& r = sMeshes[inst.mesh];
            const float* t = inst.t;
            float cx = (r.aabbMin[0] + r.aabbMax[0]) * 0.5f;
            float cy = (r.aabbMin[1] + r.aabbMax[1]) * 0.5f;
            float cz = (r.aabbMin[2] + r.aabbMax[2]) * 0.5f;
            // instance world center = M * localCenter (M rows = t[0..3], t[4..7], t[8..11])
            float wx = t[0]*cx + t[1]*cy + t[2]*cz + t[3];
            float wy = t[4]*cx + t[5]*cy + t[6]*cz + t[7];
            float wz = t[8]*cx + t[9]*cy + t[10]*cz + t[11];
            float dx = wx - cam[0], dy = wy - cam[1], dz = wz - cam[2];
            Log("  inst %u mesh %d: trans=(%.0f,%.0f,%.0f) aabbC=(%.0f,%.0f,%.0f) "
                "ext=(%.0f,%.0f,%.0f) worldC dist=%.0f",
                i, inst.mesh, t[3], t[7], t[11], cx, cy, cz,
                r.aabbMax[0]-r.aabbMin[0], r.aabbMax[1]-r.aabbMin[1], r.aabbMax[2]-r.aabbMin[2],
                sqrtf(dx*dx + dy*dy + dz*dz));
        }
    }

    sInstanceCount = n;
    sStats.meshes = (uint32_t)sMeshes.size();
    sStats.instances = n;
    sStats.pendingPairs = (uint32_t)sPending.size();
    sStats.layoutsTracked = (uint32_t)sLayouts.size();
    sStats.poolVertsUsed = sPoolVertsUsed;
    sStats.poolIdxUsed = sPoolIdxUsed;

    if (n == 0) return false;
    sTlas.Build(cmd, n);
    return true;
}

bool DumpGeometry(const char* dir)
{
    if (!sPosPool || sPoolVertsUsed == 0)
        return false;

    // GPU readback of the used pool ranges (one-time CPU wait is fine here)
    const uint64_t posBytes = (uint64_t)sPoolVertsUsed * 12;
    const uint64_t idxBytes = (uint64_t)sPoolIdxUsed * 4;
    ID3D12Resource* rbPos = RtCore::CreateBuffer(posBytes, D3D12_HEAP_TYPE_READBACK,
                                                 D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rbIdx = RtCore::CreateBuffer(idxBytes, D3D12_HEAP_TYPE_READBACK,
                                                 D3D12_RESOURCE_STATE_COPY_DEST);
    if (!rbPos || !rbIdx)
    {
        if (rbPos) rbPos->Release();
        if (rbIdx) rbIdx->Release();
        return false;
    }
    ID3D12GraphicsCommandList4* cmd = RtCore::Begin();
    if (!cmd) { rbPos->Release(); rbIdx->Release(); return false; }
    cmd->CopyBufferRegion(rbPos, 0, sPosPool, 0, posBytes);
    cmd->CopyBufferRegion(rbIdx, 0, sIdxPool, 0, idxBytes);
    uint64_t v = RtCore::Submit();
    RtCore::CpuWait(v);

    char path[MAX_PATH];
    bool ok = true;
    void* mapped = nullptr;
    FILE* f = nullptr;

    snprintf(path, sizeof(path), "%s\\pos_pool.bin", dir);
    if (SUCCEEDED(rbPos->Map(0, nullptr, &mapped)) && fopen_s(&f, path, "wb") == 0)
    {
        fwrite(mapped, 1, (size_t)posBytes, f);
        fclose(f);
        rbPos->Unmap(0, nullptr);
    }
    else ok = false;

    snprintf(path, sizeof(path), "%s\\idx_pool.bin", dir);
    if (SUCCEEDED(rbIdx->Map(0, nullptr, &mapped)) && fopen_s(&f, path, "wb") == 0)
    {
        fwrite(mapped, 1, (size_t)idxBytes, f);
        fclose(f);
        rbIdx->Unmap(0, nullptr);
    }
    else ok = false;
    rbPos->Release();
    rbIdx->Release();

    // this frame's TLAS instances (same filter as RecordBuild)
    struct DumpInstance { float t[12]; uint32_t vbBase, vCount, ibBase, iCount; };
    std::vector<DumpInstance> insts;
    for (const auto& inst : sFrameInstances)
    {
        const MeshRecord& r = sMeshes[inst.mesh];
        if (r.state != MeshState::Built) continue;
        DumpInstance d;
        memcpy(d.t, inst.t, sizeof(d.t));
        d.vbBase = r.vbBase; d.vCount = r.vertexCount;
        d.ibBase = r.ibBase; d.iCount = r.indexCount;
        insts.push_back(d);
    }
    snprintf(path, sizeof(path), "%s\\instances.bin", dir);
    if (fopen_s(&f, path, "wb") == 0)
    {
        uint32_t count = (uint32_t)insts.size();
        fwrite(&count, 4, 1, f);
        fwrite(insts.data(), sizeof(DumpInstance), insts.size(), f);
        fclose(f);
    }
    else ok = false;

    // human-readable mesh table
    snprintf(path, sizeof(path), "%s\\meshes.txt", dir);
    if (fopen_s(&f, path, "w") == 0)
    {
        fprintf(f, "verts=%u idx=%u meshes=%zu instances=%zu flushes=%u\n",
                sPoolVertsUsed, sPoolIdxUsed, sMeshes.size(), insts.size(), sPoolFlushes);
        for (size_t i = 0; i < sMeshes.size(); ++i)
        {
            const MeshRecord& r = sMeshes[i];
            fprintf(f, "mesh %zu state=%d v=%u i=%u aabb=(%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n",
                    i, (int)r.state, r.vertexCount, r.indexCount,
                    r.aabbMin[0], r.aabbMin[1], r.aabbMin[2],
                    r.aabbMax[0], r.aabbMax[1], r.aabbMax[2]);
        }
        fclose(f);
    }
    Log("RtScene: dumped %zu instances, %u verts, %u idx to %s", insts.size(),
        sPoolVertsUsed, sPoolIdxUsed, dir);
    return ok;
}

ID3D12Resource* Tlas() { return sTlas.result; }
ID3D12Resource* PosPool() { return sPosPool; }
ID3D12Resource* IdxPool() { return sIdxPool; }
ID3D12Resource* InstInfoBuffer(uint32_t frameSlot) { return sInstInfo[frameSlot % kFrameSlots]; }
uint32_t InstanceCount() { return sInstanceCount; }
Stats GetStats() { return sStats; }

void Flush()
{
    // churn fingerprint (logged before the clears): rangeKeys >> pairs means
    // offset/baseVertex churn on stable buffers; pairs growing per frame
    // means buffer-pointer churn
    Log("RtScene: flush (#%u) — rangeKeys=%zu pairsSeen=%zu dedupHits=%u meshes=%zu",
        sPoolFlushes, sMeshIndex.size(), sPairsSeen.size(), sDedupHits, sMeshes.size());

    RtCore::CpuWait(RtCore::NextFenceValue() - 1);   // drain in-flight GPU work
    for (auto& m : sMeshes)
        if (m.blas) { m.blas->Release(); m.blas = nullptr; }
    sMeshes.clear();
    sMeshIndex.clear();
    sContentIndex.clear();
    for (auto& p : sPending)
    {
        if (p.vbStaging) p.vbStaging->Release();
        if (p.ibStaging) p.ibStaging->Release();
    }
    sPending.clear();
    sFrameInstances.clear();
    sPoolVertsUsed = 0;
    sPoolIdxUsed = 0;
    sPoolFullLogged = false;
    sPlacementDumped = false;   // re-dump after every rebuild
    sInstanceCount = 0;
    sStats.blasBuilt = 0;
}

void Shutdown()
{
    Flush();
    for (uint32_t i = 0; i < kFrameSlots; ++i)
    {
        if (sInstInfo[i] && sInstInfoMapped[i]) sInstInfo[i]->Unmap(0, nullptr);
        sInstInfoMapped[i] = nullptr;
        if (sInstInfo[i]) { sInstInfo[i]->Release(); sInstInfo[i] = nullptr; }
    }
    sTlas.Release();
    if (sPosPool) { sPosPool->Release(); sPosPool = nullptr; }
    if (sIdxPool) { sIdxPool->Release(); sIdxPool = nullptr; }
    for (auto& kv : sLayouts)
        kv.first->Release();
    sLayouts.clear();
}

} // namespace RtScene
