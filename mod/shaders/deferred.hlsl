// [Dust] Modified deferred lighting shader — adds ambient occlusion to indirect lighting only.
// Based on vanilla Kenshi deferred.hlsl. AO texture is bound to register s8 by the Dust framework.
// If no AO texture is bound, the sampler returns white (1.0) via the fallback texture.

#include "constants.hlsl"
#include "gbuffer.hlsl"
#include "lightingFunctions.hlsl"
#include "shadowFunctions.hlsl"
#include "rtwshadows.hlsl"

void main_vs (
	float4 pos           : POSITION,
	out float4 oPos      : POSITION,
	out float2 oTexCoord : TEXCOORD0,
	out float3 oRay      : TEXCOORD1,
	out float2 oScreen   : TEXCOORD2,
	uniform float3 corner1,
	uniform float3 corner2
	)
{
	// Clean up inaccuracies
	pos.xy = sign(pos.xy);
	oPos = float4(pos.xy, 0, 1);
	oScreen = pos.xy;

	// Image-space
	oTexCoord.x = 0.5 * (1 + pos.x);
	oTexCoord.y = 0.5 * (1 - pos.y);

	// Ray from the camera to the far clip plane, per pixel
	//oRay = farCorner.xyz * float3(pos.xy, 1); // This version assumes symmetric frustum
	float3 pc = float3(pos.xy*0.5+0.5,1);
	oRay = lerp(corner2.xyz, corner1.xyz, pc);
}

float3 DebugOutput(
	float3 color,
	int mode,
	float3 albedo,
	float3 normal,
	float gloss,
	float shadow,
	float depth,
	float3 sun_diffuse,
	float3 sun_specular,
	float3 env_diffuse,
	float3 env_specular,
	float metalness
	)
{
	float3 all = albedo + normal + gloss.rrr + shadow.rrr + depth.rrr + sun_diffuse + sun_specular + env_diffuse + env_specular + metalness.rrr;
	all = min(0.00001f, all+1.0f);

	// 1 Debug: albedo
	// 2 Debug: normal
	// 3 Debug: gloss
	// 4 Debug: shadow
	// 5 Debug: depth
	// 6 Debug: sun_diffuse
	// 7 Debug: sun_specular
	// 8 Debug: env_diffuse
	// 9 Debug: env_specular
	// 10 Debug: sun light
	// 11 Debug: env light
	// 12 Debug: all light
	// 13 Debug: metalness

	if (mode == 1) 		color = albedo;
	else if (mode == 2) color = normal*0.5f + 0.5f;
	else if (mode == 3) color = gloss.rrr;
	else if (mode == 4) color = shadow.rrr;
	else if (mode == 5) color = depth.rrr;
	else if (mode == 6) color = sun_diffuse;
	else if (mode == 7) color = sun_specular;
	else if (mode == 8) color = env_diffuse;
	else if (mode == 9) color = env_specular;
	else if (mode == 10) color = sun_diffuse + sun_specular;
	else if (mode == 11) color = env_diffuse + env_specular;
	else if (mode == 12) color = sun_diffuse + env_diffuse + sun_specular + env_specular;
	else if (mode >= 13) color = metalness.rrr;

	return color + all;
}

