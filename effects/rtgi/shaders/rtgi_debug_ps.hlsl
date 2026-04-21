// RTGI Debug Visualization
// Displays GI components and input buffers for debugging.

Texture2D<float4> giTex      : register(t0);
Texture2D<float>  depthTex   : register(t1);
Texture2D<float4> normalsTex : register(t2);
SamplerState pointClamp : register(s0);

cbuffer DebugParams : register(b0)
{
    float debugMode;
    float tanHalfFov;
    float aspectRatio;
    float _pad0;
    // Camera basis vectors from inverseView rows — plain float4 to avoid
    // any row_major/column_major ambiguity. Each is a camera axis in world space.
    float4 camRight;
    float4 camUp;
    float4 camForward;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    int mode = (int)debugMode;

    if (mode == 1)
    {
        float4 gi = giTex.SampleLevel(pointClamp, uv, 0);
        return float4(gi.rgb, 1.0);
    }
    else if (mode == 2)
    {
        float4 gi = giTex.SampleLevel(pointClamp, uv, 0);
        return float4(gi.aaa, 1.0);
    }
    else if (mode == 3)
    {
        float4 gi = giTex.SampleLevel(pointClamp, uv, 0);
        return float4(gi.rgb * gi.a, 1.0);
    }
    else if (mode == 4)
    {
        // GBuffer world-space normals
        float3 n = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0);
        return float4(n * 0.5 + 0.5, 1.0);
    }
    else if (mode == 5)
    {
        // View-space normals (GBuffer transformed via camera basis)
        float3 wn = normalize(normalsTex.SampleLevel(pointClamp, uv, 0).rgb * 2.0 - 1.0);
        float3 v;
        v.x =  dot(wn, camRight.xyz);
        v.y =  dot(wn, camUp.xyz);
        v.z = -dot(wn, camForward.xyz);
        v = normalize(v);
        return float4(v * 0.5 + 0.5, 1.0);
    }
    else if (mode == 6)
    {
        float d = depthTex.SampleLevel(pointClamp, uv, 0);
        float vis = saturate(d * 2.0);
        return float4(vis, vis, vis, 1.0);
    }
    else
    {
        float4 gi = giTex.SampleLevel(pointClamp, uv, 0);
        return float4(gi.rgb, 1.0);
    }
}
