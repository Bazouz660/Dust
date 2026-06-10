#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "ShaderDatabase.h"
#include "DustLog.h"
#include <cstring>
#include <cmath>

namespace GeometryReplay
{

static uint32_t sReplaysIssued = 0;

void BeginFrame(ID3D11DeviceContext* ctx)
{
    // Reverted to the baseline (replay everything with replacementVP). Kept as a no-op so
    // callers stay stable; per-draw placement classification was abandoned — the captured
    // clip matrices form ~20 distinct clusters per scene (no shared cameraVP to verify
    // against), so captured-CB forensics cannot recover world placement in general.
    (void)ctx;
}

struct ScratchCBEntry
{
    ID3D11Buffer* buffer;
    uint32_t      size;
};
static std::vector<ScratchCBEntry> sScratchCBs;

static ID3D11Buffer* GetScratchCB(ID3D11Device* device, uint32_t requiredSize)
{
    for (auto& entry : sScratchCBs)
    {
        if (entry.size == requiredSize)
            return entry.buffer;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = requiredSize;
    desc.Usage           = D3D11_USAGE_DEFAULT;
    desc.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;

    ID3D11Buffer* buf = nullptr;
    HRESULT hr = device->CreateBuffer(&desc, nullptr, &buf);
    if (FAILED(hr) || !buf)
        return nullptr;

    sScratchCBs.push_back({ buf, requiredSize });
    return buf;
}

// Save/restore IA + VS state around the replay batch.
// Lighter than D3D11StateBlock — only touches what replay modifies.
struct ReplayStateBlock
{
    ID3D11InputLayout*       iaLayout   = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY iaTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer*            vbs[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbStrides[CapturedDraw::MAX_VB_SLOTS] = {};
    UINT                     vbOffsets[CapturedDraw::MAX_VB_SLOTS] = {};
    ID3D11Buffer*            ib         = nullptr;
    DXGI_FORMAT              ibFormat   = DXGI_FORMAT_R16_UINT;
    UINT                     ibOffset   = 0;
    ID3D11VertexShader*      vs         = nullptr;
    ID3D11Buffer*            vsCBs[CapturedDraw::MAX_VS_CBS] = {};
    ID3D11ShaderResourceView* vsSRVs[CapturedDraw::MAX_VS_SRVS] = {};
    ID3D11SamplerState*      vsSamplers[CapturedDraw::MAX_VS_SAMPLERS] = {};

    void Capture(ID3D11DeviceContext* ctx)
    {
        ctx->IAGetInputLayout(&iaLayout);
        ctx->IAGetPrimitiveTopology(&iaTopology);
        ctx->IAGetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
        ctx->VSGetShaderResources(0, CapturedDraw::MAX_VS_SRVS, vsSRVs);
        ctx->VSGetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, vsSamplers);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->IASetInputLayout(iaLayout);
        ctx->IASetPrimitiveTopology(iaTopology);
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs, vbStrides, vbOffsets);
        ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, CapturedDraw::MAX_VS_CBS, vsCBs);
        ctx->VSSetShaderResources(0, CapturedDraw::MAX_VS_SRVS, vsSRVs);
        ctx->VSSetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, vsSamplers);
        Release();
    }

    void Release()
    {
        if (iaLayout) { iaLayout->Release(); iaLayout = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VB_SLOTS; i++)
            if (vbs[i]) { vbs[i]->Release(); vbs[i] = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        if (vs) { vs->Release(); vs = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
            if (vsCBs[i]) { vsCBs[i]->Release(); vsCBs[i] = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_SRVS; i++)
            if (vsSRVs[i]) { vsSRVs[i]->Release(); vsSRVs[i] = nullptr; }
        for (UINT i = 0; i < CapturedDraw::MAX_VS_SAMPLERS; i++)
            if (vsSamplers[i]) { vsSamplers[i]->Release(); vsSamplers[i] = nullptr; }
    }
};

uint32_t Replay(ID3D11DeviceContext* ctx, ID3D11Device* device,
                const float* replacementVP,
                const float* replacementVPSkin,
                const float* cullCenter, float cullRadius)
{
    const auto& captures = GeometryCapture::GetCaptures();
    if (captures.empty())
        return 0;
    if (!replacementVPSkin) replacementVPSkin = replacementVP;

    (void)cullCenter; (void)cullRadius;

    ReplayStateBlock saved;
    saved.Capture(ctx);

    uint32_t replayed = 0;
    static std::vector<uint8_t> cbDataBuf;

    // [Dust diag] per-category seen/drawn histogram, logged once per ~120 Replay calls.
    static int dbgCall = 0;
    bool dbgLog = ((dbgCall++ % 120) == 0);
    int hSeen[8] = {0}, hDrawn[8] = {0}, culled = 0, instDrawn = 0;

    for (const auto& draw : captures)
    {
        if (!draw.vsMetadata || draw.vsMetadata->transformType == VSTransformType::UNKNOWN)
            continue;

        // Replay ONLY static solid geometry. Terrain (vertex-texture fetch),
        // foliage, and skinned meshes run VS variants that read resources the
        // replay never rebinds (heightmap / bone buffers / vertex SRVs), which
        // GPU-faults the back-face pass. OBJECTS / DISTANT_TOWN / TRIPLANAR are
        // the safe solid occluders back-face thickness actually needs.
        DustShaderCategory cat = ShaderDatabase::GetVertexShaderCategory(draw.vs);
        int ci = ((int)cat >= 0 && (int)cat <= 7) ? (int)cat : 0;
        if (dbgLog) hSeen[ci]++;
        {
            // SKIN disabled: bones/frame/clip-offset all verified correct, but the replayed
            // skinned geometry still doesn't land in the cube (suspected vertex-input/layout
            // issue) -> it smears and pollutes every cube. Re-enable after a RenderDoc capture
            // of a replayed skin draw pins the input bug. Static geometry casts fine.
            if (cat != DUST_SHADER_OBJECTS &&
                cat != DUST_SHADER_DISTANT_TOWN &&
                cat != DUST_SHADER_TRIPLANAR)
                continue;
        }

        if (!draw.cbStagingCopy)
            continue;

        const VSConstantBufferInfo& meta = *draw.vsMetadata;

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->Map(draw.cbStagingCopy, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
            continue;

        if (cbDataBuf.size() < draw.cbStagingSize)
            cbDataBuf.resize(draw.cbStagingSize);
        memcpy(cbDataBuf.data(), mapped.pData, draw.cbStagingSize);
        ctx->Unmap(draw.cbStagingCopy, 0);

        if (meta.clipMatrixOffset + 64 > draw.cbStagingSize)
            continue;

        float* clipDst = reinterpret_cast<float*>(cbDataBuf.data() + meta.clipMatrixOffset);

        // BASELINE (known-best state): replay every draw with the light view-proj directly.
        // Correct for pre-transformed geometry (buildings/props near the light cast right);
        // world-placed meshes (clip baked per-draw, ~20 distinct clip clusters measured per
        // scene) replay at wrong positions -> the known "phantom shadow" junk. Recovering
        // their placement from captured CBs is NOT generally possible (world uniform is
        // zero/junk for most of them) — the structural fix is the OGRE re-render occluder
        // source (docs/point_shadow_ogre_rerender_plan.md).
        memcpy(clipDst, replacementVP, 64);

        ID3D11Buffer* scratchCB = GetScratchCB(device, draw.cbStagingSize);
        if (!scratchCB)
            continue;

        ctx->UpdateSubresource(scratchCB, 0, nullptr, cbDataBuf.data(), 0, 0);

        // Set IA state (both VB slots — slot 1 has instance data for instanced draws)
        ctx->IASetInputLayout(draw.inputLayout);
        ctx->IASetPrimitiveTopology(draw.topology);
        // Slot 1 = per-instance transforms; use the per-draw snapshot (the live buffer is
        // recycled and stale by replay time for instanced draws).
        ID3D11Buffer* vbs[CapturedDraw::MAX_VB_SLOTS] = {
            draw.vertexBuffers[0],
            draw.instVBCopy ? draw.instVBCopy : draw.vertexBuffers[1] };
        ctx->IASetVertexBuffers(0, CapturedDraw::MAX_VB_SLOTS, vbs,
                                draw.vbStrides, draw.vbOffsets);
        ctx->IASetIndexBuffer(draw.indexBuffer, draw.indexFormat, draw.ibOffset);

        // Set VS and constant buffers
        ctx->VSSetShader(draw.vs, nullptr, 0);
        for (UINT i = 0; i < CapturedDraw::MAX_VS_CBS; i++)
        {
            if (i == meta.cbSlot)
                ctx->VSSetConstantBuffers(i, 1, &scratchCB);
            else
            {
                // Prefer the per-draw bindable snapshot (skinned bone palette); the live
                // vsCBs pointer is stale by replay time for skinned draws.
                ID3D11Buffer* cb = draw.cbCopies[i] ? draw.cbCopies[i] : draw.vsCBs[i];
                if (cb) ctx->VSSetConstantBuffers(i, 1, &cb);
            }
        }
        // Rebind VS-stage resources so skin/terrain VS fetches (bone palette,
        // heightmap, vertex-fetch) don't read unbound SRVs and GPU-fault.
        ctx->VSSetShaderResources(0, CapturedDraw::MAX_VS_SRVS, draw.vsSRVs);
        ctx->VSSetSamplers(0, CapturedDraw::MAX_VS_SAMPLERS, draw.vsSamplers);

        if (draw.instanceCount > 1)
        {
            ctx->DrawIndexedInstanced(draw.indexCount, draw.instanceCount,
                                      draw.startIndexLocation, draw.baseVertexLocation,
                                      draw.startInstanceLocation);
        }
        else
        {
            ctx->DrawIndexed(draw.indexCount, draw.startIndexLocation,
                             draw.baseVertexLocation);
        }

        replayed++;
        if (dbgLog) { hDrawn[ci]++; if (draw.instanceCount > 1) instDrawn++; }
    }

    saved.Restore(ctx);

    if (dbgLog)
        Log("GeoReplay seen[OBJ=%d SKIN=%d TERR=%d FOL=%d TRI=%d DT=%d UNK=%d] drawn[OBJ=%d SKIN=%d TRI=%d DT=%d] culled=%d caps=%d",
            hSeen[1],hSeen[4],hSeen[2],hSeen[3],hSeen[5],hSeen[6],hSeen[0],
            hDrawn[1],hDrawn[4],hDrawn[5],hDrawn[6], culled, (int)captures.size());
    if (dbgLog) Log("GeoReplay instanced-drawn=%d", instDrawn);

    if (sReplaysIssued < 3 && replayed > 0)
    {
        Log("GeometryReplay: replayed %u / %u draws", replayed, (uint32_t)captures.size());
        sReplaysIssued++;
    }

    return replayed;
}

void Shutdown()
{
    for (auto& entry : sScratchCBs)
        if (entry.buffer) entry.buffer->Release();
    sScratchCBs.clear();
}

} // namespace GeometryReplay
