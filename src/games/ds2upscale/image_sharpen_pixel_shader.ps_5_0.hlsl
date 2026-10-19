// Contrast adaptive sharpening of the upscaled image (the scene before depth of field / bloom / tonemap).
// Same idea as AMD's CAS: the sharpening weight of every pixel shrinks where its neighbourhood is already near the ends of
// the range, so flat areas and bright / dark extremes are not pushed into ringing. The scene is HDR, so the filter works on
// a compressed copy (c / (1 + c) per channel, range [0, 1)) and converts back afterwards.
// Parameters: texel 9 of the 16x1 RGBA32F constants texture, x = sharpness 0..1.
Texture2D<float4> g_color : register(t0);
Texture2D<float4> g_constants : register(t1);

float3 Compress(float3 c) {
  c = max(c, 0.0);
  return c / (1.0 + c);
}

float3 Expand(float3 d) {
  d = min(d, 0.9995);
  return d / (1.0 - d);
}

float4 main(float4 position : SV_Position) : SV_Target {
  int2 size;
  g_color.GetDimensions(size.x, size.y);
  const int2 p = int2(position.xy);
  const int2 last = size - 1;

  const float4 centre_sample = g_color.Load(int3(p, 0));
  const float3 e = Compress(centre_sample.rgb);
  const float3 b = Compress(g_color.Load(int3(clamp(p + int2(0, -1), 0, last), 0)).rgb);
  const float3 d = Compress(g_color.Load(int3(clamp(p + int2(-1, 0), 0, last), 0)).rgb);
  const float3 f = Compress(g_color.Load(int3(clamp(p + int2(1, 0), 0, last), 0)).rgb);
  const float3 h = Compress(g_color.Load(int3(clamp(p + int2(0, 1), 0, last), 0)).rgb);

  const float3 mn = min(min(min(d, e), min(f, b)), h);
  const float3 mx = max(max(max(d, e), max(f, b)), h);
  // Headroom of the neighbourhood (distance to 0 and to 1 relative to its peak) decides how much it may be sharpened.
  const float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
  const float sharpness = saturate(g_constants.Load(int3(9, 0, 0)).x);
  const float peak = -1.0 / lerp(8.0, 5.0, sharpness);
  const float3 w = amp * peak;
  const float3 result = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
  return float4(Expand(result), centre_sample.a);
}
