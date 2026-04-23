Texture2D edgeTex   : register(t0);
Texture2D areaTex   : register(t1);
Texture2D searchTex : register(t2);

SamplerState pointSamp  : register(s0);
SamplerState linearSamp : register(s1);

cbuffer SMAACB : register(b0) {
    float4 rtMetrics;
    float lumaThreshold;
    float depthThreshold;
    int edgeMode;
    float pad;
};

#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)

float SMAASearchLength(float2 e, float offset) {
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);
    scale += float2(-1.0, 1.0);
    bias += float2(0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return searchTex.SampleLevel(linearSamp, mad(scale, e, bias), 0).r;
}

float SMAASearchXLeft(float2 texcoord, float end) {
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0) {
        e = edgeTex.SampleLevel(linearSamp, texcoord, 0).rg;
        texcoord = mad(-float2(2.0, 0.0), rtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.0), 3.25);
    return mad(rtMetrics.x, offset, texcoord.x);
}

float SMAASearchXRight(float2 texcoord, float end) {
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0) {
        e = edgeTex.SampleLevel(linearSamp, texcoord, 0).rg;
        texcoord = mad(float2(2.0, 0.0), rtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e, 0.5), 3.25);
    return mad(-rtMetrics.x, offset, texcoord.x);
}

float SMAASearchYUp(float2 texcoord, float end) {
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0) {
        e = edgeTex.SampleLevel(linearSamp, texcoord, 0).rg;
        texcoord = mad(-float2(0.0, 2.0), rtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.0), 3.25);
    return mad(rtMetrics.y, offset, texcoord.y);
}

float SMAASearchYDown(float2 texcoord, float end) {
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0) {
        e = edgeTex.SampleLevel(linearSamp, texcoord, 0).rg;
        texcoord = mad(float2(0.0, 2.0), rtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(e.gr, 0.5), 3.25);
    return mad(-rtMetrics.y, offset, texcoord.y);
}

float2 SMAAArea(float2 dist, float e1, float e2) {
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE),
                          round(4.0 * float2(e1, e2)), dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    return areaTex.SampleLevel(linearSamp, texcoord, 0).rg;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 weights = float4(0, 0, 0, 0);
    float2 e = edgeTex.SampleLevel(pointSamp, uv, 0).rg;

    float2 pixcoord = uv * rtMetrics.zw;

    float4 off0 = mad(rtMetrics.xyxy, float4(-0.25, -0.125, 1.25, -0.125), uv.xyxy);
    float4 off1 = mad(rtMetrics.xyxy, float4(-0.125, -0.25, -0.125, 1.25), uv.xyxy);
    float4 off2 = mad(rtMetrics.xxyy,
        float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
        float4(off0.xz, off1.yw));

    [branch] if (e.g > 0.0) {
        float2 d;
        float3 coords;

        coords.x = SMAASearchXLeft(off0.xy, off2.x);
        coords.y = off1.y;
        d.x = coords.x;

        float e1 = edgeTex.SampleLevel(linearSamp, coords.xy, 0).r;

        coords.z = SMAASearchXRight(off0.zw, off2.y);
        d.y = coords.z;

        d = abs(round(mad(rtMetrics.zz, d, -pixcoord.xx)));
        float2 sqrt_d = sqrt(d);

        float e2 = edgeTex.SampleLevel(linearSamp, coords.zy, 0, int2(1, 0)).r;

        weights.rg = SMAAArea(sqrt_d, e1, e2);
    }

    [branch] if (e.r > 0.0) {
        float2 d;
        float3 coords;

        coords.y = SMAASearchYUp(off1.xy, off2.z);
        coords.x = off0.x;
        d.x = coords.y;

        float e1 = edgeTex.SampleLevel(linearSamp, coords.xy, 0).g;

        coords.z = SMAASearchYDown(off1.zw, off2.w);
        d.y = coords.z;

        d = abs(round(mad(rtMetrics.ww, d, -pixcoord.yy)));
        float2 sqrt_d = sqrt(d);

        float e2 = edgeTex.SampleLevel(linearSamp, coords.xz, 0, int2(0, 1)).g;

        weights.ba = SMAAArea(sqrt_d, e1, e2);
    }

    return weights;
}
