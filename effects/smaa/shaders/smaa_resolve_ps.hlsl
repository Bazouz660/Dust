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
    float4 a;
    a.x  = blendTex.SampleLevel(pointSamp, uv + float2( rtMetrics.x, 0), 0).a;
    a.y  = blendTex.SampleLevel(pointSamp, uv + float2(0,  rtMetrics.y), 0).g;
    a.wz = blendTex.SampleLevel(pointSamp, uv, 0).rb;

    if (dot(a, float4(1, 1, 1, 1)) < 1e-5)
        return sceneTex.SampleLevel(linearSamp, uv, 0);

    bool h = max(a.x, a.z) > max(a.y, a.w);

    float4 blendingOffset;
    float2 blendingWeight;

    [flatten] if (h) {
        blendingOffset = float4(a.x, 0.0, a.z, 0.0);
        blendingWeight = a.xz;
    } else {
        blendingOffset = float4(0.0, a.y, 0.0, a.w);
        blendingWeight = a.yw;
    }

    blendingWeight /= dot(blendingWeight, float2(1.0, 1.0));

    float4 blendingCoord = mad(blendingOffset,
        float4(rtMetrics.xy, -rtMetrics.xy), uv.xyxy);

    float4 color = blendingWeight.x * sceneTex.SampleLevel(linearSamp, blendingCoord.xy, 0);
    color += blendingWeight.y * sceneTex.SampleLevel(linearSamp, blendingCoord.zw, 0);
    return color;
}
