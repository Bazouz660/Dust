// AO pipeline common header -- shared constants, camera helpers, and utility
// functions used by all ambient-occlusion passes.

#ifndef AO_PIPELINE_HLSLI
#define AO_PIPELINE_HLSLI

// ============================================================================
// Constants
// ============================================================================

static const float PI      = 3.1415926535;
static const float HALF_PI = 1.5707963268;
static const float TAU     = 6.2831853072;

// ============================================================================
// Constant buffer -- shared by all AO passes
// ============================================================================

cbuffer AOPassData : register(b0)
{
    float2 BufferPixelSize;    // = (1/w, 1/h)
    float2 BufferScreenSize;   // = (w, h)

    float2 BufferAspectRatio;  // = (1, w/h) -- aspect ratio
    float  TanHalfFov;
    float  FarPlane;           // linear far plane (1000 default)

    // AO parameters
    float  SampleRadius;        // world/screen AO sampling radius
    float  SsaoAmount;          // AO intensity multiplier
    float  FadeDepth;           // depth at which AO fades out
    float  WorldspaceEnable;    // bool-as-float

    int    SampleQualityPreset; // 0..6
    int    ShadingRate;         // 0=full, 1=half, 2=quarter
    int    FilterSize;          // 0..2
    int    AoType;              // 0=GTAO, 1=SolidAngle, 2=Bitmask, 3=Bitmask+SA

    int    DeinterleaveTileCount;
    int    DeinterleaveHigh;
    uint   FrameCount;
    float  DebugView;

};

// ============================================================================
// Quality preset sample counts
// ============================================================================

static const uint2 samples_per_preset[7] =
{
    uint2(2, 2),    // Low       --   8 samples
    uint2(2, 4),    // Medium    --  16
    uint2(2, 10),   // High      --  40
    uint2(3, 12),   // Very High --  72
    uint2(4, 14),   // Ultra     -- 112
    uint2(6, 16),   // Extreme   -- 192
    uint2(8, 20)    // Max       -- 320
};

// ============================================================================
// Fast polynomial approximation of acos
// ============================================================================

float fast_acos(float x)
{
    float o = sqrt(mad(abs(x), mad(abs(x), 0.5405464, -3.0079475), 2.4674011));
    return x < 0.0 ? PI - o : o;
}

float2 fast_acos(float2 x)
{
    float2 o = sqrt(mad(abs(x), mad(abs(x), 0.5405464, -3.0079475), 2.4674011));
    return x < 0.0.xx ? PI - o : o;
}

// ============================================================================
// Camera helpers -- uv_to_proj, depth_to_z, z_to_depth
// ============================================================================

// Kenshi's depth at our t2 source (deferred lighting input) is already
// linearized in [0,1] -- Kenshi's pipeline does the unprojection upstream.
//
// Sky/far pixels (depth near 1.0) produce extreme z that contaminates
// normals and filter regression at silhouette edges. Cap depth so z stays
// bounded -- AO fades to zero well before the cap, so no visible AO is lost.
float depth_to_z(float depth)
{
    float far_cap = max(FadeDepth * 3.0, 0.5);
    // Kenshi clears sky to depth 0 -> z=1 (nearest), making sky taps
    // act as close occluders. Remap to far z so AO and normals ignore them.
    depth = (depth < 1e-4) ? far_cap : min(depth, far_cap);
    return depth * FarPlane + 1.0;
}

float z_to_depth(float z)
{
    float ifar = rcp(FarPlane);
    return z * ifar - ifar;
}

// View-space convention: X right, Y DOWN (top of screen = -Y), Z into scene.
// SSAORenderer's BuildViewMatrix / BuildInverseViewMatrix negate the up basis
// vector when composing matrices so OGRE's Y-up camUp gets converted to our
// shader's Y-down view-space.
float3 uv_to_proj(float2 uv, float z)
{
    float2 uvtoprojADD = -TanHalfFov * BufferAspectRatio.yx;
    float2 uvtoprojMUL = -2.0 * uvtoprojADD;
    return float3((uv * uvtoprojMUL + uvtoprojADD) * z, z);
}

float2 proj_to_uv(float3 pos)
{
    if (pos.z <= 0.001) return float2(-1.0, -1.0);
    float2 uvtoprojADD = -TanHalfFov * BufferAspectRatio.yx;
    float2 uvtoprojMUL = -2.0 * uvtoprojADD;
    return ((pos.xy / pos.z) - uvtoprojADD) / uvtoprojMUL;
}

// ============================================================================
// Deinterleave/reinterleave helpers (non-compute path)
// ============================================================================

float2 deinterleave_uv(float2 uv)
{
    float tile = (float)DeinterleaveTileCount;
    float2 splituv = uv * tile;
    float2 splitoffset = floor(splituv) - tile * 0.5 + 0.5;
    splituv = frac(splituv) + splitoffset * BufferPixelSize;
    return splituv;
}

float2 reinterleave_uv(float2 uv)
{
    uint tile = (uint)DeinterleaveTileCount;
    uint2 whichtile = (uint2)floor(uv / BufferPixelSize) % tile;
    float2 newuv = uv + whichtile;
    newuv /= (float)tile;
    return newuv;
}

uint2 deinterleave_pos(uint2 pos, uint tile, uint2 gridsize)
{
    uint2 tilesize = (gridsize + tile - 1) / tile; // CEIL_DIV
    uint2 tile_idx    = pos % tile;
    uint2 pos_in_tile = pos / tile;
    return tile_idx * tilesize + pos_in_tile;
}

