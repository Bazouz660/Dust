// RTGI Ray Trace — Compute Shader (Visibility-Bitmask Horizon GI)
//
// Horizon-based screen-space GI with a 32-sector visibility bitmask
// (Therrien et al. 2023, "Screen Space Indirect Lighting with Visibility
// Bitmask").  Each slice sweeps the depth buffer and records which angular
// SECTORS of the hemisphere are occluded.  This gives DIRECTIONAL occlusion —
// contact shadows that hug geometry — instead of the single scalar AO a
// cosine-hemisphere march produces, and gathers bounce light only from the
// sectors an occluder NEWLY closes (no double counting).
//
// The estimator integrates the hemisphere analytically per slice, so its
// per-frame variance is low enough that quality does NOT depend on temporal
// accumulation.  All sampling noise is a STATIC tiling blue-noise texture
// (no frame-index term): for a fixed camera the output is bit-identical
// every frame — zero shimmer — and because blue noise carries no
// low-frequency structure, the residual per-pixel dither is removed by a
// small a-trous kernel / the bilateral upsampler.  (IGN was tried first and
// its quasi-periodic diagonal structure showed as a repeating grid once the
// per-frame scroll was removed.)
//
// CB layout, SRV/UAV bindings and the RGBA16F (indirectRGB, AO) output are
// unchanged, so the existing temporal + a-trous denoiser and composite passes
// are untouched.  raysPerPixel is reinterpreted as the slice count and raySteps
// as the samples-per-slice (split across the slice's two march directions).
//
// Slices march BOTH directions of the slice plane into ONE bitfield.  Each
// plane's sector map spans the full great circle, so a one-sided march can
// only ever fill its own half — it underestimates enclosed occlusion ~2x and
// leans on cross-pixel rotation averaging (grain) to cover the other side.
// Two-sided marching is the correct estimator and measurably reduces grain
// relative to occlusion depth (validated offline at equal tap budget).

static const float PI       = 3.14159265358979;
static const float HALF_PI  = 1.57079632679490;
static const float TWO_PI   = 6.28318530717959;
static const uint  SECTOR_COUNT = 32u;
static const uint  BLUE_NOISE_MASK = 127u;  // texture is 128x128 (see rtgi_bluenoise.h)

// The depth buffer is a quantized point cloud: within a few texels of the
// trace origin, the direction to a sample is lattice geometry, not surface
// geometry, and near-tangent samples alias into false occlusion that beats
// against the render/depth grid ratio as visible periodic banding.  Three
// countermeasures (validated against an offline replica of this shader):
//   1. reconstruct positions from depth-TEXEL-CENTER rays (SnapUV), never
//      from render-pixel UVs — the depth value belongs to the texel ray;
//   2. skip samples closer than MIN_SAMPLE_TEXELS to the origin, where
//      quantization noise dominates the horizon angle;
//   3. shrink the hemisphere by SECTOR_BIAS sectors at each edge so exact
//      tangent-grazing samples (every sample on a flat surface) cannot
//      flicker the boundary bit.
static const float MIN_SAMPLE_TEXELS = 3.0;
static const float SECTOR_BIAS = 1.5;
static const float BIAS_SCALE = 32.0 / (32.0 - 2.0 * SECTOR_BIAS);

float3 ReconstructViewPos(float2 uv, float depth, float thf, float ar)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * ar * thf * depth;
    pos.y = (1.0 - uv.y * 2.0) * thf * depth;
    pos.z = depth;
    return pos;
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Reinhard-style firefly compression so a single bright screen pixel can't
// dominate the gathered bounce.
float3 Compress(float3 c)
{
    float k = 1.0 / (1.0 + Luminance(c));
    return c * k * k;
}

// Set the sector bits covering the angular interval [minHorizon, maxHorizon]
// (both normalised to [0,1] over the hemisphere) and OR them into the field.
uint UpdateSectors(float minHorizon, float maxHorizon, uint bitfield)
{
    uint startBit    = min((uint)(minHorizon * (float)SECTOR_COUNT), SECTOR_COUNT - 1u);
    uint horizonBits = (uint)ceil((maxHorizon - minHorizon) * (float)SECTOR_COUNT);
    uint angleBits   = horizonBits > 0u ? (0xFFFFFFFFu >> (SECTOR_COUNT - min(horizonBits, SECTOR_COUNT))) : 0u;
    return bitfield | (angleBits << startBit);
}

Texture2D<float>    depthTex    : register(t0);
Texture2D<float4>   sceneTex    : register(t1);
Texture2D<float4>   prevGI      : register(t2);
Texture2D<float4>   normalsTex  : register(t3);
Texture2D<float2>   blueNoise   : register(t4);
RWTexture2D<float4> outTex      : register(u0);
SamplerState        pointClamp  : register(s0);
SamplerState        linearClamp : register(s1);

