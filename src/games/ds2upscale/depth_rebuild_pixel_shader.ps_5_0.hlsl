// Depth rebuild after upscaling: the scene was rendered into the top-left [0, scale] part of the game's full-size depth
// buffer. Draws that run after the upscaler (heat haze / refraction sprites, other depth-tested effects over the final
// image, the per-pixel depth of field) expect the depth of the full-size picture, so every full-size pixel gets the depth
// of the scene point it shows (nearest sample of the low-resolution depth).
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;  // xy: render size / full size
};

Texture2D<float> g_depth : register(t0);  // r24_unorm_x8_uint view of a copy of the main depth buffer

void main(float4 position : SV_Position, out float depth : SV_Depth) {
  const int2 source = int2(floor(position.xy * g_uv_scale.xy));
  depth = g_depth.Load(int3(source, 0));
}
