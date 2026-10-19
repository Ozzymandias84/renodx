// Same as uv_scale_quad_vertex_shader for the screen-space shadow pass (0x3C0AB793), which also forwards the clip
// position that the pixel shader unprojects (that part must stay unscaled).
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;   // xy: render size / full size
  float4 g_uv_offset;  // xy: half-texel alignment of the scaled viewport (see uv_scale_quad_vertex_shader)
};

void main(float4 position : POSITION0, float2 uv : TEXCOORD0, out float4 out_position : SV_Position0,
          out float4 out_uv : TEXCOORD0, out float4 out_clip : TEXCOORD1) {
  out_position = position;
  out_uv = float4(uv * g_uv_scale.xy + g_uv_offset.xy, 0.0, 0.0);
  out_clip = position;
}
