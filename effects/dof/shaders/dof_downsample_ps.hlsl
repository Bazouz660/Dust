// 4-tap bilinear box downsample for DOF blur prefilter.

Texture2D sceneTex       : register(t0);
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
    float  _pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 d = texelSize * 0.5;

    float3 s;
    s  = sceneTex.SampleLevel(linearClamp, uv + float2(-d.x, -d.y), 0).rgb;
    s += sceneTex.SampleLevel(linearClamp, uv + float2( d.x, -d.y), 0).rgb;
    s += sceneTex.SampleLevel(linearClamp, uv + float2(-d.x,  d.y), 0).rgb;
    s += sceneTex.SampleLevel(linearClamp, uv + float2( d.x,  d.y), 0).rgb;

    return float4(s * 0.25, 1.0);
}
