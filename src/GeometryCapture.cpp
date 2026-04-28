#include "GeometryCapture.h"
#include "DustAPI.h"
#include "DustLog.h"
#include "ShaderDatabase.h"
#include <d3d11_1.h>
#include <vector>
#include <unordered_map>
#include <string>

namespace GeometryCapture
{

static std::vector<CapturedDraw> sCaptures;
static bool sInGBufferPass = false;
static UINT sExpectedWidth  = 0;
static UINT sExpectedHeight = 0;
static ID3D11Device* sCachedDevice = nullptr;

static uint32_t sFramesCaptured = 0;
static uint32_t sCaptureFlags = 0;

// Staging buffer pool — each buffer has its own size (must match the CB it copies
// from, since D3D11 CopyResource requires identical ByteWidth for buffers).
// Buffers are reused across frames: on frame reset we set sStagingPoolUsed=0,
// on acquire we scan the unused portion for a size match before allocating.
struct StagingEntry
{
    ID3D11Buffer* buffer;
    uint32_t      size;
};
static std::vector<StagingEntry> sStagingPool;
static uint32_t sStagingPoolUsed = 0;

static ID3D11Buffer* AcquireStagingBuffer(ID3D11Device* device, uint32_t requiredSize)
{
    // Search unused portion for a buffer of the right size
    for (uint32_t i = sStagingPoolUsed; i < (uint32_t)sStagingPool.size(); i++)
    {
        if (sStagingPool[i].size == requiredSize)
        {
            if (i != sStagingPoolUsed)
                std::swap(sStagingPool[i], sStagingPool[sStagingPoolUsed]);
            return sStagingPool[sStagingPoolUsed++].buffer;
        }
    }

    // Allocate new
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = requiredSize;
    desc.Usage           = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags  = D3D11_CPU_ACCESS_READ;
    desc.BindFlags       = 0;

    ID3D11Buffer* buf = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buf);
    if (FAILED(hr) || !buf)
        return nullptr;

    sStagingPool.push_back({ buf, requiredSize });
    // The new entry is at the end — swap it into the used region
    if (sStagingPool.size() - 1 != sStagingPoolUsed)
        std::swap(sStagingPool.back(), sStagingPool[sStagingPoolUsed]);
    return sStagingPool[sStagingPoolUsed++].buffer;
}

