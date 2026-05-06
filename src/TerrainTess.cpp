#include "TerrainTess.h"
#include "ShaderDatabase.h"
#include "DustLog.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

namespace TerrainTess
{

namespace {

// VS output layout for terrain.hlsl main_vs (TEXTURED variant — used in both
// terrainfp4 GBuffer paths). HS/DS structs must match this layout exactly.
//   POSITION  : float4 (clip-space position from worldViewProjMatrix)
//   TEXCOORD0 : float3 (world-space normal)
//   TEXCOORD1 : float3 (world-space position, for distance/parallax)
//   TEXCOORD2 : float3 (terrain triplanar/horizontal UV)
//   TEXCOORD3 : float4 (overlay & biome map UVs)
//   TEXCOORD4 : float2 (cliff blend weights)
//   TEXCOORD5 : float4 (vertical-cliff distortion offsets)
//
// The shadow VS variant has only POSITION + a single TEXCOORD0 (depth) and
// is NOT handled by the current spike — IsTerrainDraw filters it out by
// requiring the PS to also classify as TERRAIN.
static const char* kPassthroughHS = R"HLSL(
struct VsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
    float4 wvpCol1  : TEXCOORD6;
};

struct HsConst
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

HsConst HsConstFn(InputPatch<VsOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    // Per-edge factor from THAT edge's midpoint depth — adjacent patches
    // sharing an edge agree on its factor, eliminating cracks at zone
    // boundaries. D3D convention: edges[i] = edge OPPOSITE to vertex i.
    float w0 = patch[0].pos.w;
    float w1 = patch[1].pos.w;
    float w2 = patch[2].pos.w;
    float wEdge0 = (w1 + w2) * 0.5;  // edge opposite v0
    float wEdge1 = (w0 + w2) * 0.5;  // edge opposite v1
    float wEdge2 = (w0 + w1) * 0.5;  // edge opposite v2

    HsConst c;
    c.edges[0] = lerp(16.0, 1.0, smoothstep(50.0, 200.0, wEdge0));
    c.edges[1] = lerp(16.0, 1.0, smoothstep(50.0, 200.0, wEdge1));
    c.edges[2] = lerp(16.0, 1.0, smoothstep(50.0, 200.0, wEdge2));
    c.inside   = (c.edges[0] + c.edges[1] + c.edges[2]) / 3.0;
    return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstFn")]
[maxtessfactor(16.0)]
VsOut main(InputPatch<VsOut, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}
)HLSL";

static const char* kPassthroughDS = R"HLSL(
struct VsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
    float4 wvpCol1  : TEXCOORD6;
};

struct HsConst
{
    float edges[3] : SV_TessFactor;
    float inside   : SV_InsideTessFactor;
};

struct DsOut
{
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 tex0     : TEXCOORD2;
    float4 tex1     : TEXCOORD3;
    float2 uvblend  : TEXCOORD4;
    float4 texV     : TEXCOORD5;
};

[domain("tri")]
DsOut main(HsConst c, float3 bary : SV_DomainLocation, const OutputPatch<VsOut, 3> patch)
{
    DsOut o;
    o.normal   = patch[0].normal   * bary.x + patch[1].normal   * bary.y + patch[2].normal   * bary.z;
    o.worldPos = patch[0].worldPos * bary.x + patch[1].worldPos * bary.y + patch[2].worldPos * bary.z;
    o.tex0     = patch[0].tex0     * bary.x + patch[1].tex0     * bary.y + patch[2].tex0     * bary.z;
    o.tex1     = patch[0].tex1     * bary.x + patch[1].tex1     * bary.y + patch[2].tex1     * bary.z;
    o.uvblend  = patch[0].uvblend  * bary.x + patch[1].uvblend  * bary.y + patch[2].uvblend  * bary.z;
    o.texV     = patch[0].texV     * bary.x + patch[1].texV     * bary.y + patch[2].texV     * bary.z;

    // Pass-through clip + sub-vertex Y displacement projected via wvpCol1.
    // wvpCol1 is constant per draw — same for all 3 patch corners.
    // Amplitude fades to zero on the same depth range as the HS factor
    // falloff: at factor 1 (far) we'd get linear-interp ridges, so taper
    // amplitude in lockstep so distant terrain ends up flat instead.
    float4 wvpCol1 = patch[0].wvpCol1;
    float4 passClip = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
    float ampScale = 1.0 - smoothstep(50.0, 200.0, passClip.w);
    float h = sin(o.worldPos.x * 0.5) * cos(o.worldPos.z * 0.5) * 3.0 * ampScale;
    o.pos = passClip + h * wvpCol1;
    o.worldPos.y += h;  // PS-side worldPos reflects displacement (lighting/shadow consistency)
    return o;
}
)HLSL";

