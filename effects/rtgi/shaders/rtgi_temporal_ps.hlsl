// RTGI Temporal Accumulation — Motion-Compensated Bilateral Reprojection
//
// References:
//   Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering" (HPG 2017)
//     - Variance-guided temporal alpha, per-pixel variance tracking
//   Karis 2014, "High Quality Temporal Supersampling" (SIGGRAPH 2014)
//     - Neighborhood clamping, bilateral history validation
//   Nehab et al. 2007, "Accelerating Real-Time Shading with Reverse Reprojection Caching"
//     - Motion vector derivation from depth + camera matrices
//
// Key improvements over naive temporal:
//   1. History fetched at reprojected UV (not current pixel)
//   2. Bilateral validation rejects misaligned history taps (depth + normal)
//   3. Variance-guided alpha adapts blending to scene dynamics
//   4. Per-pixel temporal variance enables variance-guided spatial denoising
//
// Output (MRT):
//   SV_Target0 — Accumulated GI (RGB) + AO (A)
//   SV_Target1 — Temporal metadata: z (R), octahedral normal (GB), encoded variance (A)

// ---- Octahedral normal encoding (Meyer et al. 2010) ----

float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
    {
        float2 s = step(float2(0, 0), n.xy) * 2.0 - 1.0;
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return n.xy * 0.5 + 0.5;
}

float3 OctDecode(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0)
    {
        float2 s = step(float2(0, 0), n.xy) * 2.0 - 1.0;
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

// ---- Variance encoding (FP16-safe) ----

static const float VARIANCE_SCALE = 128.0;

float EncodeVariance(float v) { return sqrt(max(0, v * VARIANCE_SCALE)); }
float DecodeVariance(float v) { return v * v / VARIANCE_SCALE; }

// ---- View-space reconstruction ----

float3 ReconstructViewPos(float2 uv, float depth, float thf, float ar)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * ar * thf * depth;
    pos.y = (1.0 - uv.y * 2.0) * thf * depth;
    pos.z = depth;
    return pos;
}

