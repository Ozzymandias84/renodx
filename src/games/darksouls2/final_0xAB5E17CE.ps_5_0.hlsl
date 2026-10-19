#include "./shared.h"

// Original shader: final pass into the swapchain (draw 328 of the late-frame chain).
// Vanilla looks each channel up in a 1025-entry 1D brightness/gamma table, which clamps to [0, 1].
// The table is bypassed so values above SDR white survive to the swap chain proxy pass.

Texture2D<float4> g_src : register(t0);

float4 main(float4 v0 : SV_Position0) : SV_Target0 {
  float3 color = g_src.Load(int3(v0.xy, 0)).rgb;
  return float4(color, 1.f);
}