ID3D11HullShader*   gHs = nullptr;
ID3D11DomainShader* gDs = nullptr;

struct SavedState
{
    D3D11_PRIMITIVE_TOPOLOGY topo  = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11HullShader*        hs    = nullptr;
    ID3D11DomainShader*      ds    = nullptr;
    ID3D11Buffer*            hsCb0 = nullptr;
};
SavedState gSaved;

// Strip-to-list conversion cache. Key is constructed from source IB pointer
// + range (start, count). Value is the converted immutable list IB plus its
// new index count. Cached entries are released at Shutdown.
struct ConvertedIB
{
    ID3D11Buffer* listIB = nullptr;
    UINT          listIndexCount = 0;
};
std::unordered_map<uint64_t, ConvertedIB> gIBCache;

uint64_t MakeIBKey(ID3D11Buffer* ib, UINT startIdx, UINT indexCount)
{
    // Cheap mixing — pointer is the dominant identity, plus range.
    uint64_t k = (uint64_t)(uintptr_t)ib;
    k ^= (uint64_t)startIdx * 0x9E3779B97F4A7C15ULL;
    k ^= (uint64_t)indexCount * 0xBF58476D1CE4E5B9ULL;
    return k;
}

bool CompileShader(const char* src, const char* target, ID3DBlob** outBlob)
{
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, outBlob, &errors);
    if (FAILED(hr))
    {
        Log("TerrainTess: compile failed (%s): %s",
            target,
            errors ? (const char*)errors->GetBufferPointer() : "no error blob");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    return true;
}

} // anonymous namespace

