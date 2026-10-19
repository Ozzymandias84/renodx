// Replacement for the game's motion blur / depth of field combine pass (0x52355C56): returns the full-resolution
// scene unblurred and keeps the circle-of-confusion alpha that the following passes need. The game's velocity map and
// its constants stay untouched, so the upscaler can still use the velocity data.
SamplerState g_coc_sampler : register(s2);
SamplerState g_source_sampler : register(s6);
Texture2D<float4> g_coc : register(t2);
Texture2D<float4> g_source : register(t6);

float4 main(float4 position : SV_Position0, float2 uv : TEXCOORD0) : SV_Target0 {
  const float4 coc = g_coc.Sample(g_coc_sampler, uv);
  const float far_coc = coc.y * coc.w;
  const float combined = max(coc.x, far_coc);
  const float alpha = saturate(max(30.0 * combined, 10.0 * coc.z));
  return float4(g_source.Sample(g_source_sampler, uv).xyz, alpha);
}
