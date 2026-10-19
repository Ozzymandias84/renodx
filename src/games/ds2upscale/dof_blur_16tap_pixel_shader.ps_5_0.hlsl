// Replacement for the game's depth of field blur (PS 0x5EE74652, half resolution).
// 1. Taps: the original averages only 8 taps on a ring of radius 2 * CoC texels (no centre tap). With the crisp picture the
//    upscaler produces that leaves 8 visible copies of every bright thin feature. This copy uses 16 taps: the original ring
//    plus an inner ring at half the radius, rotated by 22.5 degrees.
// 2. Far field per pixel: the game's CoC map comes from a quarter-resolution MIN depth chain, so the in-focus region of a
//    near object is dilated by up to a quarter-resolution texel into the far background (a sharp strip along the edge before
//    the blur starts). The far-field CoC is recomputed here from the full resolution depth of the pixel (nearest of its
//    2 x 2 block, so object edges stay sharp), with the game's own constants. The near-field CoC (which should dilate)
//    still comes from the map.
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;
  float4 g_uv_offset;
  float4 g_dof_depth;    // g_vDepthComputeParam of the game's filter constants
  float4 g_dof_nearfar;  // g_vNearFarParam
  float4 g_dof_extra;    // x: g_vLinearCoCFactor.y, y: g_vNoBlurDist.y, z: 1 = per pixel far field enabled
};
cbuffer cbFilter : register(b6)
{
  float4 g_vDepthComputeParam : packoffset(c0);
  float4 g_vNearFarParam : packoffset(c1);
  float4 g_vScreenSize : packoffset(c2);
  float4 g_vLensParam : packoffset(c3);
  float4 g_vNoBlurDist : packoffset(c4);
  float4 g_vVelomapParam : packoffset(c5);
  float4 g_vConcentrationParam : packoffset(c6);
  float4 g_vLinearCoCFactor : packoffset(c7);
  float4 g_vIntensity : packoffset(c8);
  row_major float4x4 g_mCurToPrevScreen : packoffset(c9);
  float4 g_vDebugMode : packoffset(c13);
}
SamplerState SourceTexSampler_sampler_s : register(s0);
SamplerState CoCTexSampler_sampler_s : register(s2);
Texture2D<float4> SourceTexSampler_texture : register(t0);
Texture2D<float4> CoCTexSampler_texture : register(t2);
Texture2D<float> g_main_depth : register(t10);  // r24_unorm_x8_uint view of the full-size main depth buffer

float4 main(float4 v0 : SV_Position0, float2 v1 : TEXCOORD0) : SV_Target0
{
  float2 coc = CoCTexSampler_texture.Sample(CoCTexSampler_sampler_s, v1.xy).xy;
  if (g_dof_extra.z > 0.5) {
    const int2 block = int2(v0.xy) * 2;
    const float d = min(min(g_main_depth.Load(int3(block, 0)), g_main_depth.Load(int3(block + int2(1, 0), 0))),
                        min(g_main_depth.Load(int3(block + int2(0, 1), 0)), g_main_depth.Load(int3(block + int2(1, 1), 0))));
    const float z = -g_dof_depth.y / (g_dof_depth.x - d);
    coc.y = g_dof_extra.x * saturate((z - g_dof_nearfar.z) / (g_dof_nearfar.w - g_dof_nearfar.z));
  }
  const float radius = max(coc.x, coc.y) * 2.0;
  const float2 step = g_vScreenSize.zw * radius;
  float3 sum = 0.0;
  [unroll] for (int i = 0; i < 8; i++) {
    const float angle = 0.78539816 * float(i);
    const float2 direction = float2(cos(angle), sin(angle));
    sum += SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, v1.xy + direction * step).xyz;
    const float inner_angle = angle + 0.39269908;
    const float2 inner = float2(cos(inner_angle), sin(inner_angle));
    sum += SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, v1.xy + inner * step * 0.5).xyz;
  }
  return float4(sum * 0.0625, 1.0);
}