// Reflect the bytecode and log its input + output signatures. Used to verify
// that VS output → HS input → DS output → PS input semantic chain is intact.
// Silent pipeline failures from signature mismatches are very hard to diagnose
// without this kind of introspection.
void LogShaderSignature(const void* bytecode, size_t size, const char* tag)
{
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytecode, size, IID_ID3D11ShaderReflection, (void**)&refl)) || !refl)
    {
        Log("LogShaderSignature(%s): D3DReflect failed", tag);
        return;
    }

    D3D11_SHADER_DESC desc;
    refl->GetDesc(&desc);
    Log("=== Signature %s: %u in, %u out, %u patchOut ===",
        tag, desc.InputParameters, desc.OutputParameters, desc.PatchConstantParameters);

    for (UINT i = 0; i < desc.InputParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetInputParameterDesc(i, &p);
        Log("  IN [%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }
    for (UINT i = 0; i < desc.OutputParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetOutputParameterDesc(i, &p);
        Log("  OUT[%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }
    for (UINT i = 0; i < desc.PatchConstantParameters; i++)
    {
        D3D11_SIGNATURE_PARAMETER_DESC p = {};
        refl->GetPatchConstantParameterDesc(i, &p);
        Log("  PATCH[%u] %s%u sysVal=%u mask=0x%X",
            i, p.SemanticName ? p.SemanticName : "?",
            p.SemanticIndex, (UINT)p.SystemValueType, (UINT)p.Mask);
    }

    refl->Release();
}

void Init(ID3D11Device* device)
{
    if (!device) return;
    if (gHs && gDs) return;

    ID3DBlob* hsBlob = nullptr;
    if (!CompileShader(kPassthroughHS, "hs_5_0", &hsBlob))
        return;

    ID3DBlob* dsBlob = nullptr;
    if (!CompileShader(kPassthroughDS, "ds_5_0", &dsBlob))
    {
        hsBlob->Release();
        return;
    }

    HRESULT hr = device->CreateHullShader(hsBlob->GetBufferPointer(),
                                          hsBlob->GetBufferSize(),
                                          nullptr, &gHs);
    if (FAILED(hr))
    {
        Log("TerrainTess: CreateHullShader failed (0x%08X)", hr);
        hsBlob->Release();
        dsBlob->Release();
        return;
    }

    hr = device->CreateDomainShader(dsBlob->GetBufferPointer(),
                                    dsBlob->GetBufferSize(),
                                    nullptr, &gDs);
    if (FAILED(hr))
    {
        Log("TerrainTess: CreateDomainShader failed (0x%08X)", hr);
        gHs->Release(); gHs = nullptr;
        hsBlob->Release();
        dsBlob->Release();
        return;
    }

    LogShaderSignature(hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), "HS");
    LogShaderSignature(dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), "DS");

    hsBlob->Release();
    dsBlob->Release();

    Log("TerrainTess: passthrough HS+DS compiled and ready");
}

void Shutdown()
{
    for (auto& kv : gIBCache)
    {
        if (kv.second.listIB) kv.second.listIB->Release();
    }
    gIBCache.clear();
    if (gHs) { gHs->Release(); gHs = nullptr; }
    if (gDs) { gDs->Release(); gDs = nullptr; }
}

// Internal helpers ----------------------------------------------------------

namespace {

bool IsTerrainShaderBound(ID3D11DeviceContext* ctx)
{
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, 0);
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, 0);

    bool ok = false;
    if (vs && ps)
    {
        ok = (ShaderDatabase::GetVertexShaderCategory(vs) == DUST_SHADER_TERRAIN) &&
             (ShaderDatabase::GetPixelShaderCategory(ps) == DUST_SHADER_TERRAIN);
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    return ok;
}

// Read the source IB sub-range, generate equivalent list indices with
// alternating winding, create an immutable list IB. Caches by (src ptr, range).
// Returns the cached list IB and its index count via out params.
bool PrepareStripConversion(ID3D11DeviceContext* ctx,
                            ID3D11Buffer* origIB, DXGI_FORMAT origFormat,
                            UINT origByteOffset,
                            UINT stripIndexCount, UINT stripStartIndex,
                            ID3D11Buffer** outListIB, UINT* outListIndexCount)
{
    if (!origIB || stripIndexCount < 3) return false;
    if (origFormat != DXGI_FORMAT_R16_UINT && origFormat != DXGI_FORMAT_R32_UINT)
        return false;

    uint64_t key = MakeIBKey(origIB, stripStartIndex, stripIndexCount);
    auto it = gIBCache.find(key);
    if (it != gIBCache.end())
    {
        *outListIB = it->second.listIB;
        *outListIndexCount = it->second.listIndexCount;
        return true;
    }

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    if (!device) return false;

    UINT indexSize = (origFormat == DXGI_FORMAT_R16_UINT) ? 2 : 4;

    // Staging copy so we can read the source IB on the CPU.
    D3D11_BUFFER_DESC srcDesc = {};
    origIB->GetDesc(&srcDesc);

    D3D11_BUFFER_DESC stagingDesc = {};
    stagingDesc.ByteWidth      = srcDesc.ByteWidth;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Buffer* staging = nullptr;
    if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, &staging)))
    {
        device->Release();
        return false;
    }
    ctx->CopyResource(staging, origIB);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        staging->Release();
        device->Release();
        return false;
    }

    UINT triCount = stripIndexCount - 2;
    UINT maxListIndices = triCount * 3;
    void* listData = malloc((size_t)maxListIndices * indexSize);

    const uint8_t* srcBase = (const uint8_t*)mapped.pData
                           + origByteOffset
                           + (size_t)stripStartIndex * indexSize;

    // D3D11 TRIANGLESTRIP convention: even triangles emit (v0,v1,v2),
    // odd triangles emit (v1,v0,v2) so the rasterizer sees consistent
    // winding across the whole strip. We emit the same vertex order as
    // a TRIANGLELIST so the rasterizer produces identical triangles.
    //
    // Strips use degenerate triangles (two coincident vertices, zero area)
    // as row terminators — they're invisible in raster but tessellation +
    // displacement breaks the degeneracy and reveals them as long thin
    // spikes. Skip any triangle where two indices match.
    UINT dstTri = 0;
    if (indexSize == 2)
    {
        const uint16_t* src = (const uint16_t*)srcBase;
        uint16_t* dst = (uint16_t*)listData;
        for (UINT i = 0; i < triCount; i++)
        {
            uint16_t a = src[i], b = src[i+1], c = src[i+2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { dst[dstTri*3+0] = b; dst[dstTri*3+1] = a; dst[dstTri*3+2] = c; }
            else       { dst[dstTri*3+0] = a; dst[dstTri*3+1] = b; dst[dstTri*3+2] = c; }
            dstTri++;
        }
    }
    else
    {
        const uint32_t* src = (const uint32_t*)srcBase;
        uint32_t* dst = (uint32_t*)listData;
        for (UINT i = 0; i < triCount; i++)
        {
            uint32_t a = src[i], b = src[i+1], c = src[i+2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { dst[dstTri*3+0] = b; dst[dstTri*3+1] = a; dst[dstTri*3+2] = c; }
            else       { dst[dstTri*3+0] = a; dst[dstTri*3+1] = b; dst[dstTri*3+2] = c; }
            dstTri++;
        }
    }
    UINT listIndexCount = dstTri * 3;

    // Diagnostic — log a few source and list indices while data still mapped.
    {
        static int sLog = 0;
        if (sLog < 2)
        {
            sLog++;
            if (indexSize == 2)
            {
                const uint16_t* sp = (const uint16_t*)srcBase;
                const uint16_t* lp = (const uint16_t*)listData;
                Log("TerrainTess: src first 12: %u %u %u %u %u %u %u %u %u %u %u %u",
                    sp[0],sp[1],sp[2],sp[3],sp[4],sp[5],sp[6],sp[7],sp[8],sp[9],sp[10],sp[11]);
                Log("TerrainTess: list first 12: %u %u %u %u %u %u %u %u %u %u %u %u",
                    lp[0],lp[1],lp[2],lp[3],lp[4],lp[5],lp[6],lp[7],lp[8],lp[9],lp[10],lp[11]);
            }
            Log("TerrainTess: byteOff=%u startIdx=%u stripCnt=%u listCnt=%u fmt=%d",
                origByteOffset, stripStartIndex, stripIndexCount, listIndexCount, (int)origFormat);
        }
    }

    ctx->Unmap(staging, 0);
    staging->Release();

    D3D11_BUFFER_DESC newDesc = {};
    newDesc.ByteWidth = listIndexCount * indexSize;
    newDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    newDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = listData;

    ID3D11Buffer* listIB = nullptr;
    HRESULT hr = device->CreateBuffer(&newDesc, &initData, &listIB);
    free(listData);
    device->Release();

    if (FAILED(hr)) return false;

    ConvertedIB cv;
    cv.listIB         = listIB;
    cv.listIndexCount = listIndexCount;
    gIBCache[key]     = cv;

    static int sLogCount = 0;
    if (sLogCount < 3)
    {
        sLogCount++;
        Log("TerrainTess: converted strip→list (cache=%zu, src=%u → list=%u indices)",
            gIBCache.size(), stripIndexCount, listIndexCount);
    }

    *outListIB         = listIB;
    *outListIndexCount = listIndexCount;
    return true;
}

} // anonymous namespace