static void ReleaseCaptures()
{
    for (auto& draw : sCaptures)
    {
        for (UINT i = 0; i < CapturedDraw::MAX_VB_SLOTS; i++)
        {
            if (draw.vertexBuffers[i]) { draw.vertexBuffers[i]->Release(); draw.vertexBuffers[i] = nullptr; }
        }
        if (draw.indexBuffer)  { draw.indexBuffer->Release();  draw.indexBuffer = nullptr; }
        if (draw.inputLayout)  { draw.inputLayout->Release();  draw.inputLayout = nullptr; }
        if (draw.vs)           { draw.vs->Release();           draw.vs = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
        {
            if (draw.vsCBs[i]) { draw.vsCBs[i]->Release(); draw.vsCBs[i] = nullptr; }
        }
        if (draw.ps) { draw.ps->Release(); draw.ps = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_PS_CBS; i++)
        {
            if (draw.psCBs[i]) { draw.psCBs[i]->Release(); draw.psCBs[i] = nullptr; }
        }
        for (UINT i = 0; i < CapturedDraw::MAX_PS_SRVS; i++)
        {
            if (draw.psSRVs[i]) { draw.psSRVs[i]->Release(); draw.psSRVs[i] = nullptr; }
        }
        for (UINT i = 0; i < CapturedDraw::MAX_PS_SAMPLERS; i++)
        {
            if (draw.psSamplers[i]) { draw.psSamplers[i]->Release(); draw.psSamplers[i] = nullptr; }
        }
        draw.cbStagingCopy = nullptr;
    }
    sCaptures.clear();
    sStagingPoolUsed = 0;
}

// Cache: once we've identified the GBuffer RTVs, skip expensive COM queries
// on repeat calls with the same pointers. Invalidated on resolution change.
static ID3D11RenderTargetView* sCachedGBufferRTVs[3] = {};
static ID3D11DepthStencilView* sCachedGBufferDSV = nullptr;
static bool sCacheValid = false;

static void LogUniqueRTVConfig(UINT numViews,
                               ID3D11RenderTargetView* const* ppRTVs,
                               ID3D11DepthStencilView* pDSV,
                               bool isGBuffer)
{
    static std::unordered_map<uint64_t, bool> sSeenConfigs;
    uint64_t hash = ((uint64_t)numViews << 56);
    UINT formats[8] = {}, widths[8] = {}, heights[8] = {};
    for (UINT i = 0; i < numViews && i < 8; i++)
    {
        if (ppRTVs && ppRTVs[i])
        {
            ID3D11Resource* res = nullptr;
            ppRTVs[i]->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
                {
                    D3D11_TEXTURE2D_DESC td;
                    tex->GetDesc(&td);
                    formats[i] = td.Format; widths[i] = td.Width; heights[i] = td.Height;
                    tex->Release();
                }
                res->Release();
            }
        }
        hash ^= ((uint64_t)formats[i] << (i*8)) ^ ((uint64_t)widths[i] << (i*4)) ^ ((uint64_t)heights[i] << (i*4));
    }
    if (sSeenConfigs.find(hash) == sSeenConfigs.end())
    {
        sSeenConfigs[hash] = true;
        char buf[512] = {};
        int off = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                              "GeometryCapture: RTV config — numViews=%u%s [",
                              numViews, isGBuffer ? " GBUFFER" : "");
        for (UINT i = 0; i < numViews && i < 8; i++)
        {
            int n = _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE,
                                "%s(fmt=%u %ux%u)", (i ? " " : ""),
                                formats[i], widths[i], heights[i]);
            if (n > 0) off += n;
        }
        Log("%s] dsv=%p", buf, (void*)pDSV);
    }
}

static bool IsGBufferConfigImpl(UINT numViews,
                                ID3D11RenderTargetView* const* ppRTVs,
                                ID3D11DepthStencilView* pDSV)
{
    if (numViews != 3 || !ppRTVs || !pDSV)
        return false;

    if (sExpectedWidth == 0 || sExpectedHeight == 0)
        return false;

    for (UINT i = 0; i < 3; i++)
    {
        if (!ppRTVs[i])
            return false;
    }

    // Fast path: if pointers match the cached GBuffer RTVs, skip COM queries
    if (sCacheValid &&
        ppRTVs[0] == sCachedGBufferRTVs[0] &&
        ppRTVs[1] == sCachedGBufferRTVs[1] &&
        ppRTVs[2] == sCachedGBufferRTVs[2] &&
        pDSV == sCachedGBufferDSV)
    {
        return true;
    }

    for (UINT i = 0; i < 3; i++)
    {
        ID3D11Resource* res = nullptr;
        ppRTVs[i]->GetResource(&res);
        if (!res)
            return false;

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        res->Release();

        if (FAILED(hr) || !tex)
            return false;

        D3D11_TEXTURE2D_DESC texDesc;
        tex->GetDesc(&texDesc);
        tex->Release();

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
        ppRTVs[i]->GetDesc(&rtvDesc);
        DXGI_FORMAT format = (rtvDesc.Format != DXGI_FORMAT_UNKNOWN)
                             ? rtvDesc.Format : texDesc.Format;

        if (format != GBUFFER_COLOR_FORMATS[i])
            return false;

        if (texDesc.Width != sExpectedWidth || texDesc.Height != sExpectedHeight)
            return false;
    }

    // Cache these pointers for fast-path on subsequent calls
    for (UINT i = 0; i < 3; i++)
        sCachedGBufferRTVs[i] = ppRTVs[i];
    sCachedGBufferDSV = pDSV;
    sCacheValid = true;

    return true;
}

static bool IsGBufferConfig(UINT numViews,
                            ID3D11RenderTargetView* const* ppRTVs,
                            ID3D11DepthStencilView* pDSV)
{
    bool result = IsGBufferConfigImpl(numViews, ppRTVs, pDSV);
    LogUniqueRTVConfig(numViews, ppRTVs, pDSV, result);
    return result;
}

void OnOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numViews,
                          ID3D11RenderTargetView* const* ppRTVs,
                          ID3D11DepthStencilView* pDSV)
{
    bool isGBuffer = IsGBufferConfig(numViews, ppRTVs, pDSV);
    OnOMSetRenderTargetsWithResult(isGBuffer);
}

// Per-frame counters
static uint32_t sFrameDrawsTotal      = 0;  // every DrawIndexed seen this frame
static uint32_t sFrameDrawsCaptured   = 0;  // captured (inside GBuffer pass)
static uint32_t sFrameDrawsSkipped    = 0;  // dropped (not in GBuffer pass)
static uint32_t sFrameGBufferToggles  = 0;  // OFF→ON transitions this frame

void OnOMSetRenderTargetsWithResult(bool isGBuffer)
{
    bool wasInGBuffer = sInGBufferPass;
    sInGBufferPass = isGBuffer;

    if (!wasInGBuffer && sInGBufferPass)
    {
        sFrameGBufferToggles++;
        if (sFramesCaptured < 3)
            Log("GeometryCapture: GBuffer pass detected (%ux%u)", sExpectedWidth, sExpectedHeight);
    }
    else if (wasInGBuffer && !sInGBufferPass)
    {
        if (sFramesCaptured < 3)
            Log("GeometryCapture: GBuffer pass ended, captured %u draws",
                (uint32_t)sCaptures.size());
    }
}

// Diagnostic: cumulative set of (sourceName, vbSlotBits, instanced) combos seen.
// Logged the FIRST time each unique combo appears so we can spot e.g. an "objects"
// variant binding VB slot 2 that would have been silently dropped pre-fix.
static std::unordered_map<uint64_t, bool> sLoggedSlotCombos;

