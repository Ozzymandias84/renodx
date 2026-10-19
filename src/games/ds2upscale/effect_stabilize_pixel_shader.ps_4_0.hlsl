// Temporal stabilisation of a low-resolution effect input (depth of field circle of confusion, light shaft mask):
// blends the freshly computed map with the previous frame's stabilised map. The weight is 0 on the first frame, after a
// camera cut and while the camera moves fast, so the maps do not smear.
Texture2D<float4> g_current : register(t0);
Texture2D<float4> g_history : register(t1);
Texture2D<float4> g_params : register(t2);  // x: weight of the history

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  const int3 pixel = int3(int2(position.xy), 0);
  const float weight = saturate(g_params.Load(int3(0, 0, 0)).x);
  return lerp(g_current.Load(pixel), g_history.Load(pixel), weight);
}