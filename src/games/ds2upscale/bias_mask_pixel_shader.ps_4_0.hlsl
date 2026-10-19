// Mask of pixels the upscaler should trust the current frame on: moving objects (stencil bit set by the game) get texel 8 x,
// wind-animated foliage (stencil bit written by this add-on) gets texel 8 y, everything else 0. DLSS reads it as its "bias current
// colour" mask; FSR as its reactive mask. Parameters arrive through the same 16x1 RGBA32F texture as the motion vector pass:
// texel 5 = screen size, texel 6 = options (y: moving-object stencil bit, z: foliage stencil bit or 0), texel 8 = strengths.
Texture2D<uint2> g_stencil : register(t0);   // x24_unorm_g8_uint view of the main depth buffer
Texture2D<float4> g_constants : register(t1);

float main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  const float4 screen = g_constants.Load(int3(5, 0, 0));
  const float4 options = g_constants.Load(int3(6, 0, 0));
  const float4 extra = g_constants.Load(int3(8, 0, 0));
  const int2 pixel = int2(vpos.xy);
  if (pixel.x >= int(screen.x) || pixel.y >= int(screen.y)) return 0.0;
  const uint stencil = g_stencil.Load(int3(pixel, 0)).y;
  float mask = ((stencil & uint(options.y + 0.5)) != 0u) ? extra.x : 0.0;
  if (options.z > 0.5 && (stencil & uint(options.z + 0.5)) != 0u) mask = max(mask, extra.y);
  return mask;
}