// Main lighting - sun + ambient
float4 main_fs (
	float4 pixel 							: VPOS,
	float2 texCoord 						: TEXCOORD0,
	float3 ray 								: TEXCOORD1,
	uniform sampler gBuf0 					: register(s0),
	uniform sampler gBuf1 					: register(s1),
	uniform sampler gBuf2 					: register(s2),
	uniform sampler ambientMap			: register(s3),

	uniform float3 sunDirection,
	uniform float4 sunColour,
	uniform float4 pFogParams,
	uniform float4 envColour,
	uniform float4 worldSize,
	uniform float3 worldOffset,

	uniform float4 viewport,
	uniform float4 offset,
	uniform float4x4 inverseView,

	#ifdef CSM
		uniform float4 shadowParams,
		uniform float4 csmParams[SHADOW_MAP_COUNT],
		uniform float4 csmScale[SHADOW_MAP_COUNT],
		uniform float4 csmTrans[SHADOW_MAP_COUNT],
		uniform float4 csmUvBounds[SHADOW_MAP_COUNT],
		uniform float4x4 shadowViewMat,
		uniform sampler2D shadowJitterMap 	: register(s4),
		uniform sampler2D shadowDepthMap 	: register(s5),
		uniform samplerCUBE irradianceCube 	: register(s6),
		uniform samplerCUBE specularityCube : register(s7),
	#elif RTW
		uniform float4x4  shadow_matrix,
		uniform float     shadow_bias,
		uniform float     shadow_range,
		uniform sampler2D shadowWarpMap 	: register(s4),
		uniform sampler2D shadowDepthMap 	: register(s5),
		uniform samplerCUBE irradianceCube 	: register(s6),
		uniform samplerCUBE specularityCube : register(s7),
	#else	// NO SHADOW
		uniform samplerCUBE irradianceCube 	: register(s4),
		uniform samplerCUBE specularityCube : register(s5),
	#endif

	// [Dust] Ambient occlusion texture — bound by Dust framework at runtime
	uniform sampler aoMap 				: register(s8),

	uniform float4 ambientParams,
	uniform float4x4 proj,

	out float oDepth : DEPTH

	) : COLOR
{
	// Fix jitter
	texCoord -= offset.zw;

	// gBuf0
	float3 albedo = decodePixel(gBuf0, texCoord, viewport, pixel.xy);
	float2 metalness_gloss = tex2D(gBuf0, texCoord).ba;
	float metalness = metalness_gloss.x;
	float gloss = metalness_gloss.y;

	// gBuf1
	float4 normal_n_emissive = tex2D(gBuf1, texCoord);
	float3 normal = normalize(normal_n_emissive.xyz * 2.0f - 1.0f);
	float emissive = normal_n_emissive.w * 3.2;

	// gBuf2
	float depth = tex2D(gBuf2, texCoord).r;
	clip( depth - 0.00001f );

	// unpack translucency from emissive ?
	float translucency = normal_n_emissive.w < 0.5? normal_n_emissive.w * 2.0: 0.0;
	emissive = max(0.0, normal_n_emissive.w - 0.5) * 6.4;


	// Calculate position of texel in view space
	float distance = depth * pFogParams.x; // ??
	float4 viewPos  = float4( normalize(ray) * distance, 1.0f);
	float3 worldPos = mul( viewPos, inverseView ).xyz;

	// Depth
	float4 projPos = mul( proj, viewPos );
	oDepth = projPos.z / projPos.w;

	// Ambient map data
	float2 mapCoord = (worldPos.xz + worldOffset.xz) * worldSize.xy + worldSize.zw;
	float4 ambientMult =  tex2D(ambientMap, mapCoord);

	// Shadow
	float shadow = 1.0f;

	#ifdef RTW
		float edgeBias = saturate((distance - shadow_range * 0.6) * 0.001);
		shadow = RTWShadow( shadowDepthMap, shadowWarpMap, shadow_matrix, worldPos, shadow_bias, edgeBias );
		//shadow = lerp(1.0, shadow, saturate((shadow_range-distance) * 0.01)); // shadow fade ?

	#elif CSM
		float3 debugColorMask;
		shadowParams[2] = 0; // Shadow ambient level
		shadow = computeShadowMultiplier(
			shadowParams,
			shadowViewMat,
			csmScale,
			csmTrans,
			csmParams,
			csmUvBounds,
			shadowDepthMap,
			shadowJitterMap,
			float4( worldPos, 1.0f ),
			float4( 0.0f, 0.0f, projPos.z, 0.0f ),		// screenPos only needs fragment depth
			normal,
			debugColorMask
		);

	#endif

	sunColour.rgb *= sunColour.w * ambientParams.w * ambientMult.w*2.0;

	// Light function params
	float3 viewDir = -normalize( worldPos );
	float3 lightDir = normalize(sunDirection);
	float3 lightColor = sunColour.rgb;

	// Metalness
	float3 albedoIn = albedo;
	float3 specColor = lerp( dielectric_spec, albedo, metalness );
	albedo = lerp( albedo, 0.0f, metalness );

	LightingData sunLight = CalcPunctualLight( normal, lightDir, viewDir, gloss, specColor, lightColor * shadow, translucency );
	LightingData envLight = CalcEnvironmentLight( irradianceCube, specularityCube, normal, viewDir, gloss, specColor );

	envColour.w *= clamp(lightDir.y*5.0 + 0.2f, 0.1f, 1.0f);
	envLight.diffuse *= ambientMult.rgb * envColour.w;
	envLight.specular *= ambientMult.rgb * envColour.w;

	// [Dust] Apply ambient occlusion to indirect/environment lighting only.
	// AO does not affect direct sunlight — only ambient and IBL contributions.
	// This ensures AO visibility is consistent regardless of auto-exposure.
	float ao = tex2D(aoMap, texCoord).r;
	envLight.diffuse *= ao;
	envLight.specular *= ao;

	LightingData ld = (LightingData)0.0f;
	ld.diffuse = sunLight.diffuse + envLight.diffuse;
	ld.specular = sunLight.specular + envLight.specular;

	float3 color = ld.specular + ld.diffuse * albedo + albedo*emissive;

	#ifdef MODE
		color = DebugOutput(color, MODE, albedoIn, normal, gloss, shadow, depth, sunLight.diffuse, sunLight.specular, envLight.diffuse, envLight.specular, metalness);
	#endif

//	color.rg = mapCoord;
//	color.b *= 0.001;

	return float4(color, 1.0f);
}

