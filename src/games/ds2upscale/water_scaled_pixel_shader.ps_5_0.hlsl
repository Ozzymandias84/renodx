// Patched copy of the game's water pixel shader 0x938B36DA for render scale < 1 (generated from the decompilation;
// only the three marked lines differ). The scene is drawn into the top-left [0, scale] part of its render targets, so the
// reflection map is addressed with (scale - y) instead of (1 - y) and the ripple offsets are scaled with the picture.
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;  // xy: render size / full size
};

// ---- Created with 3Dmigoto v1.3.16 on Mon Oct 12 00:50:30 2026

cbuffer cbBase : register(b0)
{
  float4 FC_DifColMul : packoffset(c0);
  float FC_GlobalTime : packoffset(c1);
  row_major float3x4 FC_MatrixViewT : packoffset(c2);
  float4 FC_FarClipInfo : packoffset(c5);
  float4 FC_ShadowMapParam : packoffset(c6);
  float4 FC_ShadowColor : packoffset(c7);
  float4 FC_ScreenSize : packoffset(c8);
  float4 FC_FinalColorMult : packoffset(c9);
}

cbuffer cbLightEnv : register(b1)
{
  float4 FC_DirLightVec_0 : packoffset(c0);
  float4 FC_DirLightCol_0 : packoffset(c1);
  float4 FC_DirLightSpc_0 : packoffset(c2);
  float4 FC_AddHemiColor_0 : packoffset(c3);
  float4 FC_AddHemiColor_1 : packoffset(c4);
  float4 FC_FogParam : packoffset(c5);
  float4 FC_FogCol : packoffset(c6);
}

cbuffer cbMaterial : register(b2)
{
  float4 FC_DifMapMulCol : packoffset(c0);
  float4 FC_SpcMapMulCol : packoffset(c1);
  float4 FC_SpcParam : packoffset(c2);
  float4 FC_AlphaRef : packoffset(c3);
  float4 FC_ParallaxParam : packoffset(c4);
  float4 FC_BlendMaskParam : packoffset(c5);
  float4 FC_SkinMatchColor : packoffset(c6);
  float4 FC_HairMulColor : packoffset(c7);
  float4 FC_BreakParam : packoffset(c8);
  float4 FC_SSAOParam : packoffset(c9);
}

cbuffer cbMaterial2 : register(b3)
{
  float4 FC_RimLightingParam : packoffset(c0);
  float4 FC_FlowMapParam : packoffset(c1);
  float4 FC_FlowMapExOffset : packoffset(c2);
  float4 FC_HairSpecularParam0 : packoffset(c3);
  float4 FC_HairSpecularParam1 : packoffset(c4);
  float4 FC_HairSpecularParam2 : packoffset(c5);
  float FC_MetamorRate : packoffset(c6);
  float4 FC_MetamorEmissive : packoffset(c7);
  float4 FC_FacialMaskFactor0 : packoffset(c8);
  float4 FC_FacialMaskFactor1 : packoffset(c9);
  float4 FC_EmissiveColor : packoffset(c10);
  float4 FC_EmissiveColor2 : packoffset(c11);
  float4 FC_PhantomEdgeColor : packoffset(c12);
  float4 FC_PhantomFrontColor : packoffset(c13);
  float4 FC_PhantomDiffColor : packoffset(c14);
  float4 FC_PhantomSpecColor : packoffset(c15);
  float4 FC_PhantomLightColor : packoffset(c16);
  float4 FC_WetDiffuseColor : packoffset(c17);
  float4 FC_WetParam1 : packoffset(c18);
  float4 FC_WetParam2 : packoffset(c19);
  float4 FC_MagmaEmissiveColor : packoffset(c20);
  float4 FC_FlipBookParam0 : packoffset(c21);
  float4 FC_FlipBookParam1 : packoffset(c22);
  float3 FC_CDRFParam : packoffset(c23);
  float4 FC_WaterNormalUvScrollSpeed : packoffset(c24);
  float4 FC_WaterDiffuseUvScrollSpeed : packoffset(c25);
  float4 FC_WaterFogParam0 : packoffset(c26);
  float4 FC_WaterFogParam1 : packoffset(c27);
  float4 FC_WaterFresnel : packoffset(c28);
  float4 FC_WaterFadeParam : packoffset(c29);
}

cbuffer cbTexture : register(b4)
{
  float4 FC_TextureTilingScale[16] : packoffset(c0);
  float4 FC_TextureExInfo[4] : packoffset(c16);
}