cbuffer RTGIParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
    float  tanHalfFov;
    float  aspectRatio;
    float  rayLength;
    float  raySteps;
    float  thickness;
    float  fadeDistance;
    float  bounceIntensity;
    float  aoIntensity;
    float  frameIndex;
    float  raysPerPixel;
    float  thicknessCurve;
    float  normalDetail;
    float2 sampleJitter;
    float2 _padJitter;
    float4 camRight;
    float4 camUp;
    float4 camForward;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)viewportSize.x || tid.y >= (uint)viewportSize.y)
        return;

    float2 pixelPos = float2(tid.xy) + 0.5;
    float2 uv = pixelPos * invViewportSize;

    // Static per-pixel noise from the tiling blue-noise texture — no frame
    // term.  Independent channels decorrelate every use; the offset second
    // lookup supplies the sub-texel origin jitter that decoheres the
    // render-grid/depth-grid moire phase (spatially, NOT temporally).
    float2 bn  = blueNoise.Load(int3(tid.xy & BLUE_NOISE_MASK, 0));
    float2 bnJ = blueNoise.Load(int3((tid.xy + 64u) & BLUE_NOISE_MASK, 0));
    float noise     = bn.x;   // slice rotation
    float stepNoise = bn.y;   // sample distance dither

    uv += (bnJ - 0.5) * invViewportSize;

    float2 depthSize;
    depthTex.GetDimensions(depthSize.x, depthSize.y);
    float2 depthTexel = 1.0 / depthSize;

    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth <= 0.0001 || depth > fadeDistance)
    {
        outTex[tid.xy] = float4(0, 0, 0, 1);
        return;
    }

    // Reconstruct from the ray of the depth texel that pointClamp actually
    // returned, so the position lies exactly on the sampled surface point.
    float2 snapUV = (floor(uv * depthSize) + 0.5) * depthTexel;
    float3 startView = ReconstructViewPos(snapUV, depth, tanHalfFov, aspectRatio);
    float3 camera = normalize(-startView);   // view-space direction to the eye

    // ---- Normal reconstruction ----
    // geoNormal lives in the same view frame as startView (built from the same
    // ReconstructViewPos); gbufNormal is transformed into that frame, so both
    // share the position frame and dot(normal, camera) > 0 for visible pixels.
    // Neighbours offset by exactly one DEPTH texel from the snapped centre —
    // every tap lands on a texel centre, so the differencing is alias-free at
    // any render scale.
    float3 geoNormal;
    {
        float2 offX = float2(depthTexel.x, 0);
        float2 offY = float2(0, depthTexel.y);
        float dL = depthTex.SampleLevel(pointClamp, snapUV - offX, 0);
        float dR = depthTex.SampleLevel(pointClamp, snapUV + offX, 0);
        float dU = depthTex.SampleLevel(pointClamp, snapUV - offY, 0);
        float dD = depthTex.SampleLevel(pointClamp, snapUV + offY, 0);

        float3 ddx_pos = (abs(dL - depth) < abs(dR - depth))
            ? startView - ReconstructViewPos(snapUV - offX, dL, tanHalfFov, aspectRatio)
            : ReconstructViewPos(snapUV + offX, dR, tanHalfFov, aspectRatio) - startView;
        float3 ddy_pos = (abs(dU - depth) < abs(dD - depth))
            ? startView - ReconstructViewPos(snapUV - offY, dU, tanHalfFov, aspectRatio)
            : ReconstructViewPos(snapUV + offY, dD, tanHalfFov, aspectRatio) - startView;

        geoNormal = normalize(cross(ddx_pos, ddy_pos));
    }

    float3 worldN = normalsTex.SampleLevel(pointClamp, snapUV, 0).rgb * 2.0 - 1.0;
    float3 gbufNormal;
    gbufNormal.x =  dot(worldN, camRight.xyz);
    gbufNormal.y =  dot(worldN, camUp.xyz);
    gbufNormal.z = -dot(worldN, camForward.xyz);
    gbufNormal = normalize(gbufNormal);

    if (dot(geoNormal, gbufNormal) < 0)
        geoNormal = -geoNormal;

    float3 normal = normalize(lerp(geoNormal, gbufNormal, normalDetail));

    int numSlices     = max(2, (int)raysPerPixel);
    int samplesPerDir = max(1, (int)raySteps / 2);

    // Slice march distance in UV.  Projecting a view-space reach of
    // rayLength * depth through ReconstructViewPos cancels depth, so the UV
    // reach is constant.  x carries an extra 1/aspect, and screen-up is
    // -uv.y, so the screen march of (omega.x/ar, -omega.y) corresponds to the
    // view-space slice direction (omega.x, omega.y, 0).
    float uvReach = rayLength * 0.5 / tanHalfFov;

    // View-space thickness an occluder is assumed to extend behind its surface.
    float thicknessV = thickness * pow(depth, thicknessCurve);

    float3 lighting = float3(0, 0, 0);
    float  occludedAccum = 0.0;

    [loop]
    for (int slice = 0; slice < numSlices; slice++)
    {
        float phi = TWO_PI * ((float)slice + noise) / (float)numSlices;
        float2 omega;
        sincos(phi, omega.y, omega.x);

        float2 marchUV = float2(omega.x / aspectRatio, -omega.y) * uvReach;

        // GTAO slice geometry: angle of the normal projected into this slice
        // plane, measured from the view direction (signed by which side).
        float3 dir       = float3(omega, 0.0);
        float3 orthoDir  = dir - dot(dir, camera) * camera;
        float3 axis      = cross(dir, camera);
        float3 projN     = normal - axis * dot(normal, axis);
        float  projLen   = length(projN) + 1e-6;
        float  signN     = sign(dot(orthoDir, projN));
        float  cosN      = saturate(dot(projN, camera) / projLen);
        float  nAngle    = signN * acos(cosN);   // [-HALF_PI, HALF_PI]

        uint bitfield = 0u;

        // Golden-ratio offset decorrelates the distance dither across slices.
        float sliceStepNoise = frac(stepNoise + (float)slice * 0.61803398875);

        // March distance of t=1 in depth texels, for the min-distance skip.
        float texelsPerT = length(marchUV * depthSize);

        [loop]
        for (int d = 0; d < 2; d++)
        {
            float dirSign = (d == 0) ? 1.0 : -1.0;

            [loop]
            for (int s = 0; s < samplesPerDir; s++)
            {
                // Cluster samples near the origin (pow 1.5) where nearby geometry
                // contributes the most occlusion / indirect light.
                float tLin = ((float)s + 0.5 + sliceStepNoise * 0.5) / (float)samplesPerDir;
                float t = tLin * sqrt(tLin);

                if (t * texelsPerT < MIN_SAMPLE_TEXELS)
                    continue;

                float2 sUV = uv + (dirSign * t) * marchUV;
                if (any(sUV < 0.0) || any(sUV > 1.0))
                    break;

                float sDepth = depthTex.SampleLevel(pointClamp, sUV, 0);
                if (sDepth <= 0.0001)
                    continue;

                float2 sSnap = (floor(sUV * depthSize) + 0.5) * depthTexel;
                float3 sPos  = ReconstructViewPos(sSnap, sDepth, tanHalfFov, aspectRatio);
                float3 sDist = sPos - startView;
                float  sLen  = length(sDist);
                if (sLen < 1e-5)
                    continue;
                float3 sHorizon = sDist / sLen;

                // Front horizon = angle to the sample; back horizon = angle to a
                // point pushed thicknessV behind it along the view ray.  acos is
                // decreasing, so the back angle is the larger one.
                float3 backVec = sDist - camera * thicknessV;     // point pushed thicknessV behind the sample
                float2 fb;
                fb.x = dot(sHorizon, camera);
                fb.y = dot(backVec / max(length(backVec), 1e-5), camera);
                fb = acos(clamp(fb, -1.0, 1.0));                  // angles from camera dir, [0,PI]

                // Map the horizon angle into the hemisphere AROUND THE NORMAL [0,1].
                // The signed in-slice angle is +fb marching +orthoDir and -fb
                // marching -orthoDir; the normal's signed angle is nAngle.  The
                // sector is (psi_H - psi_N + HALF_PI)/PI, hence dirSign*fb - nAngle
                // (NOT +nAngle: that mirror-flips occluders about the view axis on
                // tilted surfaces).  BIAS_SCALE expands the map about the zenith so
                // tangent-grazing horizons fall outside [0,1] and cannot dither the
                // edge bits.
                float2 sect = (dirSign * fb - nAngle + HALF_PI) / PI;
                sect = saturate((sect - 0.5) * BIAS_SCALE + 0.5);
                float minH = min(sect.x, sect.y);
                float maxH = max(sect.x, sect.y);

                uint  sampleBits = UpdateSectors(minH, maxH, 0u);
                uint  newBits    = sampleBits & ~bitfield;
                float hit        = (float)countbits(newBits) / (float)SECTOR_COUNT;

                if (hit > 0.0)
                {
                    // Radiance fetched at the nearest scene-texel CORNER: the
                    // bilinear fetch then averages 4 texels for free, so scene
                    // texture detail doesn't inject chroma grain into the GI.
                    float2 radUV = (floor(sUV * depthSize) + 1.0) * depthTexel;
                    float3 rad = Compress(sceneTex.SampleLevel(linearClamp, radUV, 0).rgb);

                    if (bounceIntensity > 0.0)
                        rad += Compress(prevGI.SampleLevel(linearClamp, sUV, 0).rgb) * bounceIntensity;

                    // Receiver cosine: light arriving from the occluder direction.
                    float ndl = saturate(dot(normal, sHorizon));
                    lighting += rad * (hit * ndl);
                }

                bitfield |= sampleBits;
            }
        }

        occludedAccum += (float)countbits(bitfield) / (float)SECTOR_COUNT;
    }

    lighting /= (float)numSlices;
    float occluded = occludedAccum / (float)numSlices;        // [0,1] occluded fraction
    float ao = saturate(1.0 - occluded * aoIntensity);

    // Distance fade: vanish the effect toward the far plane.
    float fadeStart = fadeDistance * 0.8;
    float depthFade = saturate(1.0 - max(depth - fadeStart, 0.0) / max(fadeDistance - fadeStart, 0.001));
    lighting *= depthFade;
    ao = lerp(1.0, ao, depthFade);

    outTex[tid.xy] = float4(lighting, ao);
}
