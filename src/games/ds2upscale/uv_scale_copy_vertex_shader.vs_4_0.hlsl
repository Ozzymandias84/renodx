// Replacement for the game's plain copy-quad vertex shader (0x04FB3A5C, float4 UVs) while the scene is rendered into
// the top-left sub-rectangle of its render targets. The only pre-boundary user is the SSAO history copy (PS 0xE5206B32,
// half-resolution r8g8b8a8 -> r8g8b8a8): without the UV scale it squashed the whole source texture into the shrunk
// viewport instead of copying the sub-rectangle 1:1, so the AO history was misaligned and always rejected.
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;   // xy: render size / full size
  float4 g_uv_offset;  // xy: half-texel alignment of the scaled viewport (see uv_scale_quad_vertex_shader)
};

void main(float4 position : POSITION0, float4 uv : TEXCOORD0, out float4 out_position : SV_Position0,
          out float4 out_uv : TEXCOORD0) {
  out_position = position;
  out_uv = float4(uv.xy * g_uv_scale.xy + g_uv_offset.xy, uv.zw);
}
