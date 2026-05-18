// Core AO computation pass.
// Operates on deinterleaved depth (ZSrc) and writes raw AO + signed depth
// marker to RawOccTex at deinterleaved positions.

#include "ao_pipeline.hlsli"

Texture2D<float> ZSrc          : register(t0);
Texture2D<float> DepthInput    : register(t1);
SamplerState     PointSamp     : register(s0);

struct VSOUT
{
    float4 vpos : SV_Position;
    float2 uv   : TEXCOORD0;
};

float2 EvalOcclusion(uint2 screenpos, float4 uv)
{
    float z = ZSrc.SampleLevel(PointSamp, uv.xy, 0);
    float d = z_to_depth(z);

    [branch]
    if (get_fade_factor(d) < 0.001)
        return float2(1.0, d);

    float3 p = uv_to_proj(uv.zw, z);
    float edge_weight;
    float3 n = get_normals(uv.zw, edge_weight, DepthInput, PointSamp);
    p = p * 0.996;
    float3 v = normalize(-p);

    float invTile = 1.0 / (float)DeinterleaveTileCount;
    float4 texture_scale = float4(invTile, invTile, 1.0, 1.0) * BufferAspectRatio.xyxy;

    uint slice_count  = samples_per_preset[SampleQualityPreset].x;
    uint sample_count = samples_per_preset[SampleQualityPreset].y;

    float jitter = get_jitter(screenpos);

    float3 slice_dir = 0;
    sincos(jitter * PI * (6.0 / slice_count), slice_dir.x, slice_dir.y);
    float2x2 rotslice;
    sincos(PI / slice_count, rotslice._21, rotslice._11);
    rotslice._12 = -rotslice._21;
    rotslice._22 =  rotslice._11;

    float worldspace_radius  = SampleRadius * 0.5;
    float screenspace_radius = worldspace_radius / p.z * 0.5;

    [flatten]
    if (WorldspaceEnable > 0.5)
    {
        screenspace_radius = SampleRadius * 0.03;
        worldspace_radius  = screenspace_radius * p.z * 2.0;
    }

    float visibility = 0;
    float slicesum   = 0;
    float T = log(1.0 + worldspace_radius) * 0.3333;

    float falloff_factor = rcp(max(worldspace_radius, 1e-5));
    falloff_factor *= falloff_factor;

    float2 vcrossn_xy = float2(v.y * n.z - v.z * n.y, v.z * n.x - v.x * n.z);
    float  ndotv      = dot(n, v);

    [loop]
    while (slice_count-- > 0)
    {
        slice_dir.xy = mul(slice_dir.xy, rotslice);
        float4 scaled_dir = (slice_dir.xy * screenspace_radius).xyxy * texture_scale;

        float sdotv  = dot(slice_dir.xy, v.xy);
        float sdotn  = dot(slice_dir.xy, n.xy);
        float ndotns = dot(slice_dir.xy, vcrossn_xy) * rsqrt(saturate(1.0 - sdotv * sdotv) + 1e-6);

        float sliceweight = sqrt(saturate(1.0 - ndotns * ndotns));
        float cosn        = saturate(ndotv * rcp(sliceweight + 1e-6));
        float normal_angle = fast_acos(cosn);
        normal_angle = (sdotn < sdotv * ndotv) ? -normal_angle : normal_angle;

        float2 maxhorizoncos = sin(normal_angle);
        maxhorizoncos.y = -maxhorizoncos.y;
        bitfield_init();

        [unroll]
        for (int side = 0; side < 2; side++)
        {
            maxhorizoncos = maxhorizoncos.yx;
            float lowesthorizoncos = maxhorizoncos.x;

            [loop]
            for (int _sample = 0; _sample < (int)sample_count; _sample += 2)
            {
                float2 s = (float(_sample) + float2(0.0, 1.0) + jitter) / (float)sample_count;
                s *= s;

                float4 tap_uv0 = uv + s.x * scaled_dir;
                float4 tap_uv1 = uv + s.y * scaled_dir;

                if (!all(saturate(tap_uv1.zw - tap_uv1.zw * tap_uv1.zw))) break;

                float2 zz;
                zz.x = ZSrc.SampleLevel(PointSamp, tap_uv0.xy, 0);
                zz.y = ZSrc.SampleLevel(PointSamp, tap_uv1.xy, 0);

                [unroll]
                for (uint pair = 0; pair < 2; pair++)
                {
                    float4 tap_uv = (pair == 0) ? tap_uv0 : tap_uv1;
                    float  zs     = (pair == 0) ? zz.x    : zz.y;

                    float3 deltavec = uv_to_proj(tap_uv.zw, zs) - p;

                    if (AoType < 2)
                    {
                        float ddotd = dot(deltavec, deltavec);
                        float samplehorizoncos = dot(deltavec, v) * rsqrt(max(ddotd, 1e-10));
                        float falloff = rcp(1.0 + ddotd * falloff_factor);
                        samplehorizoncos = lerp(lowesthorizoncos, samplehorizoncos, falloff);
                        maxhorizoncos.x = max(maxhorizoncos.x, samplehorizoncos);
                    }
                    else
                    {
                        float ddotv = dot(deltavec, v);
                        float ddotd = dot(deltavec, deltavec);
                        float2 h_frontback = float2(ddotv, ddotv - T) *
                            rsqrt(max(float2(ddotd, ddotd - 2.0 * T * ddotv + T * T), 1e-10));
                        h_frontback = fast_acos(h_frontback);
                        h_frontback = (side != 0) ? h_frontback : -h_frontback.yx;
                        h_frontback = saturate((h_frontback + normal_angle) / PI + 0.5);
                        if (AoType == 2)
                            h_frontback = h_frontback * h_frontback * (3.0 - 2.0 * h_frontback);
                        process_horizons(h_frontback);
                    }
                }
            }
            scaled_dir = -scaled_dir;
        }

        if (AoType == 0)
        {
            float2 max_horizon_angle = fast_acos(maxhorizoncos);
            float2 h = float2(-max_horizon_angle.x, max_horizon_angle.y);
            visibility += dot(cosn + 2.0 * h * sin(normal_angle) - cos(2.0 * h - normal_angle), sliceweight);
            slicesum   += 1.0;
        }
        else if (AoType == 1)
        {
            float2 max_horizon_angle = fast_acos(maxhorizoncos);
            visibility += dot(max_horizon_angle, sliceweight);
            slicesum   += sliceweight;
        }
        else
        {
            visibility += integrate_sectors() * sliceweight;
            slicesum   += sliceweight;
        }
    }

    if (AoType == 0)
        visibility /= max(slicesum * 4.0, 1e-5);
    else if (AoType == 1)
        visibility /= max(slicesum * PI, 1e-5);
    else
        visibility /= max(slicesum, 1e-5);

    // Edge confidence drives filter sharpness later (negative depth = blurry).
    float2 res = float2(saturate(visibility), edge_weight > 0.5 ? -d : d);
    return res;
}

