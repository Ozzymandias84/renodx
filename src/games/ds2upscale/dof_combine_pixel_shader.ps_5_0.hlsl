// Replacement for the game's depth of field combine (PS 0xD817AC63, full resolution, alpha blended over the scene copy).
// The blend weight is 3 * max(near CoC, far CoC * sky factor) from the quarter-resolution CoC map. Its far part is replaced
// by the far-field CoC of the full-resolution depth of the pixel itself (see dof_blur_16tap_pixel_shader.ps_5_0.hlsl), so
// the in-focus object edge is not surrounded by a sharp strip of the background. Everything else is the original.
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;
  float4 g_uv_offset;
  float4 g_dof_depth;    // g_vDepthComputeParam of the game's filter constants
  float4 g_dof_nearfar;  // g_vNearFarParam
  float4 g_dof_extra;    // x: g_vLinearCoCFactor.y, y: g_vNoBlurDist.y, z: 1 = per pixel far field enabled
};
SamplerState SourceTexSampler_sampler_s : register(s0);
SamplerState CoCTexSampler_sampler_s : register(s2);
Texture2D<float4> SourceTexSampler_texture : register(t0);
Texture2D<float4> CoCTexSampler_texture : register(t2);
Texture2D<float> g_main_depth : register(t10);  // r24_unorm_x8_uint view of the full-size main depth buffer

void main(float4 v0 : SV_Position0, float2 v1 : TEXCOORD0, out float4 o0 : SV_Target0)
{
  const float3 coc_map = CoCTexSampler_texture.Sample(CoCTexSampler_sampler_s, v1.xy).xyw;  // near, far, sky factor
  float far_coc = coc_map.y;
  float sky_factor = coc_map.z;
  if (g_dof_extra.z > 0.5) {
    const float d = g_main_depth.Load(int3(int2(v0.xy), 0));
    const float z = -g_dof_depth.y / (g_dof_depth.x - d);
    far_coc = g_dof_extra.x * saturate((z - g_dof_nearfar.z) / (g_dof_nearfar.w - g_dof_nearfar.z));
    sky_factor = (d >= 1.0) ? g_dof_extra.y : 1.0;
  }
  float weight = max(coc_map.x, far_coc * sky_factor);
  weight = saturate(3.0 * weight);
  const float3 color = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, v1.xy).xyz;
  const float luma = dot(color, float3(0.298909992, 0.586610019, 0.114480004));
  o0.xyz = color;
  o0.w = weight;
  if (0.100000001 >= weight * (1.0 + luma)) discard;
}
