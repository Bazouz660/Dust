#include "SurveyRecorder.h"
#include "Survey.h"
#include "D3D11Hook.h"
#include "DustLog.h"
#include <windows.h>
#include <cstring>

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while(0)

namespace SurveyRecorder
{

// ==================== State ====================

static SurveyFrameData       sCurrentFrame;
static uint32_t              sDrawIndex = 0;

// Timing
static LARGE_INTEGER sFreq;
static LARGE_INTEGER sFrameStartTime;
static bool          sFreqInit = false;

// Breadcrumb for crash diagnosis
static char sBreadcrumb[128] = {};

// Staging buffer pool for CB readback (detail level 2+)
static const int     STAGING_POOL_SIZE = 12;
static ID3D11Buffer* sStagingBuffers[STAGING_POOL_SIZE] = {};
static uint32_t      sStagingSizes[STAGING_POOL_SIZE] = {};

// Shader source tracking
// Key: bytecode hash -> source info (populated by D3DCompile hook)
static std::unordered_map<uint64_t, ShaderSourceInfo> sBytecodeToSource;
// Key: shader COM pointer -> source info (populated when CreateShader maps bytecode to pointer)
static std::unordered_map<uint64_t, ShaderSourceInfo> sShaderPtrToSource;

// ==================== Helpers ====================

static void SetBreadcrumb(const char* method, uint32_t drawIdx)
{
    snprintf(sBreadcrumb, sizeof(sBreadcrumb), "SURVEY: %s @ draw %u", method, drawIdx);
}

static uint64_t HashBytecode(const void* data, SIZE_T size)
{
    // FNV-1a 64-bit
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t* p = (const uint8_t*)data;
    for (SIZE_T i = 0; i < size; i++)
    {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static double GetElapsedMs(LARGE_INTEGER start)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)sFreq.QuadPart;
}

static void EnsureQPC()
{
    if (!sFreqInit)
    {
        QueryPerformanceFrequency(&sFreq);
        sFreqInit = true;
    }
}

// ==================== Staging buffer management ====================

static ID3D11Buffer* GetStagingBuffer(ID3D11Device* device, int poolIndex, uint32_t requiredSize)
{
    if (poolIndex < 0 || poolIndex >= STAGING_POOL_SIZE)
        return nullptr;

    // Reuse if big enough
    if (sStagingBuffers[poolIndex] && sStagingSizes[poolIndex] >= requiredSize)
        return sStagingBuffers[poolIndex];

    // Release old
    SAFE_RELEASE(sStagingBuffers[poolIndex]);

    // Cap to 512 bytes for safety
    uint32_t allocSize = (requiredSize > 512) ? 512 : requiredSize;
    if (allocSize == 0) allocSize = 512;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = allocSize;
    desc.Usage           = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags  = D3D11_CPU_ACCESS_READ;
    desc.BindFlags       = 0;
    desc.MiscFlags       = 0;

    HRESULT hr = device->CreateBuffer(&desc, nullptr, &sStagingBuffers[poolIndex]);
    if (SUCCEEDED(hr))
    {
        sStagingSizes[poolIndex] = allocSize;
        return sStagingBuffers[poolIndex];
    }

    return nullptr;
}

// ==================== State snapshot (SEH-protected) ====================

// Query failure bitmask flags
enum QueryFlag : uint32_t
{
    QF_PS           = 1 << 0,
    QF_VS           = 1 << 1,
    QF_GS           = 1 << 2,
    QF_HS           = 1 << 3,
    QF_DS           = 1 << 4,
    QF_IA_TOPO      = 1 << 5,
    QF_IA_LAYOUT    = 1 << 6,
    QF_OM_RT        = 1 << 7,
    QF_OM_BLEND     = 1 << 8,
    QF_OM_DS        = 1 << 9,
    QF_RS           = 1 << 10,
    QF_PS_SRV       = 1 << 11,
    QF_PS_CB        = 1 << 12,
    QF_VS_SRV       = 1 << 13,
    QF_VS_CB        = 1 << 14,
    QF_VIEWPORT     = 1 << 15,
    QF_IA_VB        = 1 << 16,
    QF_IA_IB        = 1 << 17,
};

static void GetSRVDetails(ID3D11ShaderResourceView* srv, SurveySRVInfo& info, int detailLevel)
{
    info.srvPtr = (uint64_t)srv;

    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    srv->GetDesc(&desc);
    info.format    = desc.Format;
    info.dimension = desc.ViewDimension;

    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (res)
    {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
        {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            info.width  = td.Width;
            info.height = td.Height;
            if (detailLevel >= Survey::DETAIL_DEEP)
            {
                info.mipLevels = td.MipLevels;
                info.arraySize = td.ArraySize;
            }
            tex->Release();
        }
        res->Release();
    }
}

static void ReadCBData(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, SurveyCBInfo& info, int poolIdx)
{
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return;

    ID3D11Buffer* staging = GetStagingBuffer(device, poolIdx, info.size);
    device->Release();
    if (!staging) return;

    // Copy the smaller of CB size or 512 bytes
    D3D11_BOX box = {};
    box.left   = 0;
    box.right  = (info.size > 512) ? 512 : info.size;
    box.top    = 0;
    box.bottom = 1;
    box.front  = 0;
    box.back   = 1;

    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, cb, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        uint32_t copySize = (info.size > 512) ? 512 : info.size;
        memcpy(info.data, mapped.pData, copySize);
        info.hasData = true;
        ctx->Unmap(staging, 0);
    }
}

