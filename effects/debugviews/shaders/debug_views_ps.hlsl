// debug_views_ps.hlsl — Fullscreen GBuffer visualisation modes for Dust/Kenshi.
// Handles modes 2-8; mode 1 (wireframe) is done entirely in C++ via geometry replay.
// Runs on a fullscreen triangle at POST_TONEMAP, fully replacing the frame.

#pragma pack_matrix(row_major)

cbuffer DebugViewsCB : register(b0)
{
    int      viewMode;   // 2=world-normals 3=view-normals 4=depth 5=luma 6=roughness 7=metalness 8=lighting
    float    depthMin;   // linear depth mapped to black
    float    depthMax;   // linear depth mapped to white
    float    _pad0;
    float4x4 inverseView; // row-major world←view for view-normals transform
    float2   viewportSize;
    float2   _pad1;
};

// GBuffer inputs (no scene copy needed — we fully replace the frame)
Texture2D<float>  depthTex   : register(t0); // GBuffer2 linear view-Z
Texture2D<float4> normalsTex : register(t1); // GBuffer1 normals in [0,1]
Texture2D<float4> albedoTex  : register(t2); // GBuffer0 YCoCg-encoded albedo
Texture2D<float4> hdrTex     : register(t3); // pre-fog HDR (lighting mode)

SamplerState pointSamp  : register(s0);
SamplerState linearSamp : register(s1);

// ── Helpers ────────────────────────────────────────────────────────────────

float3 DecodeNormal(float4 raw) { return normalize(raw.xyz * 2.0 - 1.0); }
float3 NormalToRGB(float3 n)   { return n * 0.5 + 0.5; }
float3 Reinhard(float3 c)      { return c / (c + 1.0); }

// ── Modes ──────────────────────────────────────────────────────────────────

float3 ModeWorldNormals(float2 uv)
{
    return NormalToRGB(DecodeNormal(normalsTex.SampleLevel(pointSamp, uv, 0)));
}

float3 ModeViewNormals(float2 uv)
{
    float3 wn = DecodeNormal(normalsTex.SampleLevel(pointSamp, uv, 0));
    // Row-vector convention: viewNormal = worldNormal * invView_3x3
    // inverseView rows are the camera axes in world space; projecting wn onto them
    // gives the camera-space components.
    float3x3 ivr = (float3x3)inverseView;
    return NormalToRGB(normalize(mul(wn, ivr)));
}

float3 ModeDepth(float2 uv)
{
    // GBuffer2 stores length(worldPos - cameraPos) / farClip, already in [0,1].
    // Invert so near=white, far=black (matches typical depth visualisations).
    float d = depthTex.SampleLevel(pointSamp, uv, 0);
    float t = saturate((d - depthMin) / max(depthMax - depthMin, 0.001));
    return (1.0 - t).xxx;
}

float3 ModeLuma(float2 uv)
{
    // GBuffer0.r = Y channel of YCoCg encoding (albedo luminance)
    float y = albedoTex.SampleLevel(pointSamp, uv, 0).r;
    return float3(y, y, y);
}

float3 ModeRoughness(float2 uv)
{
    float gloss = albedoTex.SampleLevel(pointSamp, uv, 0).a;
    float rough = 1.0 - gloss;
    return lerp(float3(0.0, 0.3, 1.0), float3(1.0, 0.1, 0.0), rough); // blue→red
}

float3 ModeMetalness(float2 uv)
{
    float metal = albedoTex.SampleLevel(pointSamp, uv, 0).b;
    return lerp(float3(0.04, 0.04, 0.04), float3(1.0, 0.84, 0.0), metal); // grey→gold
}

float3 ModeLighting(float2 uv)
{
    return Reinhard(hdrTex.SampleLevel(linearSamp, uv, 0).rgb);
}

// ── Main ───────────────────────────────────────────────────────────────────

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 result;
    [branch] switch (viewMode)
    {
        case 2:  result = ModeWorldNormals(uv); break;
        case 3:  result = ModeViewNormals(uv);  break;
        case 4:  result = ModeDepth(uv);        break;
        case 5:  result = ModeLuma(uv);         break;
        case 6:  result = ModeRoughness(uv);    break;
        case 7:  result = ModeMetalness(uv);    break;
        case 8:  result = ModeLighting(uv);     break;
        default: result = float3(1, 0, 1);      break; // magenta = bad mode
    }
    return float4(result, 1.0);
}