// ============================================================================================= //

void light_vs(
	float4 pos           : POSITION,
	out float4 oPos      : POSITION,
	uniform float4x4 worldViewProjMatrix
	)
{
	// Transform to screen coordinates
	oPos = mul(worldViewProjMatrix, pos);

	// Image-space
	//oTexCoord.x = 0.5 * (1 + oPos.x);
	//oTexCoord.y = 0.5 * (1 - oPos.y);

	// Ray from the camera to the far clip plane, per pixel
	//float3 pc = float3(oPos.xy * 0.5 + 0.5, 1);
	//oRay = lerp(farCorner2.xyz, farCorner.xyz, pc);
}

float4 light_fs(
	float4 pixel     : VPOS,
	uniform sampler gBuf0   : register(s0),
	uniform sampler gBuf1   : register(s1),
	uniform sampler gBuf2   : register(s2),

	uniform float3 diffuseColour,
	uniform float3 specularColour,
	uniform float4 falloff, // constant, linear, quadratic, radius
	uniform float3 position,
	uniform float power,

	#ifdef SPOTLIGHT
	uniform float3 direction,
	uniform float3 spot,
	#endif

	uniform float farClip,
	uniform float4 offset,
	uniform float4 viewport,
	uniform float3 farCorner,
	uniform float3 farCorner2,
	uniform float4x4 viewMatrix,
	uniform float4x4 proj
) : COLOR {

	// Calculate texture coordinates from viewport
	float2 texCoord = pixel.xy * viewport.zw;
	texCoord -= offset.zw;

	// gBuf0
	float3 albedo = decodePixel(gBuf0, texCoord, viewport, pixel.xy);
	float2 metalness_gloss = tex2D(gBuf0, texCoord).ba;
	float metalness = metalness_gloss.x;
	float gloss = metalness_gloss.y;

	// gBuf1
	float4 normal_n_emissive = tex2D(gBuf1, texCoord);
	float3 normal = normalize(normal_n_emissive.xyz * 2.0f - 1.0f);
	float emissive = normal_n_emissive.w;

	// gBuf2
	float depth = tex2D(gBuf2, texCoord).r;
	clip( depth - 0.00001f );

	// Calculate position of texel in view space
	float distance = depth * farClip;
	float3 ray = lerp(farCorner2, farCorner, float3(texCoord.x, 1.0f - texCoord.y, 1.0f));
	float4 viewPos  = float4( normalize(ray) * distance, 1.0f);
	float3 worldPos = mul( viewPos, viewMatrix ).xyz;
	float3 lightDir = position - worldPos;
	distance = length( lightDir );
	lightDir /= distance;

	clip( falloff.w - distance );
	clip( dot( normal, lightDir ) - 0.01f );

	// Attenuation
	float x = saturate( distance / falloff.w );
	float start = pow( 1.0f - x, 3 ) * 0.8f + 0.2f;
	float vb = -3 * pow( x - 0.6f, 2 ) + 0.242f;
	float vc =  3 * pow( x - 1.0f, 2 );
	float attenuation = x < 0.649f ? start : vb;
	attenuation = x < 0.8f ? attenuation : vc;


	#ifdef SPOTLIGHT
		float a = saturate( dot( direction, -lightDir ) );
		float spotFactor = pow( saturate( a - spot.y ) / ( spot.x - spot.y ), spot.z );
		attenuation *= saturate(spotFactor);
	#endif

	float3 viewDir = -normalize( worldPos );
	float3 lightColor = diffuseColour;

	// Metalness
	float3 specColor = lerp( dielectric_spec, albedo, metalness );
	albedo = lerp( albedo, 0.0f, metalness );

	LightingData ld = CalcPunctualLight( normal, lightDir, viewDir, gloss, specColor, lightColor );

	float3 color = ld.specular + ld.diffuse * albedo;
	color = color * attenuation * power;

	#ifdef MODE
	if(MODE>0 && MODE != 12) return float4(0,0,0,0);
	#endif

	return float4(color, 1.0f);
}