float4 main(VSOUT i) : SV_Target
{
    uint2 dispatchthreadid = (uint2)floor(i.vpos.xy);
    uint  tile  = (uint)DeinterleaveTileCount;
    uint2 write_pos = reinterleave_pos(dispatchthreadid, tile, (uint2)BufferScreenSize);
    uint2 tilesize  = ((uint2)BufferScreenSize + tile - 1) / tile;
    uint2 tile_idx  = dispatchthreadid / tilesize;

    if (shading_rate(tile_idx))
        discard;

    float4 uv;
    uv.xy = pixel_idx_to_uv((float2)dispatchthreadid, BufferScreenSize);
    uv.zw = deinterleave_uv(uv.xy);

    float2 o = EvalOcclusion(write_pos, uv);

    // AO intensity blending: lerp toward 1 by amount; if >1 do gamma boost.
    float amt = saturate(SsaoAmount);
    o.x = lerp(1.0, o.x, amt);
    if (SsaoAmount > 1.0)
        o.x = lerp(o.x, o.x * o.x, saturate(SsaoAmount - 1.0));
    o.x = lerp(1.0, o.x, get_fade_factor(o.y));

    // RG16F: (filtered AO, signed depth marker)
    // BA channels store x^2 for the filter regression.
    return float4(o.x, o.y, o.x * o.x, o.y * o.y);
}
