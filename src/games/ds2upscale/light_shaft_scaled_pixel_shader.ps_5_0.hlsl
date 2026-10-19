// Patched copy of the light shaft mask pass 0xF036E70E for render scale < 1: the half-resolution depth it tests for sky
// is only valid in the top-left [0, scale] part of its texture, so it is sampled with scaled UVs (the scene copy is full).
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;  // xy: render size / full size
};

// ---- Created with 3Dmigoto v1.3.16 on Mon Oct 12 00:53:05 2026

cbuffer cbFilter : register(b6)
{
  float3 g_ToneMapInvAvgLuminance : packoffset(c0);
  float3 g_ToneMapScaler : packoffset(c1);
  float3 g_ToneMapMaxLuminance : packoffset(c2);
  float4 g_ToneMapSampleOffset[16] : packoffset(c3);
  float4 g_ToneMapElapsedTime : packoffset(c19);
  float4 g_ToneMapAdaptGradient : packoffset(c20);
  float4 g_BrightPassThreshold : packoffset(c21);
  float4 g_BloomBoostColor : packoffset(c22);
  float4 g_vScreenLightPos : packoffset(c23);
  float4 g_vLightShaftParam : packoffset(c24);
  float4 g_vPaletteCornerColor[4] : packoffset(c25);
  float4 g_vBloomFinalColor : packoffset(c29);
  float4 g_vLightShaftFinalColor : packoffset(c30);
  float4 g_vStarFinalColor : packoffset(c31);
  float4 g_vBloomToneMapFactor : packoffset(c32);
  float4 g_vBloomCompositeFactor : packoffset(c33);
  float4 g_vStarToneMapFactor : packoffset(c34);
  float4 g_vBloomUVScaler : packoffset(c35);
  float4 g_vFilmicToneMapParam1 : packoffset(c36);
  float4 g_vFilmicToneMapParam2 : packoffset(c37);
  float4 g_vLightShaftParallelDir : packoffset(c38);
  float4 g_vLightShaftMaskPower : packoffset(c39);
  float4 g_vToneMapAdaptMinMax : packoffset(c40);
}

SamplerState SourceTexSampler_sampler_s : register(s0);
SamplerState DepthTexSampler_sampler_s : register(s4);
Texture2D<float4> SourceTexSampler_texture : register(t0);
Texture2D<float4> DepthTexSampler_texture : register(t4);


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = g_ToneMapSampleOffset[1].xy + v1.xy;
  r0.z = DepthTexSampler_texture.Sample(DepthTexSampler_sampler_s, r0.xy * g_uv_scale.xy).x;
  r0.xyw = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, r0.xy).xyz;
  r0.z = cmp(r0.z == 1.000000);
  r0.z = r0.z ? 1.000000 : 0;
  r0.xyz = r0.xyw * r0.zzz;
  r1.xy = g_ToneMapSampleOffset[0].xy + v1.xy;
  r0.w = DepthTexSampler_texture.Sample(DepthTexSampler_sampler_s, r1.xy * g_uv_scale.xy).x;
  r1.xyz = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, r1.xy).xyz;
  r0.w = cmp(r0.w == 1.000000);
  r0.w = r0.w ? 1.000000 : 0;
  r0.xyz = r1.xyz * r0.www + r0.xyz;
  r1.xy = g_ToneMapSampleOffset[2].xy + v1.xy;
  r0.w = DepthTexSampler_texture.Sample(DepthTexSampler_sampler_s, r1.xy * g_uv_scale.xy).x;
  r1.xyz = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, r1.xy).xyz;
  r0.w = cmp(r0.w == 1.000000);
  r0.w = r0.w ? 1.000000 : 0;
  r0.xyz = r1.xyz * r0.www + r0.xyz;
  r1.xy = g_ToneMapSampleOffset[3].xy + v1.xy;
  r0.w = DepthTexSampler_texture.Sample(DepthTexSampler_sampler_s, r1.xy * g_uv_scale.xy).x;
  r1.xyz = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, r1.xy).xyz;
  r0.w = cmp(r0.w == 1.000000);
  r0.w = r0.w ? 1.000000 : 0;
  r0.xyz = r1.xyz * r0.www + r0.xyz;
  r1.xy = g_vScreenLightPos.xy + -v1.xy;
  r1.xy = g_vLightShaftMaskPower.xy * r1.xy;
  r0.w = dot(r1.xy, r1.xy);
  r0.w = sqrt(r0.w);
  r0.w = 1 + -r0.w;
  r0.w = max(0, r0.w);
  r0.w = log2(r0.w);
  r0.w = g_vLightShaftMaskPower.z * r0.w;
  r0.w = exp2(r0.w);
  r0.w = 0.25 * r0.w;
  o0.xyz = r0.xyz * r0.www;
  o0.w = 1;
  return;
}