static void CaptureDrawState(ID3D11DeviceContext* ctx, CapturedDraw& draw)
{
    // IA state
    ctx->IAGetInputLayout(&draw.inputLayout);
    ctx->IAGetPrimitiveTopology(&draw.topology);
    ctx->IAGetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, draw.vertexBuffers,
                            draw.vbStrides, draw.vbOffsets);
    ctx->IAGetIndexBuffer(&draw.indexBuffer, &draw.indexFormat, &draw.ibOffset);

    // VS
    ctx->VSGetShader(&draw.vs, nullptr, nullptr);
    ctx->VSGetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, draw.vsCBs);

    // PS pointer (always — cheap, enables shader identification)
    ctx->PSGetShader(&draw.ps, nullptr, nullptr);

    // Diagnostic — log first occurrence of each unique (vs, vbSlotBits, instanced) combo.
    // Unique combos are bounded by (#VSes × 16 × 2) so this can't spam unboundedly.
    if (draw.vs)
    {
        uint32_t slotBits = 0;
        for (UINT i = 0; i < CapturedDraw::MAX_VB_SLOTS; i++)
            if (draw.vertexBuffers[i]) slotBits |= (1u << i);
        bool instanced = (draw.instanceCount > 1);
        uint64_t key = ((uint64_t)(uintptr_t)draw.vs) ^
                       ((uint64_t)slotBits << 1) ^
                       ((uint64_t)instanced << 33);
        if (sLoggedSlotCombos.find(key) == sLoggedSlotCombos.end())
        {
            sLoggedSlotCombos[key] = true;
            const char* nm = ShaderDatabase::GetSourceName((uint64_t)draw.vs);
            Log("GeometryCapture: VS '%s' (%p) — slotBits=0x%X instanced=%d strides=[%u,%u,%u,%u,%u,%u,%u,%u]",
                nm ? nm : "<unclassified>", draw.vs, slotBits, (int)instanced,
                draw.vbStrides[0], draw.vbStrides[1], draw.vbStrides[2], draw.vbStrides[3],
                draw.vbStrides[4], draw.vbStrides[5], draw.vbStrides[6], draw.vbStrides[7]);
        }
    }

    // PS resources (opt-in — adds ~0.4ms/frame for 200 draws)
    if (sCaptureFlags & DUST_CAPTURE_PS_RESOURCES)
    {
        ctx->PSGetConstantBuffers(0, CapturedDraw::MAX_PS_CBS, draw.psCBs);
        ctx->PSGetShaderResources(0, CapturedDraw::MAX_PS_SRVS, draw.psSRVs);
        ctx->PSGetSamplers(0, CapturedDraw::MAX_PS_SAMPLERS, draw.psSamplers);
    }

    // Look up shader metadata
    draw.vsMetadata = ShaderMetadata::GetVSInfo(draw.vs);

    // For classified draws, copy the CB containing the clip matrix to a staging buffer.
    // By replay time (POST_LIGHTING), the GPU will have completed these copies.
    if (draw.vsMetadata && draw.vsMetadata->transformType != VSTransformType::UNKNOWN &&
        sCachedDevice)
    {
        uint32_t slot = draw.vsMetadata->cbSlot;
        if (slot < CapturedDraw::MAX_VS_CBS && draw.vsCBs[slot])
        {
            // Diagnostic: log unique (first,num) combos from VSGetConstantBuffers1
            // for the matrix CB slot. If `first` varies across draws, Kenshi is using
            // a shared CB with per-draw offsets and our replay is reading the wrong
            // chunk of memory.
            {
                ID3D11DeviceContext1* ctx1 = nullptr;
                if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)))
                {
                    ID3D11Buffer* cbs[1] = {};
                    UINT firstCB[1] = {};
                    UINT numCB[1] = {};
                    ctx1->VSGetConstantBuffers1(slot, 1, cbs, firstCB, numCB);
                    static std::unordered_map<uint64_t, bool> sOffsetCombos;
                    uint64_t key = ((uint64_t)slot << 56) ^ ((uint64_t)firstCB[0] << 24) ^ (uint64_t)numCB[0];
                    if (sOffsetCombos.find(key) == sOffsetCombos.end())
                    {
                        sOffsetCombos[key] = true;
                        Log("GeometryCapture: VSGetConstantBuffers1 slot %u — first=%u num=%u (vs %p)",
                            slot, firstCB[0], numCB[0], draw.vs);
                    }
                    if (cbs[0]) cbs[0]->Release();
                    ctx1->Release();
                }
            }

            // Use the SOURCE CB's actual ByteWidth, not cbTotalSize from reflection.
            // CopyResource silently fails if source and dest sizes differ — and OGRE
            // can pad the CB resource above the reflected cbuffer size.
            D3D11_BUFFER_DESC srcDesc = {};
            draw.vsCBs[slot]->GetDesc(&srcDesc);
            uint32_t cbSize = srcDesc.ByteWidth;

            // Diagnostic: log mismatches between reflection-reported size and actual
            // resource size. If we see these, our pre-fix stage copies were silently
            // failing for these draws.
            static std::unordered_map<uint64_t, bool> sLoggedSizeMismatch;
            uint32_t reflectedSize = draw.vsMetadata->cbTotalSize;
            if (cbSize != reflectedSize && draw.vs)
            {
                uint64_t key = (uint64_t)(uintptr_t)draw.vs;
                if (sLoggedSizeMismatch.find(key) == sLoggedSizeMismatch.end())
                {
                    sLoggedSizeMismatch[key] = true;
                    const char* nm = ShaderDatabase::GetSourceName((uint64_t)draw.vs);
                    Log("GeometryCapture: VS '%s' (%p) CB size MISMATCH — reflection=%u actual=%u",
                        nm ? nm : "<unclassified>", draw.vs, reflectedSize, cbSize);
                }
            }

            ID3D11Buffer* staging = AcquireStagingBuffer(sCachedDevice, cbSize);
            if (staging)
            {
                ctx->CopyResource(staging, draw.vsCBs[slot]);
                draw.cbStagingCopy = staging;
                draw.cbStagingSize = cbSize;
            }
        }
    }
}

