#include "GeometryReplay.h"
#include "GeometryCapture.h"
#include "ShaderDatabase.h"
#include "DustLog.h"
#include "D3D11Hook.h"
#include <cstring>
#include <cmath>

namespace GeometryReplay
{

static uint32_t sReplaysIssued = 0;

// ---- cameraVP oracle (placement recovery) ------------------------------------------
// Row-major 4x4 multiply: out = a x b (out[r][c] = sum_k a[r][k]*b[k][c]).
static void MatMul(float* out, const float* a, const float* b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] +
                         a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}

// Standard row-major 4x4 inverse (double accumulation). Returns false if singular.
static bool Inverse4x4(float* out, const float* m)
{
    double a[16]; for (int i = 0; i < 16; ++i) a[i] = (double)m[i];
    double inv[16];
    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    double det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (det > -1e-12 && det < 1e-12) return false;
    double idet = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] * idet);
    return true;
}

// Per-frame cameraVP^-1 for placement recovery.
// VALIDATED OFFLINE against bit-exact captured CB bytes (tools/gate*.py, 2026-06-10):
//   cameraVP = inverseView x proj  (plain row-major multiply of the two gCameraData matrices).
// Then per draw, M = capturedClip x cameraVP^-1 is the draw's WORLD matrix:
//   M ~= I        -> PRE-TRANSFORMED (verts already in world space)  -> clip = lightVP
//   M rigid       -> WORLD-PLACED (verts model-local; M places them) -> clip = M x lightVP
//                    (this recovers the cliff/distant meshes that else replay as phantoms)
//   otherwise     -> fall back to PRE (never emit a wild placement).
//
// COHERENCE (kills camera-motion lag): gCameraData is read one frame late, but the captures
// are THIS frame — using the stale VP directly makes buildings drift off identity during a pan
// (misclassified -> jitter). Instead the stale VP is only an ANCHOR: the PRE-TRANSFORMED
// building batch's OWN captured clip IS cameraVP, from the exact same frame. We pick the
// captured clip nearest the anchor (validated margin: building cluster 0.0 vs next 675 units,
// so a frame of motion cannot flip the choice) and use THAT -> buildings are exact identity
// regardless of motion. Falls back to the raw anchor if no captured clip is close (the
// pre-transformed batch isn't in view this frame).
static bool  sCamVPValid = false;
static float sCamVPInv[16];

void BeginFrame(ID3D11DeviceContext* ctx)
{
    sCamVPValid = false;
    float iv[16], pj[16];
    if (!D3D11Hook::GetCameraMatrices(iv, pj)) return;
    float anchor[16];
    MatMul(anchor, iv, pj);

    // Find the captured clip nearest the anchor and copy it out (the coherent same-frame
    // cameraVP). Single pass: copy the running best into a local so no mapped pointer escapes.
    const auto& captures = GeometryCapture::GetCaptures();
    float camVP[16];
    float bestDist = 1e30f;
    for (const auto& d : captures)
    {
        if (!d.vsMetadata || d.vsMetadata->transformType == VSTransformType::UNKNOWN) continue;
        if (!d.cbStagingCopy || d.vsMetadata->clipMatrixOffset + 64 > d.cbStagingSize) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(d.cbStagingCopy, 0, D3D11_MAP_READ, 0, &m))) continue;
        const float* clip = (const float*)((const char*)m.pData + d.vsMetadata->clipMatrixOffset);
        float dist = 0.0f;
        for (int k = 0; k < 16; ++k) { float a = clip[k]-anchor[k]; if (a<0) a=-a; if (a>dist) dist=a; }
        if (dist < bestDist) { bestDist = dist; memcpy(camVP, clip, 64); }
        ctx->Unmap(d.cbStagingCopy, 0);
    }

    // No captured clip near the anchor (pre-transformed batch not in view) -> use the raw
    // anchor; placement still correct, just with the minor 1-frame skew.
    if (bestDist >= 100.0f) memcpy(camVP, anchor, 64);

    sCamVPValid = Inverse4x4(sCamVPInv, camVP);
}

// Classify M = clip x cameraVP^-1. Returns 0=PRE, 1=WORLD, 2=fallback-to-PRE.
static int ClassifyPlacement(const float* clip, float worldOut[16])
{
    if (!sCamVPValid) return 0;   // no oracle -> current behavior (all PRE)
    float M[16];
    MatMul(M, clip, sCamVPInv);
    // Identity test (generous tol absorbs the 1-frame gCameraData/capture skew during motion;
    // PRE vs WORLD margin is huge — world translations are hundreds vs identity's zeros).
    float idres = 0.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        { float t = M[r*4+c] - (r==c ? 1.0f : 0.0f); float a = t<0?-t:t; if (a>idres) idres=a; }
    if (idres < 0.02f) return 0;  // PRE-TRANSFORMED
    // Rigid/affine test: projective column ~0, w ~1, 3x3 rows orthonormal.
    auto af = [](float v){ return v<0?-v:v; };
    if (af(M[3]) > 1e-2f || af(M[7]) > 1e-2f || af(M[11]) > 1e-2f) return 2; // non-affine -> junk
    if (af(M[15]-1.0f) > 1e-2f) return 2;
    for (int r = 0; r < 3; ++r)
    {
        float len2 = M[r*4+0]*M[r*4+0] + M[r*4+1]*M[r*4+1] + M[r*4+2]*M[r*4+2];
        if (af(len2 - 1.0f) > 0.05f) return 2;   // non-unit row -> not a pure rotation
    }
    memcpy(worldOut, M, 64);
    return 1;   // WORLD-PLACED
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
    int hPre = 0, hWorld = 0;

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

        // Placement via the cameraVP oracle (BeginFrame). PRE-TRANSFORMED draws keep the bare
        // light view-proj; WORLD-PLACED draws (today's phantoms) compose their recovered world
        // matrix with it so they replay at the right spot. Anything that doesn't classify as a
        // clean rotation falls back to PRE (never a wild placement).
        float worldM[16];
        int place = ClassifyPlacement(clipDst, worldM);
        if (place == 1)
        {
            float wvp[16];
            MatMul(wvp, worldM, replacementVP);   // local -> world -> light clip
            memcpy(clipDst, wvp, 64);
            if (dbgLog) hWorld++;
        }
        else
        {
            memcpy(clipDst, replacementVP, 64);
            if (dbgLog) hPre++;
        }

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
    if (dbgLog) Log("GeoReplay instanced-drawn=%d  placement[PRE=%d WORLD=%d] camVP=%d",
                    instDrawn, hPre, hWorld, (int)sCamVPValid);

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