uint2 reinterleave_pos(uint2 pos, uint tile, uint2 gridsize)
{
    uint2 tilesize = (gridsize + tile - 1) / tile;
    uint2 tile_idx    = pos / tilesize;
    uint2 pos_in_tile = pos % tilesize;
    return pos_in_tile * tile + tile_idx;
}

float2 pixel_idx_to_uv(float2 pos, float2 texture_size)
{
    float2 inv_texture_size = rcp(texture_size);
    return pos * inv_texture_size + 0.5 * inv_texture_size;
}

// ============================================================================
// Jitter -- deterministic per-tile value for deinterleaved sampling.
// All pixels in the same deinterleave tile share the same jitter so the
// Gather4 filter can cleanly combine the structured directional diversity.
// ============================================================================

float get_jitter(uint2 p)
{
    uint tiles = (uint)DeinterleaveTileCount;
    uint jitter_idx = dot(p % tiles, uint2(1, tiles));
    jitter_idx *= DeinterleaveHigh ? 17u : 11u;
    return ((jitter_idx % (tiles * tiles)) + 0.5) / (float)(tiles * tiles);
}

// Golden-ratio 1D quasi-random sequence (R1, Roberts 2018). Used to dither
// the sector-mapping in the visibility bitmask so the 1/32 quantization
// becomes high-frequency noise that the gather4 filter can smooth out.
float qmc_roberts1(float idx, float seed)
{
    return frac(seed + idx * 0.38196601125);
}

// ============================================================================
// Depth-based fade factor
// ============================================================================

float get_fade_factor(float depth)
{
    float fade = saturate(1.0 - depth * depth);
    depth /= max(FadeDepth, 1e-5);
    return fade * saturate(exp2(-depth * depth));
}

// ============================================================================
// Variable shading rate -- skip tiles for temporal reuse
// ============================================================================

bool shading_rate(uint2 tile_idx)
{
    if (ShadingRate == 1)
        return ((tile_idx.x + tile_idx.y) & 1) ^ (FrameCount & 1);
    if (ShadingRate == 2)
        return (tile_idx.x & 1 + (tile_idx.y & 1) * 2) ^ (FrameCount & 3);
    return false;
}

// ============================================================================
// Visibility bitmask sector operations
// ============================================================================

static uint occlusion_bitfield;

void bitfield_init()
{
    occlusion_bitfield = 0xFFFFFFFFu;
}

void process_horizons(float2 h)
{
    uint a = (uint)(h.x * 32.0);
    uint b = (uint)(h.y * 32.0) - a;
    uint occlusion = ((1u << b) - 1u) << a;
    occlusion_bitfield &= ~occlusion;
}

float integrate_sectors()
{
    return saturate(countbits(occlusion_bitfield) / 32.0);
}

// ============================================================================
// Normals from depth -- reconstructs view-space normals using cross products
// of neighboring projected positions, weighted to minimize discontinuities.
//
// IMPORTANT: this MUST operate on the SCREEN-LAYOUT depth (Kenshi's raw
// depth from t0), NOT on ZSrc. ZSrc holds depth in deinterleaved layout, so
// sampling it at screen UV would return depth for an unrelated screen pixel
// and the resulting normal repeats with the tile pattern (16-tile artifact).
// ============================================================================

float3 get_normals(float2 uv, out float edge_weight, Texture2D<float> rawDepth, SamplerState samp)
{
    float3 delta = float3(BufferPixelSize, 0);
    float3 center = uv_to_proj(uv,         depth_to_z(rawDepth.SampleLevel(samp, uv,         0)));
    float3 deltaL = uv_to_proj(uv - delta.xz, depth_to_z(rawDepth.SampleLevel(samp, uv - delta.xz, 0))) - center;
    float3 deltaR = uv_to_proj(uv + delta.xz, depth_to_z(rawDepth.SampleLevel(samp, uv + delta.xz, 0))) - center;
    float3 deltaT = uv_to_proj(uv - delta.zy, depth_to_z(rawDepth.SampleLevel(samp, uv - delta.zy, 0))) - center;
    float3 deltaB = uv_to_proj(uv + delta.zy, depth_to_z(rawDepth.SampleLevel(samp, uv + delta.zy, 0))) - center;

    float4 zdeltaLRTB = abs(float4(deltaL.z, deltaR.z, deltaT.z, deltaB.z));
    float4 w = zdeltaLRTB.xzyw + zdeltaLRTB.zywx;
    w = rcp(0.001 + w * w);

    edge_weight = saturate(1.0 - dot(w, 1.0));

    float3 n0 = cross(deltaT, deltaL);
    float3 n1 = cross(deltaR, deltaT);
    float3 n2 = cross(deltaB, deltaR);
    float3 n3 = cross(deltaL, deltaB);

    float4 finalweight = w * rsqrt(float4(dot(n0, n0), dot(n1, n1), dot(n2, n2), dot(n3, n3)));
    float3 normal = n0 * finalweight.x + n1 * finalweight.y + n2 * finalweight.z + n3 * finalweight.w;
    normal *= rsqrt(dot(normal, normal) + 1e-8);
    return normal;
}

// ============================================================================
// Misc helpers
// ============================================================================

bool inside_screen(float2 uv)
{
    return all(saturate(uv - uv * uv));
}

#endif // AO_PIPELINE_HLSLI