void Begin(ID3D11DeviceContext* ctx)
{
    // Diagnostic: log the first few tessellated draws so we can confirm we're
    // actually subdividing terrain (and not silently skipping every draw).
    static int sCount = 0;
    if (sCount < 5)
    {
        sCount++;
        Log("TerrainTess: tessellating draw #%d", sCount);

    }

    // Save current state — Kenshi/Ogre never sets HS/DS on its own, but be
    // defensive in case some other hook does.
    ctx->IAGetPrimitiveTopology(&gSaved.topo);
    ctx->HSGetShader(&gSaved.hs, nullptr, 0);
    ctx->DSGetShader(&gSaved.ds, nullptr, 0);
    ctx->HSGetConstantBuffers(0, 1, &gSaved.hsCb0);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ctx->HSSetShader(gHs, nullptr, 0);
    ctx->DSSetShader(gDs, nullptr, 0);

    // Defensive: clear any GS that might be bound between DS and rasterizer.
    ID3D11GeometryShader* nullGs = nullptr;
    ctx->GSSetShader(nullGs, nullptr, 0);

    // Mirror VS cbuffer to HS slot 0 so HsConstFn can read cameraPos for
    // distance-based tessellation factors.
    ID3D11Buffer* vsCb0 = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vsCb0);
    ctx->HSSetConstantBuffers(0, 1, &vsCb0);
    if (vsCb0) vsCb0->Release();
}

