Texture2D edgeTex : register(t0);
SamplerState pointSamp : register(s0);

cbuffer SMAACB : register(b0) {
    float4 rtMetrics;
    float lumaThreshold;
    float depthThreshold;
    int edgeMode;
    float pad;
};

#define MAX_SEARCH_STEPS 16

float SearchXLeft(float2 uv) {
    float d = 0;
    [loop] for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        float2 s = uv + float2(-(d + 1.0) * rtMetrics.x, 0);
        float2 e = edgeTex.SampleLevel(pointSamp, s, 0).rg;
        if (e.g < 0.5) break;
        d += 1.0;
        if (e.r > 0.5) break;
    }
    return d;
}

float SearchXRight(float2 uv) {
    float d = 0;
    [loop] for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        float2 s = uv + float2((d + 1.0) * rtMetrics.x, 0);
        float2 e = edgeTex.SampleLevel(pointSamp, s, 0).rg;
        if (e.g < 0.5) break;
        d += 1.0;
        if (e.r > 0.5) break;
    }
    return d;
}

float SearchYUp(float2 uv) {
    float d = 0;
    [loop] for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        float2 s = uv + float2(0, -(d + 1.0) * rtMetrics.y);
        float2 e = edgeTex.SampleLevel(pointSamp, s, 0).rg;
        if (e.r < 0.5) break;
        d += 1.0;
        if (e.g > 0.5) break;
    }
    return d;
}

float SearchYDown(float2 uv) {
    float d = 0;
    [loop] for (int i = 0; i < MAX_SEARCH_STEPS; i++) {
        float2 s = uv + float2(0, (d + 1.0) * rtMetrics.y);
        float2 e = edgeTex.SampleLevel(pointSamp, s, 0).rg;
        if (e.r < 0.5) break;
        d += 1.0;
        if (e.g > 0.5) break;
    }
    return d;
}

float2 Area(float d1, float d2, float e1, float e2) {
    float L = d1 + d2;
    if (L < 1e-5) return float2(0, 0);

    // Sub-pixel position of the real geometric edge relative to the pixel boundary.
    // Positive h: edge is on the neighbor side -> neighbor needs blending.
    // Negative h: edge is on the current pixel side -> current pixel needs blending.
    float h = 0.5 * (e1 * d2 - e2 * d1) / L;

    // .x = current pixel blends toward neighbor (when h < 0)
    // .y = neighbor blends toward current pixel (when h > 0)
    return sqrt(float2(saturate(-h), saturate(h)));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 weights = float4(0, 0, 0, 0);
    float2 e = edgeTex.SampleLevel(pointSamp, uv, 0).rg;

    [branch] if (e.g > 0.5) {
        float d1 = SearchXLeft(uv);
        float d2 = SearchXRight(uv);

        float e1 = edgeTex.SampleLevel(pointSamp,
            uv + float2((-d1 - 1.0) * rtMetrics.x, rtMetrics.y), 0).r;
        float e2 = edgeTex.SampleLevel(pointSamp,
            uv + float2((d2 + 1.0) * rtMetrics.x, rtMetrics.y), 0).r;

        weights.rg = Area(d1, d2, e1, e2);
    }

    [branch] if (e.r > 0.5) {
        float d1 = SearchYUp(uv);
        float d2 = SearchYDown(uv);

        float e1 = edgeTex.SampleLevel(pointSamp,
            uv + float2(rtMetrics.x, -d1 * rtMetrics.y), 0).g;
        float e2 = edgeTex.SampleLevel(pointSamp,
            uv + float2(rtMetrics.x, (d2 + 1.0) * rtMetrics.y), 0).g;

        weights.ba = Area(d1, d2, e1, e2);
    }

    return weights;
}