static void SnapshotState(ID3D11DeviceContext* ctx, SurveyDrawEvent& ev)
{
    int detail = Survey::GetDetailLevel();

    // Device health check
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return;
    HRESULT removeReason = device->GetDeviceRemovedReason();
    device->Release();
    if (removeReason != S_OK)
    {
        Log("SURVEY: Device removed (0x%08X), skipping state snapshot", removeReason);
        return;
    }

    // --- Minimal (level 0): RT info ---

    // OM Render Targets
    SetBreadcrumb("OMGetRenderTargets", ev.drawIndex);
    __try
    {
        ID3D11RenderTargetView* rtvs[8] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(8, rtvs, &dsv);

        for (int i = 0; i < 8; i++)
        {
            if (!rtvs[i]) continue;
            ev.numRenderTargets = i + 1;
            ev.renderTargets[i].rtvPtr = (uint64_t)rtvs[i];

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
            rtvs[i]->GetDesc(&rtvDesc);
            ev.renderTargets[i].format = rtvDesc.Format;

            ID3D11Resource* res = nullptr;
            rtvs[i]->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
                {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    ev.renderTargets[i].width  = td.Width;
                    ev.renderTargets[i].height = td.Height;
                    tex->Release();
                }
                res->Release();
            }
            rtvs[i]->Release();
        }

        if (dsv)
        {
            ev.dsvPtr = (uint64_t)dsv;
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
            dsv->GetDesc(&dsvDesc);
            ev.dsvFormat = dsvDesc.Format;

            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
                {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    ev.dsvWidth  = td.Width;
                    ev.dsvHeight = td.Height;
                    tex->Release();
                }
                res->Release();
            }
            dsv->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_OM_RT;
        Log("SURVEY: SEH in OMGetRenderTargets at draw %u", ev.drawIndex);
    }

    if (detail < Survey::DETAIL_STANDARD)
        return;

    // --- Standard (level 1): shaders, SRVs, CBs, IA, OM states ---

    // PS
    SetBreadcrumb("PSGetShader", ev.drawIndex);
    __try
    {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ev.psPtr = (uint64_t)ps;
        SAFE_RELEASE(ps);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_PS;
        Log("SURVEY: SEH in PSGetShader at draw %u", ev.drawIndex);
    }

    // VS
    SetBreadcrumb("VSGetShader", ev.drawIndex);
    __try
    {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ev.vsPtr = (uint64_t)vs;
        SAFE_RELEASE(vs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_VS;
        Log("SURVEY: SEH in VSGetShader at draw %u", ev.drawIndex);
    }

    // GS
    SetBreadcrumb("GSGetShader", ev.drawIndex);
    __try
    {
        ID3D11GeometryShader* gs = nullptr;
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ev.gsPtr = (uint64_t)gs;
        SAFE_RELEASE(gs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_GS;
    }

    // HS
    SetBreadcrumb("HSGetShader", ev.drawIndex);
    __try
    {
        ID3D11HullShader* hs = nullptr;
        ctx->HSGetShader(&hs, nullptr, nullptr);
        ev.hsPtr = (uint64_t)hs;
        SAFE_RELEASE(hs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_HS;
    }

    // DS
    SetBreadcrumb("DSGetShader", ev.drawIndex);
    __try
    {
        ID3D11DomainShader* ds = nullptr;
        ctx->DSGetShader(&ds, nullptr, nullptr);
        ev.dsPtr = (uint64_t)ds;
        SAFE_RELEASE(ds);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_DS;
    }

    // IA topology
    SetBreadcrumb("IAGetPrimitiveTopology", ev.drawIndex);
    __try
    {
        ctx->IAGetPrimitiveTopology(&ev.topology);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_IA_TOPO;
    }

    // IA input layout
    SetBreadcrumb("IAGetInputLayout", ev.drawIndex);
    __try
    {
        ID3D11InputLayout* layout = nullptr;
        ctx->IAGetInputLayout(&layout);
        ev.inputLayoutPtr = (uint64_t)layout;
        SAFE_RELEASE(layout);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_IA_LAYOUT;
    }

    // OM blend state
    SetBreadcrumb("OMGetBlendState", ev.drawIndex);
    __try
    {
        ID3D11BlendState* blend = nullptr;
        float factor[4];
        UINT mask;
        ctx->OMGetBlendState(&blend, factor, &mask);
        ev.blendStatePtr = (uint64_t)blend;
        SAFE_RELEASE(blend);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_OM_BLEND;
    }

    // OM depth-stencil state
    SetBreadcrumb("OMGetDepthStencilState", ev.drawIndex);
    __try
    {
        ID3D11DepthStencilState* dss = nullptr;
        UINT ref;
        ctx->OMGetDepthStencilState(&dss, &ref);
        ev.depthStencilStatePtr = (uint64_t)dss;
        SAFE_RELEASE(dss);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_OM_DS;
    }

    // RS rasterizer state
    SetBreadcrumb("RSGetState", ev.drawIndex);
    __try
    {
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        ev.rasterizerStatePtr = (uint64_t)rs;
        SAFE_RELEASE(rs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_RS;
    }

    // PS SRVs (slots 0-15)
    SetBreadcrumb("PSGetShaderResources", ev.drawIndex);
    __try
    {
        ID3D11ShaderResourceView* srvs[16] = {};
        ctx->PSGetShaderResources(0, 16, srvs);
        for (int i = 0; i < 16; i++)
        {
            if (!srvs[i]) continue;
            ev.numPSSRVs = i + 1;
            GetSRVDetails(srvs[i], ev.psSRVs[i], detail);
            srvs[i]->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_PS_SRV;
        Log("SURVEY: SEH in PSGetShaderResources at draw %u", ev.drawIndex);
    }

    // VS SRVs (slots 0-3)
    SetBreadcrumb("VSGetShaderResources", ev.drawIndex);
    __try
    {
        ID3D11ShaderResourceView* srvs[4] = {};
        ctx->VSGetShaderResources(0, 4, srvs);
        for (int i = 0; i < 4; i++)
        {
            if (!srvs[i]) continue;
            ev.numVSSRVs = i + 1;
            GetSRVDetails(srvs[i], ev.vsSRVs[i], detail);
            srvs[i]->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_VS_SRV;
    }

    // PS constant buffers (slots 0-7)
    SetBreadcrumb("PSGetConstantBuffers", ev.drawIndex);
    __try
    {
        ID3D11Buffer* cbs[8] = {};
        ctx->PSGetConstantBuffers(0, 8, cbs);
        for (int i = 0; i < 8; i++)
        {
            if (!cbs[i]) continue;
            ev.psCBs[i].cbPtr = (uint64_t)cbs[i];
            D3D11_BUFFER_DESC bd;
            cbs[i]->GetDesc(&bd);
            ev.psCBs[i].size = bd.ByteWidth;

            // Deep: read CB contents
            if (detail >= Survey::DETAIL_DEEP && bd.ByteWidth > 0)
                ReadCBData(ctx, cbs[i], ev.psCBs[i], i);

            cbs[i]->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_PS_CB;
        Log("SURVEY: SEH in PSGetConstantBuffers at draw %u", ev.drawIndex);
    }

    // VS constant buffers (slots 0-3)
    SetBreadcrumb("VSGetConstantBuffers", ev.drawIndex);
    __try
    {
        ID3D11Buffer* cbs[4] = {};
        ctx->VSGetConstantBuffers(0, 4, cbs);
        for (int i = 0; i < 4; i++)
        {
            if (!cbs[i]) continue;
            ev.vsCBs[i].cbPtr = (uint64_t)cbs[i];
            D3D11_BUFFER_DESC bd;
            cbs[i]->GetDesc(&bd);
            ev.vsCBs[i].size = bd.ByteWidth;

            if (detail >= Survey::DETAIL_DEEP && bd.ByteWidth > 0)
                ReadCBData(ctx, cbs[i], ev.vsCBs[i], 8 + i);

            cbs[i]->Release();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_VS_CB;
    }

    if (detail < Survey::DETAIL_FULL)
        return;

    // --- Full (level 3): viewport, VB/IB metadata ---

    // Viewport
    SetBreadcrumb("RSGetViewports", ev.drawIndex);
    __try
    {
        D3D11_VIEWPORT vp[1];
        UINT numVP = 1;
        ctx->RSGetViewports(&numVP, vp);
        if (numVP > 0)
        {
            ev.vpX = vp[0].TopLeftX;
            ev.vpY = vp[0].TopLeftY;
            ev.vpW = vp[0].Width;
            ev.vpH = vp[0].Height;
            ev.vpMinDepth = vp[0].MinDepth;
            ev.vpMaxDepth = vp[0].MaxDepth;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_VIEWPORT;
    }

    // Vertex buffer (slot 0 only)
    SetBreadcrumb("IAGetVertexBuffers", ev.drawIndex);
    __try
    {
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, offset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        ev.vertexStride = stride;
        SAFE_RELEASE(vb);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_IA_VB;
    }

    // Index buffer
    SetBreadcrumb("IAGetIndexBuffer", ev.drawIndex);
    __try
    {
        ID3D11Buffer* ib = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        UINT offset = 0;
        ctx->IAGetIndexBuffer(&ib, &fmt, &offset);
        ev.indexFormat = fmt;
        ev.indexBufferOffset = offset;
        SAFE_RELEASE(ib);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ev.failedQueries |= QF_IA_IB;
    }
}

// ==================== Draw event recording ====================

static void RecordDraw(ID3D11DeviceContext* ctx, SurveyDrawEvent::DrawType type,
                       UINT count, UINT instances, UINT startIdx, INT baseVtx, UINT startInst)
{
    SurveyDrawEvent ev = {};
    ev.drawIndex     = sDrawIndex++;
    ev.type          = type;
    ev.vertexOrIndexCount = count;
    ev.instanceCount = instances;
    ev.startIndex    = startIdx;
    ev.baseVertex    = baseVtx;
    ev.startInstanceLocation = startInst;

    SnapshotState(ctx, ev);

    sCurrentFrame.draws.push_back(std::move(ev));
}

void OnDraw(ID3D11DeviceContext* ctx, UINT VertexCount, UINT StartVertexLocation)
{
    sCurrentFrame.totalDrawCalls++;
    RecordDraw(ctx, SurveyDrawEvent::DRAW, VertexCount, 1, StartVertexLocation, 0, 0);
}

void OnDrawIndexed(ID3D11DeviceContext* ctx, UINT IndexCount,
                   UINT StartIndexLocation, INT BaseVertexLocation)
{
    sCurrentFrame.totalDrawIndexedCalls++;
    RecordDraw(ctx, SurveyDrawEvent::DRAW_INDEXED, IndexCount, 1,
               StartIndexLocation, BaseVertexLocation, 0);
}

void OnDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT IndexCountPerInstance,
                            UINT InstanceCount, UINT StartIndexLocation,
                            INT BaseVertexLocation, UINT StartInstanceLocation)
{
    sCurrentFrame.totalDrawInstCalls++;
    RecordDraw(ctx, SurveyDrawEvent::DRAW_INDEXED_INSTANCED, IndexCountPerInstance,
               InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

// ==================== Frame lifecycle ====================

SurveyFrameData OnEndFrame()
{
    EnsureQPC();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    sCurrentFrame.captureTimeMs = (float)GetElapsedMs(sFrameStartTime);
    sCurrentFrame.frameIndex = Survey::CurrentFrame();

    // Move out the completed frame
    SurveyFrameData completed = std::move(sCurrentFrame);

    // Reset for next frame
    sCurrentFrame = SurveyFrameData{};
    sDrawIndex = 0;
    QueryPerformanceCounter(&sFrameStartTime);

    return completed;
}

void Reset()
{
    sCurrentFrame = SurveyFrameData{};
    sDrawIndex = 0;
    EnsureQPC();
    QueryPerformanceCounter(&sFrameStartTime);
    memset(sBreadcrumb, 0, sizeof(sBreadcrumb));
    Log("SURVEY: Recorder reset");
}

void Shutdown()
{
    for (int i = 0; i < STAGING_POOL_SIZE; i++)
        SAFE_RELEASE(sStagingBuffers[i]);
}

// ==================== Shader source tracking ====================

void OnShaderCompiled(const void* pSrcData, SIZE_T srcSize,
                      const char* entryPoint, const char* target,
                      const char* sourceName,
                      const char* const* defineNames,
                      const void* bytecode, SIZE_T bytecodeSize)
{
    if (!bytecode || bytecodeSize == 0)
        return;

    uint64_t hash = HashBytecode(bytecode, bytecodeSize);

    ShaderSourceInfo info;
    if (pSrcData && srcSize > 0)
        info.source = std::string((const char*)pSrcData, srcSize);
    info.entryPoint = entryPoint ? entryPoint : "";
    info.target     = target ? target : "";
    info.sourceName = sourceName ? sourceName : "";
    if (defineNames)
        for (const char* const* p = defineNames; *p; ++p)
            info.defines.emplace_back(*p);

    sBytecodeToSource[hash] = std::move(info);
}

void OnPixelShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                          ID3D11PixelShader* shader)
{
    if (!bytecode || bytecodeSize == 0 || !shader)
        return;

    uint64_t hash = HashBytecode(bytecode, bytecodeSize);
    auto it = sBytecodeToSource.find(hash);
    if (it != sBytecodeToSource.end())
        sShaderPtrToSource[(uint64_t)shader] = it->second;
}

void OnVertexShaderCreated(const void* bytecode, SIZE_T bytecodeSize,
                           ID3D11VertexShader* shader)
{
    if (!bytecode || bytecodeSize == 0 || !shader)
        return;

    uint64_t hash = HashBytecode(bytecode, bytecodeSize);
    auto it = sBytecodeToSource.find(hash);
    if (it != sBytecodeToSource.end())
        sShaderPtrToSource[(uint64_t)shader] = it->second;
}

const ShaderSourceInfo* GetShaderSource(uint64_t shaderPtr)
{
    auto it = sShaderPtrToSource.find(shaderPtr);
    return (it != sShaderPtrToSource.end()) ? &it->second : nullptr;
}

const std::unordered_map<uint64_t, ShaderSourceInfo>& GetShaderMap()
{
    return sShaderPtrToSource;
}

} // namespace SurveyRecorder
