Texture2D sceneTex : register(t0);
Texture2D depthTex : register(t1);
SamplerState pointSamp : register(s0);

cbuffer SMAACB : register(b0) {
    float4 rtMetrics;
    float lumaThreshold;
    float depthThreshold;
    int edgeMode;
    float pad;
};

float Luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 edges = float2(0, 0);

    if (edgeMode == 0 || edgeMode == 2) {
        float L      = Luma(sceneTex.SampleLevel(pointSamp, uv, 0).rgb);
        float Lleft  = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(-rtMetrics.x, 0), 0).rgb);
        float Ltop   = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(0, -rtMetrics.y), 0).rgb);

        float2 delta = abs(L.xx - float2(Lleft, Ltop));

        float Lright     = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(rtMetrics.x, 0), 0).rgb);
        float Lbottom    = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(0, rtMetrics.y), 0).rgb);
        float Lleftleft  = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(-2.0 * rtMetrics.x, 0), 0).rgb);
        float Ltoptop    = Luma(sceneTex.SampleLevel(pointSamp, uv + float2(0, -2.0 * rtMetrics.y), 0).rgb);

        float2 maxDelta = max(delta, abs(float2(Lright, Lbottom) - L));
        maxDelta = max(maxDelta, abs(float2(Lleftleft, Ltoptop) - float2(Lleft, Ltop)));

        float adapted = max(lumaThreshold, max(maxDelta.x, maxDelta.y) * 0.5);
        edges = step(adapted.xx, delta);
    }

    if (edgeMode == 1) {
        float D     = depthTex.SampleLevel(pointSamp, uv, 0).r;
        float Dleft = depthTex.SampleLevel(pointSamp, uv + float2(-rtMetrics.x, 0), 0).r;
        float Dtop  = depthTex.SampleLevel(pointSamp, uv + float2(0, -rtMetrics.y), 0).r;
        edges = step(depthThreshold.xx, abs(D.xx - float2(Dleft, Dtop)));
    }

    if (edgeMode == 2) {
        float D     = depthTex.SampleLevel(pointSamp, uv, 0).r;
        float Dleft = depthTex.SampleLevel(pointSamp, uv + float2(-rtMetrics.x, 0), 0).r;
        float Dtop  = depthTex.SampleLevel(pointSamp, uv + float2(0, -rtMetrics.y), 0).r;
        edges = max(edges, step(depthThreshold.xx, abs(D.xx - float2(Dleft, Dtop))));
    }

    if (dot(edges, 1) == 0)
        discard;

    return float4(edges, 0, 0);
}