float2 ViewPosToUV(float3 vp, float thf, float ar)
{
    float2 uv;
    uv.x = (vp.x / (vp.z * ar * thf)) * 0.5 + 0.5;
    uv.y = 0.5 - (vp.y / (vp.z * thf)) * 0.5;
    return uv;
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- Resources ----

Texture2D<float4> currentGI        : register(t0);
Texture2D<float4> historyGI        : register(t1);
Texture2D<float>  depthTex         : register(t2);
Texture2D<float4> prevTemporalData : register(t3);
Texture2D<float4> normalsTex       : register(t4);
Texture2D<float4> spatialMoments   : register(t5);
SamplerState      pointClamp       : register(s0);
SamplerState      linearClamp      : register(s1);

cbuffer TemporalParams : register(b0)
{
    float2   viewportSize;
    float2   invViewportSize;
    float    tanHalfFov;
    float    aspectRatio;
    float    temporalBlend;
    float    frameIndex;
    row_major float4x4 reprojMatrix;
    float    motionMagnitude;
    float    _pad0;
    float    _pad1;
    float    _pad2;
};

struct TemporalOutput
{
    float4 accum    : SV_Target0;
    float4 metadata : SV_Target1;
};

TemporalOutput main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
    TemporalOutput o;

    int2 pix = int2(pos.xy);
    float depth = depthTex.SampleLevel(pointClamp, uv, 0);
    float4 current = currentGI.Load(int3(pix, 0));

    float3 normalWorld = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;

    // ---- First frame or sky ----
    if (frameIndex < 1.0 || depth <= 0.0001)
    {
        o.accum = current;
        float2 octN = OctEncode(normalWorld);
        o.metadata = float4(depth, octN, 0);
        return o;
    }

    // ---- Compute motion vector via reprojection ----
    float3 viewPos = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);
    float3 prevViewPos = mul(float4(viewPos, 1.0), reprojMatrix).xyz;

    float2 prevUV = uv;
    bool insideScreen = false;

    if (prevViewPos.z > 0.0001)
    {
        prevUV = ViewPosToUV(prevViewPos, tanHalfFov, aspectRatio);
        insideScreen = all(prevUV > 0.0) && all(prevUV < 1.0);
    }

    // ---- Bilateral history fetch at reprojected UV ----
    // Karis 2014: sample 2x2 bilinear footprint, reject taps by depth + normal
    bool validHistory = insideScreen;
    float4 history = float4(0, 0, 0, 1);
    float prevVariance = 0;

    if (insideScreen)
    {
        float2 texelUV = prevUV * viewportSize - 0.5;
        int2 texelLower = int2(floor(texelUV));
        float2 bilinearKernel = frac(texelUV);

        float4 bilinearW;
        bilinearW.x = (1.0 - bilinearKernel.x) * (1.0 - bilinearKernel.y);
        bilinearW.y =        bilinearKernel.x   * (1.0 - bilinearKernel.y);
        bilinearW.z = (1.0 - bilinearKernel.x) *        bilinearKernel.y;
        bilinearW.w =        bilinearKernel.x   *        bilinearKernel.y;

        float3 viewDir = normalize(viewPos);
        float NdotV = abs(dot(viewDir, normalWorld));

        float wsum = 0;
        float4 minVal = float4(1e10, 1e10, 1e10, 1e10);
        float4 maxVal = float4(-1e10, -1e10, -1e10, -1e10);

        static const int2 offsets[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

        [unroll]
        for (int tap = 0; tap < 4; tap++)
        {
            int2 tapPix = texelLower + offsets[tap];

            if (any(tapPix < 0) || tapPix.x >= (int)viewportSize.x || tapPix.y >= (int)viewportSize.y)
                continue;

            float4 prevData = prevTemporalData.Load(int3(tapPix, 0));
            float prevZ = abs(prevData.r);
            float3 prevN = OctDecode(prevData.gb);

            // Depth rejection (Schied 2017 Eq. 3)
            float wz = exp2(-128.0 * abs(prevZ / depth - 1.0) * max(NdotV, 0.1));

            // Normal rejection
            float wn = saturate(dot(prevN, normalWorld));

            float w = bilinearW[tap] * wz * wn;

            float4 tapHistory = historyGI.Load(int3(tapPix, 0));
            history += tapHistory * w;
            prevVariance += DecodeVariance(prevData.a) * bilinearW[tap];
            wsum += w;

            minVal = min(minVal, tapHistory);
            maxVal = max(maxVal, tapHistory);
        }

        if (wsum > 0.05)
        {
            history /= wsum;
            prevVariance /= max(dot(bilinearW, 1), 1e-6);

            // Neighborhood clamping (Karis 2014)
            history = clamp(history, minVal, maxVal);
        }
        else
        {
            validHistory = false;
        }
    }

    // ---- Extended neighborhood clamp from current frame (firefly suppression) ----
    {
        float4 cN = currentGI.Load(int3(pix + int2(0, -1), 0));
        float4 cS = currentGI.Load(int3(pix + int2(0,  1), 0));
        float4 cE = currentGI.Load(int3(pix + int2( 1, 0), 0));
        float4 cW = currentGI.Load(int3(pix + int2(-1, 0), 0));
        float3 cMin = min(min(cN.rgb, cS.rgb), min(cE.rgb, cW.rgb));
        float3 cMax = max(max(cN.rgb, cS.rgb), max(cE.rgb, cW.rgb));
        current.rgb = clamp(current.rgb, cMin, cMax);
    }

    // ---- Variance-guided alpha (Schied 2017 Section 4.2) ----
    // Uses spatial moments to detect scene change vs noise
    float alpha;
    {
        float4 moments = spatialMoments.SampleLevel(linearClamp, uv, 0);
        float2 currData = moments.xy;
        float2 prevData = moments.zw;

        float bias = abs(currData.x - prevData.x);
        float varCurr = currData.y * currData.y;
        float varPrev = prevData.y * prevData.y;
        float denom = 1e-10 + varCurr + varPrev + bias * bias;
        alpha = saturate(1.0 - varCurr / denom);

        float maxAlpha = max(0.15, 1.0 - temporalBlend);
        alpha = clamp(alpha, 0.01, maxAlpha);
    }

    if (!validHistory)
    {
        alpha = 1.0;
        prevVariance = Luminance(current.rgb) * 16.0;
    }

    // ---- Blend ----
    float4 result;
    result.rgb = lerp(history.rgb, current.rgb, alpha);
    result.a   = lerp(history.a,   current.a,   alpha);

    // ---- Per-pixel temporal variance (Welford online estimator) ----
    float prevLum = Luminance(history.rgb);
    float currLum = Luminance(current.rgb);
    float blendedLum = Luminance(result.rgb);
    float varianceUpdate = (prevLum - currLum) * (blendedLum - currLum);
    varianceUpdate *= alpha * 0.5;
    float temporalVariance = lerp(prevVariance, varianceUpdate, alpha);

    // ---- Output ----
    o.accum = result;

    float2 octN = OctEncode(normalWorld);
    bool earlyOut = depth <= 0.0001 || depth > 0.999;
    o.metadata = float4(earlyOut ? -depth : depth, octN, EncodeVariance(temporalVariance));

    return o;
}