cbuffer cbInstance : register(b5)
{
  float4 FC_AmbientSH_0 : packoffset(c0);
  float4 FC_AmbientSH_1 : packoffset(c1);
  float4 FC_AmbientSH_2 : packoffset(c2);
  float4 FC_CubeEnvSH_0 : packoffset(c3);
  float4 FC_CubeEnvSH_1 : packoffset(c4);
  float4 FC_CubeEnvSH_2 : packoffset(c5);
  float4 FC_InstanceColor : packoffset(c6);
  float4 FC_LightMask : packoffset(c7);
}

SamplerState g_DiffuseMap_sampler_s : register(s0);
SamplerState g_SpecularMap_sampler_s : register(s1);
SamplerState g_BumpMap_sampler_s : register(s2);
SamplerState g_DiffuseMap2_sampler_s : register(s3);
SamplerState g_SpecularMap2_sampler_s : register(s4);
SamplerState g_BumpMap2_sampler_s : register(s5);
SamplerState g_RefractionMap_sampler_s : register(s10);
SamplerState g_DepthMapSampler_sampler_s : register(s12);
SamplerState g_ReflectionMap_sampler_s : register(s13);
Texture2D<float4> g_DiffuseMap_texture : register(t0);
Texture2D<float4> g_SpecularMap_texture : register(t1);
Texture2D<float4> g_BumpMap_texture : register(t2);
Texture2D<float4> g_DiffuseMap2_texture : register(t3);
Texture2D<float4> g_SpecularMap2_texture : register(t4);
Texture2D<float4> g_BumpMap2_texture : register(t5);
Texture2D<float4> g_RefractionMap_texture : register(t10);
Texture2D<float4> g_DepthMapSampler_texture : register(t12);
Texture2D<float4> g_ReflectionMap_texture : register(t13);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  float4 v3 : TEXCOORD2,
  float4 v4 : TEXCOORD3,
  float4 v5 : TEXCOORD4,
  float4 v6 : TEXCOORD5,
  float4 v7 : TEXCOORD6,
  float4 v8 : COLOR0,
  float v9 : SV_ClipDistance0,
  uint v10 : SV_IsFrontFace0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = dot(v2.xyz, v2.xyz);
  r0.x = rsqrt(r0.x);
  r0.yzw = v2.xyz * r0.xxx + FC_DirLightVec_0.xyz;
  r1.xyz = v2.xyz * r0.xxx;
  r0.x = dot(r0.yzw, r0.yzw);
  r0.x = rsqrt(r0.x);
  r0.xyz = r0.yzw * r0.xxx;
  r0.w = dot(r1.xyz, r0.xyz);
  r0.w = saturate(1 + -r0.w);
  r1.w = r0.w * r0.w;
  r1.w = r1.w * r1.w;
  r0.w = r1.w * r0.w;
  r2.xyzw = FC_WaterNormalUvScrollSpeed.xyzw * FC_GlobalTime + v7.xyzw;
  r2.xy = r2.xy * FC_TextureTilingScale[2].xy + FC_TextureTilingScale[2].zw;
  r2.zw = r2.zw * FC_TextureTilingScale[5].xy + FC_TextureTilingScale[5].zw;
  r1.w = g_BumpMap2_texture.Sample(g_BumpMap2_sampler_s, r2.zw).z;
  r1.w = 1 + -r1.w;
  r2.x = g_BumpMap_texture.Sample(g_BumpMap_sampler_s, r2.xy).z;
  r2.x = 1 + -r2.x;
  r1.w = -r2.x * r1.w + 1;
  r2.x = -r1.w * r1.w + 1;
  r1.w = r1.w * FC_ParallaxParam.x + FC_ParallaxParam.y;
  r2.y = 1 + -v5.w;
  r2.x = -r2.x * r2.y + 1;
  r3.z = dot(v2.xyz, v1.xyz);
  r3.x = dot(v2.xyz, v4.xyz);
  r3.y = dot(v2.xyz, v3.xyz);
  r2.y = dot(r3.xyz, r3.xyz);
  r2.y = rsqrt(r2.y);
  r3.xyzw = r3.xyxy * r2.yyyy;
  r3.xyzw = r3.xyzw * r1.wwww + v7.xyzw;
  r2.yz = r3.zw * FC_TextureTilingScale[4].xy + FC_TextureTilingScale[4].zw;
  r2.yz = g_SpecularMap2_texture.Sample(g_SpecularMap2_sampler_s, r2.yz).xy;
  r4.xy = r3.xy * FC_TextureTilingScale[1].xy + FC_TextureTilingScale[1].zw;
  r4.xy = g_SpecularMap_texture.Sample(g_SpecularMap_sampler_s, r4.xy).xy;
  r1.w = r4.y * r4.y;
  r2.z = r2.z * r2.z + -r1.w;
  r2.y = -r4.x + r2.y;
  r2.y = r2.x * r2.y + r4.x;
  r2.y = FC_SpcParam.x * r2.y;
  r2.y = max(1, r2.y);
  r2.y = FC_SpcParam.z * r2.y;
  r1.w = r2.x * r2.z + r1.w;
  r1.w = saturate(FC_SpcParam.w * r1.w);
  r2.z = 1 + -r1.w;
  r0.w = r0.w * r2.z + r1.w;
  r4.xyzw = FC_WaterNormalUvScrollSpeed.xyzw * FC_GlobalTime + r3.xyzw;
  r3.xyzw = FC_WaterDiffuseUvScrollSpeed.xyzw * FC_GlobalTime + r3.xyzw;
  r4.xy = r4.xy * FC_TextureTilingScale[2].xy + FC_TextureTilingScale[2].zw;
  r4.zw = r4.zw * FC_TextureTilingScale[5].xy + FC_TextureTilingScale[5].zw;
  r5.xyz = g_BumpMap2_texture.Sample(g_BumpMap2_sampler_s, r4.zw).xyw;
  r4.xyz = g_BumpMap_texture.Sample(g_BumpMap_sampler_s, r4.xy).xyw;
  r4.x = r4.x * r4.z;
  r4.xy = r4.xy * float2(2,2) + float2(-1,-1);
  r5.x = r5.x * r5.z;
  r4.xy = r5.xy * float2(2,2) + r4.xy;
  r4.xy = float2(-1,-1) + r4.xy;
  r5.xyz = v3.xyz * r4.yyy;
  r5.xyz = r4.xxx * v4.xyz + r5.xyz;
  r1.w = dot(r4.xy, r4.xy);
  r1.w = 1 + -r1.w;
  r1.w = max(0, r1.w);
  r1.w = sqrt(r1.w);
  r4.xyz = v10.xxx ? v1.xyz : -v1.xyz;
  r5.xyz = r1.www * r4.xyz + r5.xyz;
  r1.w = saturate(dot(r1.xyz, r4.xyz));
  r2.w = dot(r5.xyz, r5.xyz);
  r2.w = rsqrt(r2.w);
  r4.xyz = r5.xyz * r2.www;
  r0.x = saturate(dot(r0.xyz, r4.xyz));
  r0.x = log2(r0.x);
  r0.x = r2.y * r0.x;
  r0.y = r2.y * 0.125 + 0.25;
  r0.x = exp2(r0.x);
  r0.x = r0.x * r0.w;
  r0.z = dot(FC_DirLightVec_0.xyz, r4.xyz);
  r0.w = saturate(16 * r0.z);
  r0.z = saturate(r0.z);
  r0.z = r0.z * r2.z;
  r0.x = r0.x * r0.w;
  r2.yzw = FC_LightMask.www * FC_DirLightSpc_0.xyz;
  r2.yzw = r2.yzw * r0.yyy;
  r0.y = dot(r1.xyz, r4.xyz);
  r0.y = 1 + -abs(r0.y);
  r0.y = max(0, r0.y);
  r0.w = r0.y * r0.y;
  r0.w = r0.w * r0.w;
  r0.y = r0.y * r0.w;
  r0.y = r0.y * FC_WaterFresnel.x + FC_WaterFresnel.y;
  r0.w = FC_WaterFogParam0.z * r4.x;
  r1.x = 10 + v2.w;
  r1.x = 10 / r1.x;
  r5.yz = FC_ScreenSize.zw * v0.xy;
  r6.x = r0.w * r1.x * g_uv_scale.x + r5.y;  // patched: ripple offset scaled
  r6.y = -v0.y * FC_ScreenSize.w + g_uv_scale.y;  // patched: mirrored lookup inside [0, scale]
  r6.xyz = g_ReflectionMap_texture.Sample(g_ReflectionMap_sampler_s, r6.xy).xyz;
  r6.xyz = r6.xyz * r0.yyy;
  r0.y = 1 + -r0.y;
  r2.yzw = r2.yzw * r0.xxx + r6.xyz;
  r0.xw = r3.zw * FC_TextureTilingScale[3].xy + FC_TextureTilingScale[3].zw;
  r1.yz = r3.xy * FC_TextureTilingScale[0].xy + FC_TextureTilingScale[0].zw;
  r3.xyz = g_DiffuseMap_texture.Sample(g_DiffuseMap_sampler_s, r1.yz).xyz;
  r6.xyz = g_DiffuseMap2_texture.Sample(g_DiffuseMap2_sampler_s, r0.xw).xyz;
  r6.xyz = r6.xyz + -r3.xyz;
  r3.xyz = r2.xxx * r6.xyz + r3.xyz;
  r3.xyz = v8.xyz * r3.xyz;
  r3.xyz = r3.xyz * r3.xyz;
  r3.xyz = FC_DifMapMulCol.xyz * r3.xyz;
  r4.w = 1;
  r6.x = dot(r4.xyzw, FC_AmbientSH_0.xyzw);
  r6.y = dot(r4.xyzw, FC_AmbientSH_1.xyzw);
  r6.z = dot(r4.xyzw, FC_AmbientSH_2.xyzw);
  r6.xyz = max(float3(0,0,0), r6.xyz);
  r7.xyz = FC_AddHemiColor_0.xyz * r4.yyy + FC_AddHemiColor_1.xyz;
  r0.x = dot(r4.xyz, v4.xyz);
  r0.x = FC_WaterFogParam0.w * r0.x;
  r5.x = r0.x * r1.x * g_uv_scale.x + r5.y;  // patched: refraction offset scaled
  r4.xyzw = g_RefractionMap_texture.Sample(g_RefractionMap_sampler_s, r5.xz).xyzw;
  r1.xyz = r6.xyz * r0.yyy + r7.xyz;
  r6.xyz = FC_LightMask.www * FC_DirLightCol_0.xyz;
  r0.xzw = r6.xyz * r0.zzz + r1.xyz;
  r1.x = cmp(r4.w == 0.000000);
  r6.xyz = g_RefractionMap_texture.Sample(g_RefractionMap_sampler_s, r5.yz).xyz;
  r1.y = g_DepthMapSampler_texture.Sample(g_DepthMapSampler_sampler_s, r5.yz).x;
  r1.y = FC_WaterFogParam0.x + -r1.y;
  r1.y = -FC_WaterFogParam0.y / r1.y;
  r1.y = -v2.w + r1.y;
  r4.xyz = r1.xxx ? r4.xyz : r6.xyz;
  r4.xyz = r4.xyz * r0.yyy;
  r0.xyz = r3.xyz * r0.xzw + -r4.xyz;
  r0.xyz = FC_WaterFresnel.zzz * r0.xyz + r4.xyz;
  r0.xyz = FC_SpcMapMulCol.xyz * r2.yzw + r0.xyz;
  r2.xyz = -r0.xyz * FC_DifColMul.xyz + FC_FogCol.xyz;
  r0.w = v8.w;
  r0.xyzw = FC_DifColMul.xyzw * r0.xyzw;
  r1.x = FC_FogParam.w * -v2.y;
  r1.z = exp2(-r1.x);
  r1.z = 1 + -r1.z;
  r1.z = r1.z / r1.x;
  r1.x = cmp(abs(r1.x) < 0.00100000005);
  r1.x = r1.x ? 0.693148971 : r1.z;
  r1.z = dot(-v2.xyz, -v2.xyz);
  r1.z = sqrt(r1.z);
  r1.z = FC_FogParam.z * r1.z;
  r1.x = r1.z * r1.x;
  r1.x = exp2(-r1.x);
  r1.x = -r1.x * FC_FogParam.x + FC_FogParam.x;
  r1.x = log2(r1.x);
  r1.x = FC_FogCol.w * r1.x;
  r1.x = exp2(r1.x);
  r2.xyz = r1.xxx * r2.xyz + r0.xyz;
  r2.xyz = r2.xyz + -r6.xyz;
  r1.x = FC_WaterFadeParam.x + -FC_WaterFresnel.w;
  r1.x = r1.w * r1.x + FC_WaterFresnel.w;
  r1.x = saturate(r1.y * r1.x);
  r0.xyz = r1.xxx * r2.xyz + r6.xyz;
  o0.xyzw = FC_FinalColorMult.xyzw * r0.xyzw;
  return;
}