void End(ID3D11DeviceContext* ctx)
{
    ctx->IASetPrimitiveTopology(gSaved.topo);
    ctx->HSSetShader(gSaved.hs, nullptr, 0);
    ctx->DSSetShader(gSaved.ds, nullptr, 0);
    ctx->HSSetConstantBuffers(0, 1, &gSaved.hsCb0);

    if (gSaved.hs)    { gSaved.hs->Release();    gSaved.hs = nullptr; }
    if (gSaved.ds)    { gSaved.ds->Release();    gSaved.ds = nullptr; }
    if (gSaved.hsCb0) { gSaved.hsCb0->Release(); gSaved.hsCb0 = nullptr; }
    gSaved.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

bool TryDrawTessellated(ID3D11DeviceContext* ctx,
                        UINT indexCount, UINT startIndex,
                        INT baseVertex, DrawIndexedFn drawFn)
{
    if (!ctx || !gHs || !gDs || !drawFn) return false;
    if (!IsTerrainShaderBound(ctx)) return false;

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);

    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    {
        // Direct path — patchlist with N control points takes the same indices
        // as a TRIANGLELIST. Just override topology, bind HS/DS, draw.
        Begin(ctx);
        drawFn(ctx, indexCount, startIndex, baseVertex);
        End(ctx);
        return true;
    }

    if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    {
        ID3D11Buffer* origIB = nullptr;
        DXGI_FORMAT   origFormat = DXGI_FORMAT_UNKNOWN;
        UINT          origOffset = 0;
        ctx->IAGetIndexBuffer(&origIB, &origFormat, &origOffset);
        if (!origIB) return false;

        ID3D11Buffer* listIB = nullptr;
        UINT          listIC = 0;
        bool ok = PrepareStripConversion(ctx, origIB, origFormat, origOffset,
                                         indexCount, startIndex, &listIB, &listIC);
        if (!ok)
        {
            origIB->Release();
            return false;
        }

        ctx->IASetIndexBuffer(listIB, origFormat, 0);
        Begin(ctx);
        drawFn(ctx, listIC, 0, baseVertex);
        End(ctx);
        ctx->IASetIndexBuffer(origIB, origFormat, origOffset);
        origIB->Release();
        return true;
    }

    return false;
}

} // namespace TerrainTess
