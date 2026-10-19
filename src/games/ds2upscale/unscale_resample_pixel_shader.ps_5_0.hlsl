// Stretches the valid [0, scale] part of a render-scaled screen-fraction texture (written while the scene was drawn into
// the top-left sub-rectangle) over the whole texture, so that a game shader that samples it with full-range UVs after the
// upscaler (the motion blur reads the velocity map this way) sees the picture it expects. Bilinear, texel-centre aligned.
// The last texels inside the rectangle are not trusted: the game's own filter passes (velocity dilation) read across the
// rectangle border into the part of the texture that was never drawn (zero = a large velocity), which would blur the right
// and bottom screen edge. Those texels take the value of the last clean one instead.
// Parameters: texel 10 of the 16x1 RGBA32F constants texture, xy = render size / full size.
Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_constants : register(t1);

float4 main(float4 position : SV_Position) : SV_Target {
  int2 size;
  g_source.GetDimensions(size.x, size.y);
  const float2 scale = g_constants.Load(int3(10, 0, 0)).xy;
  const int BORDER = 4;
  const int2 valid = int2(max(floor(float2(size) * scale + 0.5), 1.0));
  const int2 last = max(valid - 1 - BORDER, int2(0, 0));
  const float2 coordinate = position.xy * scale - 0.5;  // position is the pixel centre (p + 0.5)
  const float2 fraction = frac(coordinate);
  const int2 first = int2(floor(coordinate));
  const int2 a = clamp(first, int2(0, 0), last);
  const int2 b = clamp(first + 1, int2(0, 0), last);
  const float4 top = lerp(g_source.Load(int3(a.x, a.y, 0)), g_source.Load(int3(b.x, a.y, 0)), fraction.x);
  const float4 bottom = lerp(g_source.Load(int3(a.x, b.y, 0)), g_source.Load(int3(b.x, b.y, 0)), fraction.x);
  return lerp(top, bottom, fraction.y);
}
