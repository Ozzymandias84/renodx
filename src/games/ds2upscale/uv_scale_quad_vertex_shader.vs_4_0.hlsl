// Replacement for the game's full-screen quad vertex shader (0x449A78BC) while the scene is rendered into the
// top-left sub-rectangle of its render targets (render scale < 1): the quad covers the shrunk viewport, but the
// textures it reads hold valid data only in [0, scale] of their UV range, so the UVs are scaled to match.
// g_uv_offset.xy is added afterwards: the half-texel correction 0.5 * (1 - scale) / target size that keeps the sampled
// texel grid identical to the unscaled game.
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;   // xy: render size / full size
  float4 g_uv_offset;  // xy: UV offset
};

void main(float4 position : POSITION0, float2 uv : TEXCOORD0, out float4 out_position : SV_Position0,
          out float2 out_uv : TEXCOORD0) {
  out_position = position;
  out_uv = uv * g_uv_scale.xy + g_uv_offset.xy;
}
