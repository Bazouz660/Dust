# Deferred Lighting System

Source: `deferred.hlsl` (441 lines), `data/materials/common/lightingFunctions.hlsl`

## Sun Pass (`main_fs`)

| Property     | Value |
|--------------|-------|
| Draw index   | 3641 (pre-pass), 3648 (main lighting) |
| Type         | Fullscreen TRIANGLESTRIP (4 verts) / TRIANGLELIST (3 verts) |
| PS           | 0x6162EFF8 (`main_fs`), 2 variants across captures (day/night or CSM/RTW) |
| VS           | 0x6162EB38 (`main_vs`) |
| Output       | R11G11B10_FLOAT native |

### PS SRV Inputs

| Slot | Format              | Size       | Content |
|------|---------------------|------------|---------|
| 0    | B8G8R8A8_UNORM      | 2560x1440  | GBuffer RT0 (albedo+metalness+gloss) |
| 1    | B8G8R8A8_UNORM      | 2560x1440  | GBuffer RT1 (normal+emissive) |
| 2    | R32_FLOAT           | 2560x1440  | GBuffer RT2 (linear depth) |
| 3    | B8G8R8A8_UNORM      | 1024x1024  | Ambient map (world-space tint) |
| 4    | R32_FLOAT           | 513x2      | Shadow jitter / filter kernel |
| 5    | R32_FLOAT           | 4096x4096  | Shadow depth map (CSM atlas) |
| 6    | BC3_UNORM CUBE      | 16x16      | Irradiance cubemap |
| 7    | BC3_UNORM CUBE      | 256x256    | Specular cubemap |

### VS SRV Inputs (unusual — VS reads GBuffer directly)

| Slot | Format           | Size       | Content |
|------|------------------|------------|---------|
| 0    | B8G8R8A8_UNORM   | 2560x1440  | GBuffer RT0 |
| 1    | B8G8R8A8_UNORM   | 2560x1440  | GBuffer RT1 |
| 2    | R32_FLOAT        | 2560x1440  | GBuffer RT2 |
| 3    | B8G8R8A8_UNORM   | 1024x1024  | Ambient map |

This is an OGRE 2.0 pattern — the VS reconstructs world position from depth for the PS.

### PS Constant Buffer (slot 0, 352 bytes)

| Register | Example Values                                      | Semantic |
|----------|-----------------------------------------------------|----------|
| c0       | (-0.1237, 0.5833, 0.8028, 0.0)                     | `sunDirection` (xyz) |
| c1       | (1.4864, 1.3865, 1.3063, 0.7916)                   | `sunColour` (rgb), intensity |
| c2       | (50000.0, 3000.0, 30000.0, 0.0)                    | `pFogParams` (start, density, range) |
| c3       | (1.0, 1.0, 1.0, 1.0)                               | `envColour` |
| c4       | (0.000003, 0.000003, 0.5030, 0.4994)               | `worldSize` / `worldOffset` |
| c5       | (-49906.6, 0.0, 49036.3, 0.0)                      | World-space offset |
| c6       | (2560.0, 1440.0, 0.000391, 0.000694)               | `viewport` (res + texel size) |
| c7       | (0.0, 0.0, 0.0, 0.0)                               | (unused / padding) |
| c8-c11   | (rotation matrix + translation)                     | `inverseView` (4x3) |
| c12-c15  | (small values + 0.852, 0.248, 1.0)                 | `shadowParams`, `proj` |

Additional uniforms from shader source: `ambientParams`, `csmParams[4]`, `csmScale[4]`,
`csmTrans[4]`, `csmUvBounds[4]`, `shadowViewMat`

### VS Constant Buffer (slot 0, 32 bytes)

| Register | Example Values                                       | Semantic |
|----------|------------------------------------------------------|----------|
| c0       | (36818.99, 20710.68, -50000.0, -451.47)             | Far frustum corner 1 |
| c1       | (-36818.99, -20710.68, -50000.0, 0.00008)           | Far frustum corner 2 + near plane |

---

## BRDF — PBR (NOT Blinn-Phong)

Source: `data/materials/common/lightingFunctions.hlsl`

### Diffuse: Lambert with Fresnel Energy Conservation

```hlsl
ld.diffuse = PI * dotNL * lightColor * FresnelDiffuse(specColor);
// FresnelDiffuse(specColor) = saturate(1 - avg(specColor))
```

Disney/Burley diffuse is **implemented but commented out** in the same file:
```hlsl
//ld.diffuse = lightFactor * Fr_DisneyDiffuse(view, light, normal, roughness);
```