static void CaptureDraw(ID3D11DeviceContext* ctx, UINT indexCount,
                        UINT startIndex, INT baseVertex,
                        UINT instanceCount, UINT startInstance)
{
    sFrameDrawsTotal++;
    if (!sInGBufferPass)
    {
        sFrameDrawsSkipped++;
        return;
    }
    sFrameDrawsCaptured++;

    CapturedDraw draw;
    draw.indexCount            = indexCount;
    draw.startIndexLocation    = startIndex;
    draw.baseVertexLocation    = baseVertex;
    draw.instanceCount         = instanceCount;
    draw.startInstanceLocation = startInstance;

    CaptureDrawState(ctx, draw);
    sCaptures.push_back(draw);
}

void OnDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount,
                   UINT startIndexLocation, INT baseVertexLocation)
{
    CaptureDraw(ctx, indexCount, startIndexLocation, baseVertexLocation, 1, 0);
}

void OnDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT indexCountPerInstance,
                            UINT instanceCount, UINT startIndexLocation,
                            INT baseVertexLocation, UINT startInstanceLocation)
{
    CaptureDraw(ctx, indexCountPerInstance, startIndexLocation, baseVertexLocation,
                instanceCount, startInstanceLocation);
}

void ResetFrame()
{
    // Per-frame coverage diagnostic — log when skip rate or toggle count changes,
    // so we can see if the GBuffer-pass detection is missing draws (which would
    // explain camera-dependent missing buildings if the game briefly leaves the
    // 3-RT GBuffer config to do something else, then returns).
    static uint32_t sLastSkipped = ~0u;
    static uint32_t sLastToggles = ~0u;
    if (sFrameDrawsSkipped != sLastSkipped || sFrameGBufferToggles != sLastToggles)
    {
        Log("GeometryCapture: frame coverage — total=%u captured=%u skipped=%u toggles=%u",
            sFrameDrawsTotal, sFrameDrawsCaptured, sFrameDrawsSkipped, sFrameGBufferToggles);
        sLastSkipped = sFrameDrawsSkipped;
        sLastToggles = sFrameGBufferToggles;
    }
    sFrameDrawsTotal = sFrameDrawsCaptured = sFrameDrawsSkipped = 0;
    sFrameGBufferToggles = 0;

    if (!sCaptures.empty())
    {
        if (sFramesCaptured == 0)
        {
            uint32_t classified = 0;
            for (const auto& draw : sCaptures)
                if (draw.vsMetadata && draw.vsMetadata->transformType != VSTransformType::UNKNOWN)
                    classified++;
            Log("GeometryCapture: first frame — %u draws captured, %u classified (%.0f%%)",
                (uint32_t)sCaptures.size(), classified,
                sCaptures.size() > 0 ? classified * 100.0f / sCaptures.size() : 0.0f);
            if (sCaptureFlags & DUST_CAPTURE_PS_RESOURCES)
                Log("GeometryCapture: PS resource capture enabled");

            // (Per-source histogram now logged cumulatively from CaptureDrawState as
            // unique (source, slotBits, instanced) combos appear.)
        }
        sFramesCaptured++;
    }

    ReleaseCaptures();
    sInGBufferPass = false;
}

const std::vector<CapturedDraw>& GetCaptures()
{
    return sCaptures;
}

uint32_t GetCaptureCount()
{
    return (uint32_t)sCaptures.size();
}

bool IsInGBufferPass()
{
    return sInGBufferPass;
}

void SetDevice(ID3D11Device* device)
{
    sCachedDevice = device;
    sCaptures.reserve(256);
}

bool CheckGBufferConfig(UINT numViews, ID3D11RenderTargetView* const* ppRTVs,
                        ID3D11DepthStencilView* pDSV)
{
    return IsGBufferConfig(numViews, ppRTVs, pDSV);
}

void SetResolution(UINT width, UINT height)
{
    if (width != sExpectedWidth || height != sExpectedHeight)
        sCacheValid = false;
    sExpectedWidth  = width;
    sExpectedHeight = height;
}

void SetCaptureFlags(uint32_t flags)
{
    sCaptureFlags = flags;
}

uint32_t GetCaptureFlags()
{
    return sCaptureFlags;
}

void Shutdown()
{
    ReleaseCaptures();
    for (auto& entry : sStagingPool)
        if (entry.buffer) entry.buffer->Release();
    sStagingPool.clear();
    sStagingPoolUsed = 0;
}

} // namespace GeometryCapture
