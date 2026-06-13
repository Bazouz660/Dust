// RTGI Ray Trace — Compute Shader (Visibility-Bitmask Horizon GI)
//
// Horizon-based screen-space GI with a 32-sector visibility bitmask
// (Therrien et al. 2023, "Screen Space Indirect Lighting with Visibility
// Bitmask").  Each slice sweeps the depth buffer and records which angular
// SECTORS of the hemisphere are occluded.  This gives DIRECTIONAL occlusion —
// contact shadows that hug geometry — instead of the single scalar AO the old
// cosine-hemisphere march produced, and gathers bounce light only from the
// sectors an occluder NEWLY closes (no double counting).
//
// CB layout, SRV/UAV bindings and the RGBA16F (indirectRGB, AO) output are
// unchanged, so the existing temporal + a-trous denoiser and composite passes
// are untouched.  raysPerPixel is reinterpreted as the slice count and raySteps
// as the samples-per-slice.

static const float PI       = 3.14159265358979;
static const float HALF_PI  = 1.57079632679490;
static const float TWO_PI   = 6.28318530717959;
static const uint  SECTOR_COUNT = 32u;

float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float3 ReconstructViewPos(float2 uv, float depth, float thf, float ar)
{
    float3 pos;
    pos.x = (uv.x * 2.0 - 1.0) * ar * thf * depth;
    pos.y = (1.0 - uv.y * 2.0) * thf * depth;
    pos.z = depth;
    return pos;
}

float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Reinhard-style firefly compression (matches the previous trace) so a single
// bright screen pixel can't dominate the gathered bounce.
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
    uv += sampleJitter * invViewportSize;

    float depth = depthTex.SampleLevel(pointClamp, uv, 0);

    if (depth <= 0.0001 || depth > fadeDistance)
    {
        outTex[tid.xy] = float4(0, 0, 0, 1);
        return;
    }

    float3 startView = ReconstructViewPos(uv, depth, tanHalfFov, aspectRatio);
    float3 camera = normalize(-startView);   // view-space direction to the eye

    // ---- Normal reconstruction (unchanged from the previous trace) ----
    // geoNormal lives in the same view frame as startView (built from the same
    // ReconstructViewPos); gbufNormal is transformed into that frame, so both
    // share the position frame and dot(normal, camera) > 0 for visible pixels.
    float3 geoNormal;
    {
        float dL = depthTex.SampleLevel(pointClamp, uv - float2(invViewportSize.x, 0), 0);
        float dR = depthTex.SampleLevel(pointClamp, uv + float2(invViewportSize.x, 0), 0);
        float dU = depthTex.SampleLevel(pointClamp, uv - float2(0, invViewportSize.y), 0);
        float dD = depthTex.SampleLevel(pointClamp, uv + float2(0, invViewportSize.y), 0);

        float3 ddx_pos = (abs(dL - depth) < abs(dR - depth))
            ? startView - ReconstructViewPos(uv - float2(invViewportSize.x, 0), dL, tanHalfFov, aspectRatio)
            : ReconstructViewPos(uv + float2(invViewportSize.x, 0), dR, tanHalfFov, aspectRatio) - startView;
        float3 ddy_pos = (abs(dU - depth) < abs(dD - depth))
            ? startView - ReconstructViewPos(uv - float2(0, invViewportSize.y), dU, tanHalfFov, aspectRatio)
            : ReconstructViewPos(uv + float2(0, invViewportSize.y), dD, tanHalfFov, aspectRatio) - startView;

        geoNormal = normalize(cross(ddx_pos, ddy_pos));
    }

    float3 worldN = normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0;
    float3 gbufNormal;
    gbufNormal.x =  dot(worldN, camRight.xyz);
    gbufNormal.y =  dot(worldN, camUp.xyz);
    gbufNormal.z = -dot(worldN, camForward.xyz);
    gbufNormal = normalize(gbufNormal);

    if (dot(geoNormal, gbufNormal) < 0)
        geoNormal = -geoNormal;

    float3 normal = normalize(lerp(geoNormal, gbufNormal, normalDetail));

    int numSlices  = max(2, (int)raysPerPixel);
    int numSamples = max(1, (int)raySteps);

    // Slice march distance in UV.  The previous trace reached rayLength * depth
    // in view space; projecting that through ReconstructViewPos cancels depth,
    // so the UV reach is constant.  x carries an extra 1/aspect, and screen-up
    // is -uv.y, so the screen march of (omega.x/ar, -omega.y) corresponds to the
    // view-space slice direction (omega.x, omega.y, 0).
    float uvReach = rayLength * 0.5 / tanHalfFov;

    // View-space thickness an occluder is assumed to extend behind its surface.
    float thicknessV = thickness * pow(depth, thicknessCurve);

    // Per-pixel temporal rotation/offset; the denoiser accumulates across frames.
    float2 jit = float2(frac(frameIndex * 0.7548776662), frac(frameIndex * 0.5698402910)) * 127.0;
    float  noise = IGN(pixelPos + jit);

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

        [loop]
        for (int s = 0; s < numSamples; s++)
        {
            // Cluster samples near the origin (pow 1.5) where nearby geometry
            // contributes the most occlusion / indirect light.
            float tLin = ((float)s + 0.5 + noise * 0.5) / (float)numSamples;
            float t = tLin * sqrt(tLin);

            float2 sUV = uv + t * marchUV;
            if (any(sUV < 0.0) || any(sUV > 1.0))
                break;

            float sDepth = depthTex.SampleLevel(pointClamp, sUV, 0);
            if (sDepth <= 0.0001)
                continue;

            float3 sPos  = ReconstructViewPos(sUV, sDepth, tanHalfFov, aspectRatio);
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
            // Samples march along +orthoDir, so their signed angle from the view
            // axis is +fb; the normal's signed angle is nAngle.  The sector is
            // (psi_H - psi_N + HALF_PI)/PI, hence fb - nAngle (NOT +nAngle: that
            // mirror-flips occluders about the view axis on tilted surfaces).
            float2 sect = saturate((fb - nAngle + HALF_PI) / PI);
            float minH = min(sect.x, sect.y);
            float maxH = max(sect.x, sect.y);

            uint  sampleBits = UpdateSectors(minH, maxH, 0u);
            uint  newBits    = sampleBits & ~bitfield;
            float hit        = (float)countbits(newBits) / (float)SECTOR_COUNT;

            if (hit > 0.0)
            {
                float3 rad = Compress(sceneTex.SampleLevel(linearClamp, sUV, 0).rgb);

                if (bounceIntensity > 0.0)
                    rad += Compress(prevGI.SampleLevel(linearClamp, sUV, 0).rgb) * bounceIntensity;

                // Receiver cosine: light arriving from the occluder direction.
                float ndl = saturate(dot(normal, sHorizon));
                lighting += rad * (hit * ndl);
            }

            bitfield |= sampleBits;
        }

        occludedAccum += (float)countbits(bitfield) / (float)SECTOR_COUNT;
    }

    lighting /= (float)numSlices;
    float occluded = occludedAccum / (float)numSlices;        // [0,1] occluded fraction
    float ao = saturate(1.0 - occluded * aoIntensity);

    // Distance fade (unchanged): vanish the effect toward the far plane.
    float fadeStart = fadeDistance * 0.8;
    float depthFade = saturate(1.0 - max(depth - fadeStart, 0.0) / max(fadeDistance - fadeStart, 0.001));
    lighting *= depthFade;
    ao = lerp(1.0, ao, depthFade);

    outTex[tid.xy] = float4(lighting, ao);
}