### Specular: GGX Microfacet (Trowbridge-Reitz)

```hlsl
ld.specular = lightColor * LightingFuncGGX_OPT3(N, V, L, roughness, F0) / PI;
```

| Term          | Function                        | Formula |
|---------------|---------------------------------|---------|
| D (NDF)       | GGX / Trowbridge-Reitz          | `alpha^2 / (PI * (dotNH^2 * (alpha^2-1) + 1)^2)` |
| F (Fresnel)   | Schlick, Horner-form             | `exp2((-5.55473*dotLH - 6.98316)*dotLH)` |
| V (Visibility)| Kelemen-Szirmay-Kalos            | `1 / (dotLH^2 * (1-k^2) + k^2)` where `k = alpha/2` |

### Roughness Mapping

```hlsl
float roughness = 1.0 - gloss * 0.99;  // gloss=0 -> rough=1.0, gloss=1 -> rough=0.01
float alpha = roughness * roughness;     // perceptual roughness to alpha
```

### Metalness Workflow

```hlsl
float3 specColor = lerp(dielectric_spec, albedo, metalness);  // F0
albedo = lerp(albedo, 0.0, metalness);                        // metals have no diffuse
```

### Translucency

Simple wrap lighting for translucent materials:
```hlsl
if (dotNL < 0) dotNL = lerp(0.0, -dotNL, translucency);
```

---

## Environment Lighting (IBL)

| Component          | Source              | Details |
|--------------------|---------------------|---------|
| Irradiance         | 16x16 cubemap (s6)  | Sampled at mip 3, decoded `rgb * a * 4.0` |
| Specular           | 256x256 cubemap (s7)| Mip = `(1-gloss) * 7.0`, decoded `rgb * a * 10.0` |
| Dominant direction | Frostbite approach   | `GetSpecularDominantDir()` |
| Environment BRDF   | Lazarov analytic fit | `LazarovEnvironmentBRDF(gloss, NdotV, F0)` (Black Ops 2) |

---

## Point & Spot Lights — Forward Light Volumes

Source: `deferred.hlsl` (`light_vs` / `light_fs`)

Point and spot lights render as **forward draws with light volume meshes** after the
fullscreen deferred pass. Each light = one draw call.

| Property        | Value |
|-----------------|-------|
| PS entry        | `light_fs` (0x63DB5878) |
| VS entry        | `quad_vs` (0x6394F578) |
| Topology        | TRIANGLELIST |
| Blending        | Additive onto R11G11B10_FLOAT HDR target |
| Lights per frame | 0-59 (no budget cap) |

### Light Volume Rendering

- `light_vs`: Transforms sphere/cone mesh with `worldViewProjMatrix`
- `light_fs`: Reads GBuffer (same gBuf0/1/2), computes per-pixel lighting
- Same GGX specular + metalness workflow as sun pass

### Attenuation Model (Custom Cubic, NOT Inverse-Square)

```hlsl
float x = saturate(distance / falloff.w);
float start = pow(1-x, 3) * 0.8 + 0.2;   // near: smooth cubic
float vb = -3*pow(x-0.6, 2) + 0.242;      // mid: parabolic bump
float vc = 3*pow(x-1.0, 2);                // far: quadratic fade
attenuation = x < 0.649 ? start : (x < 0.8 ? vb : vc);
```

Three-piece piecewise function with smooth transitions.

### Spotlight Extension

```hlsl
float spotFactor = pow(saturate((dot(direction, -lightDir) - spot.y) / (spot.x - spot.y)), spot.z);
```

### Light Uniforms (Per Draw)

`diffuseColour`, `specularColour`, `falloff` (constant, linear, quadratic, radius),
`position`, `power`, optional `direction` + `spot` for spotlights.

### Light Volume CB (slot 0, 32 bytes)

| Register | Values | Semantic |
|----------|--------|----------|
| c0       | (2560.0, 1440.0, 0.000391, 0.000694) | Resolution + texel size |
| c1       | (1.543, 1.0, 2560.0, 1440.0)         | tan(fovY/2)*aspect, 1.0, resolution |

---

## BRDF Upgrade Opportunities

1. **Easiest win**: Uncomment Disney/Burley diffuse (already implemented)
2. **Smith-GGX correlated**: Replace Kelemen-SzK visibility with height-correlated Smith GGX
3. **Multi-scatter**: Add Kulla-Conty energy compensation for rough metals
4. **Better IBL**: Replace Lazarov with Split-Sum or higher-quality analytic fit
5. **Inverse-square falloff**: Replace custom cubic attenuation with physical light falloff
