Texture2D sceneTex : register(t0);
Texture2D blendTex : register(t1);
SamplerState linearSamp : register(s0);
SamplerState pointSamp  : register(s1);

cbuffer SMAACB : register(b0) {
    float4 rtMetrics;
    float lumaThreshold;
    float depthThreshold;
    int edgeMode;
    float pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Gather blend weights from current pixel and neighbors.
    //   a.x = right  neighbor's A (vertical-edge right blend)
    //   a.y = bottom  neighbor's G (horizontal-edge down blend)
    //   a.w = current pixel's  R (horizontal-edge up blend)
    //   a.z = current pixel's  B (vertical-edge left blend)
    float4 a;
    a.x  = blendTex.SampleLevel(pointSamp, uv + float2( rtMetrics.x, 0), 0).a;
    a.y  = blendTex.SampleLevel(pointSamp, uv + float2(0,  rtMetrics.y), 0).g;
    a.wz = blendTex.SampleLevel(pointSamp, uv, 0).rb;

    if (dot(a, float4(1, 1, 1, 1)) < 1e-5)
        return sceneTex.SampleLevel(linearSamp, uv, 0);

    bool h = max(a.x, a.z) > max(a.y, a.w);

    float2 blendDir;
    float  blendWeight;

    if (h) {
        blendWeight = max(a.x, a.z);
        blendDir = float2((a.z > a.x) ? -1.0 : 1.0, 0);
    } else {
        blendWeight = max(a.y, a.w);
        blendDir = float2(0, (a.w > a.y) ? -1.0 : 1.0);
    }

    float2 blendCoord = uv + blendDir * blendWeight * rtMetrics.xy;
    return sceneTex.SampleLevel(linearSamp, blendCoord, 0);
}
