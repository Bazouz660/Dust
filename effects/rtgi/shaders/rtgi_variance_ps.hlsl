// RTGI Variance Extraction Pass
// Reads the temporal metadata texture and extracts the encoded per-pixel
// variance into a dedicated variance texture for the A-Trous denoiser.
// This decouples the temporal data format from the denoise pipeline.

Texture2D<float4> metadataTex : register(t0);

cbuffer VarianceParams : register(b0)
{
    float2 viewportSize;
    float2 invViewportSize;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float encoded = metadataTex.Load(int3(int2(pos.xy), 0)).a;
    float variance = encoded * encoded / 128.0;
    return float4(variance, 0, 0, 0);
}
