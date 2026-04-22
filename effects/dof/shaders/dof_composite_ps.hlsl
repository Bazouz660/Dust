// DOF composite — blends sharp original with blurred based on signed CoC.
// CoC > 0 = far blur, CoC < 0 = near blur, CoC == 0 = in focus.

Texture2D sceneTex       : register(t0);
Texture2D blurTex        : register(t1);
Texture2D<float> cocTex  : register(t2);
SamplerState linearClamp : register(s0);

cbuffer DOFParams : register(b0)
{
    float2 texelSize;
    float  focusDistance;
    float  nearStart;
    float  nearEnd;
    float  nearStrength;
    float  farStart;
    float  farEnd;
    float  farStrength;
    float  blurRadius;
    float  maxDepth;
    int    cocMode;
    float  aperture;
    float  highlightThreshold;
    float  highlightBoost;
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float3 sharp   = sceneTex.SampleLevel(linearClamp, uv, 0).rgb;
    float3 blurred = blurTex.SampleLevel(linearClamp, uv, 0).rgb;
    float  coc     = cocTex.SampleLevel(linearClamp, uv, 0).r;

    float blendFactor = saturate(abs(coc));
    float3 result = lerp(sharp, blurred, blendFactor);

    float blurBrightness = max(blurred.r, max(blurred.g, blurred.b));
    float highlightBleed = saturate(blurBrightness - highlightThreshold) * highlightBoost * blendFactor;
    result += blurred * highlightBleed;

    return float4(result, 1.0);
}
