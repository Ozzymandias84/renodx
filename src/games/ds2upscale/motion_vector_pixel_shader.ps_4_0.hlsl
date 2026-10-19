// Motion vectors for DLSS: render-resolution pixels, "pixel + vector = where it was in the previous frame".
// The whole picture moves with the camera: every pixel is reprojected from its hardware depth and the two camera matrices
// (the game has no camera term). Optionally (constants texel 11) pixels of moving objects (stencil bit) take the game's own object
// velocity map instead, see below.
//
// Parameters arrive through a 16x1 RGBA32F texture (t1): RenoDX's constant injection rewrites constant buffer b13.
Texture2D<float> g_depth : register(t0);  // r24_unorm_x8_uint view of the main depth buffer
Texture2D<float4> g_constants : register(t1);
Texture2D<uint2> g_stencil : register(t2);      // x24_unorm_g8_uint view of the main depth buffer
Texture2D<float4> g_velocity : register(t3);    // the game's object velocity map: quarter size RGBA8 (xy = velocity code, z = coverage)

float4 main(float4 vpos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
  const float4 rel_row0 = g_constants.Load(int3(0, 0, 0));   // xyz rotation row, w translation: current view -> previous view
  const float4 rel_row1 = g_constants.Load(int3(1, 0, 0));
  const float4 rel_row2 = g_constants.Load(int3(2, 0, 0));
  const float4 proj_cur = g_constants.Load(int3(3, 0, 0));   // x/y: projection diagonal, z/w: depth = z - w / view_z
  const float4 proj_prev = g_constants.Load(int3(4, 0, 0));  // x/y: previous projection diagonal
  const float4 screen = g_constants.Load(int3(5, 0, 0));     // width, height, 1/width, 1/height of the render rectangle

  const int2 pixel = int2(vpos.xy);
  if (pixel.x >= int(screen.x) || pixel.y >= int(screen.y)) return float4(0.0, 0.0, 0.0, 0.0);  // outside the render rectangle

  const float depth = g_depth.Load(int3(pixel, 0));
  const float view_z = proj_cur.w / max(proj_cur.z - depth, 1e-7);
  const float2 uv_centre = (float2(pixel) + 0.5) * screen.zw;
  const float2 ndc = float2(uv_centre.x * 2.0 - 1.0, 1.0 - uv_centre.y * 2.0);
  const float3 view_cur = float3(ndc.x * view_z / proj_cur.x, ndc.y * view_z / proj_cur.y, view_z);
  const float3 view_prev = float3(dot(rel_row0.xyz, view_cur) + rel_row0.w,
                                  dot(rel_row1.xyz, view_cur) + rel_row1.w,
                                  dot(rel_row2.xyz, view_cur) + rel_row2.w);
  const float2 ndc_prev = float2(proj_prev.x * view_prev.x, proj_prev.y * view_prev.y) / max(view_prev.z, 1e-4);
  const float2 uv_prev = float2(ndc_prev.x * 0.5 + 0.5, 0.5 - ndc_prev.y * 0.5);
  float2 motion = (uv_prev - uv_centre) * screen.xy;

  // The game's velocity map (r,g = current minus previous position in UV, coded as code * 0.2 - 0.0996, b = w / (10 + w) on
  // covered pixels and 1 elsewhere) is a quarter of the picture size, so it knows the silhouette only to 4 pixels. The
  // full-resolution stencil bit says which pixels are really a moving object; those take the velocity of the nearest covered
  // texel (within two texels), the rest of the picture keeps the camera vector.
  const float4 engine = g_constants.Load(int3(11, 0, 0));   // x: on, y: gain, zw: velocity map size in texels
  if (engine.x > 0.5) {
    const uint stencil = g_stencil.Load(int3(pixel, 0)).y;
    if ((stencil & uint(g_constants.Load(int3(6, 0, 0)).y + 0.5)) != 0u) {
      const float2 render_over_full = g_constants.Load(int3(10, 0, 0)).xy;
      const int2 map_size = int2(engine.zw);
      const int2 centre_texel = clamp(int2(uv_centre * render_over_full * engine.zw), int2(0, 0), map_size - 1);
      // Weighted mean of the covered texels around the pixel (closer = heavier): the codes are whole numbers (one step is about
      // 0.75 px at 1080p), the mean gives fractional steps and keeps a slow animation from flipping between two codes.
      // Texels that were never drawn (outside the render rectangle) read 0 and are skipped.
      float2 code_sum = float2(0.0, 0.0);
      float weight_sum = 0.0;
      [unroll] for (int oy = -2; oy <= 2; oy++) {
        [unroll] for (int ox = -2; ox <= 2; ox++) {
          const int2 texel = clamp(centre_texel + int2(ox, oy), int2(0, 0), map_size - 1);
          const float3 sample_value = g_velocity.Load(int3(texel, 0)).xyz;
          const float weight = 1.0 / (1.0 + float(ox * ox + oy * oy));
          if (sample_value.z > 0.03 && sample_value.z < 0.96) {
            code_sum += sample_value.xy * weight;
            weight_sum += weight;
          }
        }
      }
      if (weight_sum > 0.0) {
        const float2 delta_uv = (code_sum / weight_sum) * 0.2 - 0.0996078;
        const float2 engine_motion = -delta_uv * screen.xy * engine.y;
        // Only motion that differs clearly from the camera's is taken from the game: a body that moves like the world (idle,
        // breathing, a half step) keeps the exact camera vector, so small animation noise of the game's map never reaches
        // DLSS. Large object motion fades in between 1.5 and 4 pixels of difference.
        const float2 residual = engine_motion - motion;
        const float blend = smoothstep(1.5, 4.0, length(residual));
        motion += residual * blend;
      }
    }
  }
  return float4(motion, 0.0, 0.0);
}
