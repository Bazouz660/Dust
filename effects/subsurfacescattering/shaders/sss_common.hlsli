// sss_common.hlsli — shared constants, skin-tag detection and diffusion profile
// for the Dust Subsurface Scattering effect.
//
// Skin tag: the host patches the game's character.hlsl / skin.hlsl GBuffer-fill
// shaders to write a sentinel value into buf1.a (the NORMALS render target alpha,
// an 8-bit B8G8R8A8_UNORM channel). buf1.a is decoded by the game's deferred
// lighting as translucency (a<0.5 -> translucency=a*2) / emissive (a>=0.5). We
// place the tag in the low translucency range so skin reads as mildly translucent
// (physically appropriate) and we never touch the emissive >=0.5 range (glowy eyes).
//
// Tag value = 17/255 (~0.0667). Detect by an 8-bit-aware band around byte 17.

#define SSS_TAG_VALUE   (17.0 / 255.0)
#define SSS_TAG_LO      (15.5 / 255.0)   // byte 16 .. 18 accepted
#define SSS_TAG_HI      (18.5 / 255.0)

cbuffer SSSParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  blurWidth;      // world-space-ish blur scale (screen px at unit depth proxy)
    float  depthThreshold; // bilateral depth rejection (fraction of center depth)
    float  followSurface;  // 0..1 — how strongly to clamp blur to skin-tagged taps
    float  maxRadiusPx;    // hard cap on per-tap pixel offset
    float  strength;       // composite blend: 0 = no SSS, 1 = full blurred skin
    float  debugMode;      // 0 = off, 1 = highlight skin mask
    float2 _pad;
};

// Is this NORMALS.a value our skin sentinel?
bool IsSkinTag(float a)
{
    return a > SSS_TAG_LO && a < SSS_TAG_HI;
}

// 7-tap separable skin diffusion profile (normalized to sum 1.0).
// Approximates Jimenez "Separable Subsurface Scattering" kernel — a wide,
// reddish falloff. We only have lit (already-shaded) HDR color, not separated
// diffuse, so we apply a per-channel weighting that bleeds red further than
// green/blue, mimicking the dermis red diffusion. Offsets are in "profile units"
// scaled by blurWidth/depth at runtime.
static const int   SSS_TAPS = 7;
static const float SSS_OFFSET[7] = { -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0 };

// Per-channel weights. Index 3 is the center. Red diffuses widest.
static const float3 SSS_WEIGHT[7] = {
    float3(0.060, 0.020, 0.012),
    float3(0.110, 0.075, 0.055),
    float3(0.170, 0.165, 0.150),
    float3(0.320, 0.480, 0.566),   // center
    float3(0.170, 0.165, 0.150),
    float3(0.110, 0.075, 0.055),
    float3(0.060, 0.020, 0.012),
};

// Separable subsurface blur, one axis. `dir` is the unit pixel step direction
// (1,0) for horizontal, (0,1) for vertical, expressed in UV via invViewportSize.
//   sceneTex   t0 : HDR color of the in-progress blur (linear-clamp sampled)
//   normalsTex t1 : GBuffer normals; .a carries the skin tag (point sampled)
//   depthTex   t2 : euclidean depth / farClip (point sampled)
// Non-skin center pixels are returned unchanged so the pass is a no-op there.
float4 SSSBlurAxis(Texture2D sceneTex, Texture2D normalsTex, Texture2D depthTex,
                   SamplerState linearClamp, SamplerState pointClamp,
                   float2 uv, float2 dir)
{
    float3 centerColor = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  centerTag   = normalsTex.SampleLevel(pointClamp, uv, 0).a;

    // Skip everything that isn't skin — pass color straight through.
    if (!IsSkinTag(centerTag))
        return float4(centerColor, 1.0);

    float centerDepth = depthTex.SampleLevel(pointClamp, uv, 0).r;
    if (centerDepth <= 0.0)
        return float4(centerColor, 1.0);

    // World-constant blur: nearer skin (small depth) -> wider screen blur.
    // depth is euclidean dist/farClip in [0,1]; scale so screen radius shrinks
    // with distance. Clamp to a hard pixel cap to keep it stable up close.
    float radiusPx = min(blurWidth / max(centerDepth, 1e-3), maxRadiusPx);

    float2 stepUV = dir * invViewportSize * radiusPx;

    float3 accum  = float3(0, 0, 0);
    float3 wsum   = float3(0, 0, 0);

    [unroll]
    for (int i = 0; i < SSS_TAPS; ++i)
    {
        float2 sampleUV = uv + stepUV * SSS_OFFSET[i];
        float3 c   = sceneTex.SampleLevel(linearClamp, sampleUV, 0).rgb;
        float  tag = normalsTex.SampleLevel(pointClamp, sampleUV, 0).a;
        float  d   = depthTex.SampleLevel(pointClamp, sampleUV, 0).r;

        // Bilateral: reject taps too far in depth (background bleed).
        float depthDiff = abs(d - centerDepth);
        float depthW = step(depthDiff, depthThreshold * centerDepth + 1e-4);

        // Surface-follow: bias toward skin-tagged taps so skin color does not
        // bleed onto clothing/hair. followSurface=0 -> ignore tag, 1 -> require it.
        float skinW = IsSkinTag(tag) ? 1.0 : (1.0 - followSurface);

        float3 w = SSS_WEIGHT[i] * (depthW * skinW);
        accum += c * w;
        wsum  += w;
    }

    float3 blurred = accum / max(wsum, 1e-4);
    return float4(blurred, 1.0);
}
