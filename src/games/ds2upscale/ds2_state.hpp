// Runtime state shared by the event handlers and the passes: what the game bound, the camera, the render scale latched for
// the frame, and the add-on's own GPU objects.
#pragma once

#include "ds2_settings.hpp"

namespace ds2 {

// ---- What the game bound, per command list ----
struct BoundBuffer {
  uint64_t handle = 0;
  uint64_t offset = 0;
  uint64_t size = UINT64_MAX;
};

struct CommandListState {
  reshade::api::viewport game_viewport = {};
  bool has_viewport = false;
  bool viewport_overridden = false;
  std::array<reshade::api::rect, 16> game_scissors = {};  // scissor rectangles as the game set them (HUD lists clip their text)
  uint32_t game_scissor_count = 0;
  bool scissor_overridden = false;  // a scaled HUD draw replaced the game's scissor rectangle

  uint32_t ps_hash = 0;
  uint32_t vs_hash = 0;
  uint64_t game_vs_pipeline = 0;  // what the game bound last (our replacement is bound on top of it per draw)
  uint64_t game_ps_pipeline = 0;
  bool vs_overridden = false;
  uint64_t vs_override_pipeline = 0;
  bool ps_overridden = false;
  uint64_t ps_override_pipeline = 0;
  std::array<BoundBuffer, 16> vs_constant_buffers = {};  // vertex shader constant buffer slots as the game bound them
  uint64_t vertex_buffer0 = 0;                          // vertex buffer slot 0 (HUD scaling reads the quad vertices)
  uint64_t vertex_offset0 = 0;
  uint32_t vertex_stride0 = 0;
  int hud_constants_slot = -1;                          // vertex constant buffer slot currently replaced for a HUD draw
  uint64_t input_layout = 0;                               // input layout pipeline the game bound last
  std::array<BoundBuffer, 16> ps_constant_buffers = {};  // pixel shader constant buffer slots as the game bound them
  std::array<uint64_t, 16> ps_srvs = {};  // pixel shader resource views as the game bound them (slot 0 = trigger input)

  // Samplers: what the game set, and what is really bound (a biased twin or the original).
  std::array<uint64_t, 16> game_samplers = {};
  std::array<uint64_t, 16> bound_samplers = {};
  bool sampler_dirty = true;   // the game changed its samplers (or the twin mode changed) since the last twin pass
  bool twins_applied = false;  // the biased twins are what is bound right now

  uint32_t render_target_count = 0;
  uint64_t render_target_resource = 0;
  reshade::api::format render_target_format = reshade::api::format::unknown;
  uint32_t render_target_width = 0;
  uint32_t render_target_height = 0;
  bool has_depth_stencil = false;
};

inline std::unordered_map<reshade::api::command_list*, CommandListState> command_list_states;
inline std::unordered_map<uint64_t, uint32_t> pipeline_pixel_hash;
inline std::unordered_map<uint64_t, uint32_t> pipeline_vertex_hash;
// Input layouts: where POSITION sits in a vertex and how many floats it has (pipeline handle -> offset, components).
inline std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> position_layouts;
inline std::unordered_set<uint64_t> live_resources;  // every resource / view that currently exists (validated before use)
inline std::unordered_set<uint64_t> live_views;
inline std::unordered_set<uint64_t> camera_buffers;  // constant buffers of the scene camera's size (their writes are mirrored)

// ---- Mirrors of the game's constant buffers (the camera and the depth of field filter constants) ----
struct MappedRegion {
  uint8_t* data = nullptr;
  uint64_t offset = 0;
  uint64_t size = 0;
  bool filter = false;
  bool vertex = false;  // mirror of a small vertex buffer (HUD scaling)
  uint64_t buffer_size = 0;
};
inline std::unordered_map<uint64_t, std::vector<uint8_t>> camera_buffer_contents;
inline std::unordered_map<uint64_t, std::vector<uint8_t>> vertex_buffer_contents;
inline std::unordered_set<uint64_t> hud_vertex_buffers;  // vertex buffers the HUD draws read from (the only ones that are mirrored)
inline std::unordered_map<uint64_t, MappedRegion> mapped_regions;
inline std::unordered_set<uint64_t> tracked_filter_buffers;  // constant buffers whose contents are mirrored (DoF filter)
inline std::unordered_map<uint64_t, std::vector<uint8_t>> filter_buffer_contents;

// ---- Camera (unjittered projection + view, read from the scene vertex constants) ----
struct Camera {
  bool valid = false;
  float proj_x = 0.f;   // projection[0][0]
  float proj_y = 0.f;   // projection[1][1]
  float depth_a = 0.f;  // hardware depth = depth_a - depth_b / view_z
  float depth_b = 0.f;
  float rotation[3][3] = {};  // view matrix rows
  float translation[3] = {};
  float position[3] = {};  // world-space camera position
};
inline Camera camera_current;
inline Camera camera_previous;
inline uint32_t camera_capture_frame = 0;
inline bool camera_captured_this_frame = false;
inline uint32_t last_scene_frame = 0;  // last frame in which the scene camera was seen (HUD scaling follows it for a while)

// ---- Frame bookkeeping ----
inline uint32_t back_buffer_width = 0;
inline uint32_t back_buffer_height = 0;
inline uint32_t frame_number = 0;
inline uint32_t draws_this_frame = 0;  // guards the per-frame rollover against extra present events
inline bool in_own_pass = false;       // our own passes re-enter the draw handlers

// Upscaler trigger: the first draw of the post chain blit, or (fallback) the draw before the tonemap.
inline uint32_t last_draw_ps_hash = 0;
inline uint32_t last_draw_ordinal = 0;
inline uint32_t fallback_hash = 0;
inline uint32_t fallback_ordinal = 0;
inline uint32_t frames_without_anchor = 0;
inline uint64_t tonemap_scene_view = 0;
inline bool upscaled_this_frame = false;
inline uint32_t last_upscaled_frame = 0;

inline float jitter_x = 0.f;
inline int jitter_phases = 8;  // length of the jitter sequence this frame (the FSR constants need it)
inline float jitter_y = 0.f;

// Render scale, latched once per frame (at present) so a frame is rendered consistently.
inline uint32_t render_width = 0;
inline uint32_t render_height = 0;
inline float scale_x = 1.f;
inline float scale_y = 1.f;
inline bool scaling_active = false;    // upscaling mode on and the add-on enabled
inline bool scene_frame_hint = false;  // the previous frame was a 3D scene frame (a camera was captured in it)
inline bool frame_scaled = false;      // this frame renders its scene passes into the sub-rectangle (from the first draw on)
inline int latched_mode = 0;

// ---- Add-on GPU objects ----
inline reshade::api::resource main_depth_resource = {0u};
inline reshade::api::resource constants_texture = {0u};      // 16 x 1 RGBA32F: parameters of the motion vector / mask / sharpen passes
inline reshade::api::resource exposure_texture = {0u};       // 1 x 1 r32_float: fixed exposure for DLSS
inline float exposure_applied = -1.f;                        // the value exposure_texture currently holds
inline reshade::api::resource motion_vector_texture = {0u};  // full size r16g16_float, valid in the render rectangle
inline reshade::api::resource upscaled_texture = {0u};       // full size RGBA16F DLSS output
inline reshade::api::resource engine_velocity_resource = {0u};  // the game's object velocity map (quarter size RGBA8), seen at its motion blur pass
inline uint32_t engine_velocity_width = 0;
inline uint32_t engine_velocity_height = 0;
inline reshade::api::resource engine_velocity_dummy = {0u};     // 1 x 1 RGBA8 bound instead while there is no velocity map
inline ID3D11DepthStencilState* foliage_saved_state = nullptr;  // the game's depth-stencil state while a foliage draw runs with another stencil reference
inline UINT foliage_saved_ref = 0;
inline UINT foliage_written_ref = 0;
inline ID3D11DeviceContext* foliage_context = nullptr;
inline uint32_t texture_width = 0;
inline uint32_t texture_height = 0;
inline std::array<float, 64> motion_vector_constants = {};  // 16 RGBA32F texels, same size as constants_texture

// Render targets that were drawn through a shrunk viewport this frame: their valid data is the top-left [0, scale] part.
inline std::unordered_set<uint64_t> scaled_written_resources;

// Passes built from the RenderPass helper; each remembers the resources it was built for.
struct PassSlot {
  renodx::utils::render::RenderPass pass;
  reshade::api::resource first = {0u};
  reshade::api::resource second = {0u};
  reshade::api::resource third = {0u};
  reshade::api::resource target = {0u};
};
inline PassSlot motion_vector_slot;  // depth -> motion vectors
inline PassSlot bias_mask_slot;      // stencil -> DLSS bias current colour mask
inline PassSlot sharpen_slot;        // upscaled image -> scene texture with contrast adaptive sharpening

// Full-range copies of render-scaled textures that a post-upscaler game shader reads (see ResampleScaledTexture).
struct ResampleSlot {
  PassSlot pass;
  reshade::api::resource scratch = {0u};
  reshade::api::resource_view scratch_view = {0u};
  uint32_t width = 0;
  uint32_t height = 0;
  reshade::api::format format = reshade::api::format::unknown;
  reshade::api::format source_format = reshade::api::format::unknown;
};
inline std::unordered_map<uint64_t, ResampleSlot> resample_slots;  // keyed by the source resource
inline reshade::api::resource bias_mask_texture = {0u};  // r8_unorm, same size as the motion vectors
inline uint32_t bias_mask_width = 0;
inline uint32_t bias_mask_height = 0;

// Replacement shaders: b11 carries xy = render size / full size, [4..5] = UV offset (quad vertex shader),
// [8..11] DoF g_vDepthComputeParam, [12..15] g_vNearFarParam, [16] g_vLinearCoCFactor.y, [17] g_vNoBlurDist.y,
// [18] 1 = per pixel far field.
inline reshade::api::pipeline_layout override_layout = {0u};
inline reshade::api::pipeline quad_vertex_override = {0u};
inline reshade::api::pipeline shadow_vertex_override = {0u};
inline reshade::api::pipeline copy_vertex_override = {0u};
inline reshade::api::pipeline motion_blur_off_override = {0u};
inline reshade::api::pipeline water_override = {0u};
inline reshade::api::pipeline light_shaft_override = {0u};
inline reshade::api::pipeline ssao_override = {0u};
inline reshade::api::pipeline dof_blur_override = {0u};
inline reshade::api::pipeline dof_combine_override = {0u};
inline bool override_pipelines_ready = false;
inline std::array<float, UV_SCALE_CONSTANT_COUNT> uv_scale_values = {1.f, 1.f};

// Depth of field constants of the game's filter buffer (mirrored at the CoC draw) and a depth view for its replacements.
inline uint32_t dof_constants_frame = 0;  // frame in which uv_scale_values[8..18] were filled
inline reshade::api::resource_view main_depth_srv = {0u};
inline reshade::api::resource main_depth_srv_resource = {0u};

// Depth rebuild after upscaling (see ds2_passes.hpp).
inline reshade::api::pipeline_layout depth_rebuild_layout = {0u};
inline reshade::api::pipeline depth_rebuild_pipeline = {0u};
inline reshade::api::resource depth_rebuild_source = {0u};  // the main depth resource the views below belong to
inline reshade::api::resource depth_rebuild_copy = {0u};
inline reshade::api::resource_view depth_rebuild_copy_view = {0u};
inline reshade::api::resource_view depth_rebuild_write_view = {0u};

// Temporal stabilisation of the two low-resolution effect maps that are computed from the (jittered, render-scaled) depth.
struct StabilizedEffect {
  reshade::api::resource target = {0u};                 // the game's render target of the effect's draw
  std::array<reshade::api::resource, 2> textures = {};  // ping-pong: textures[history_index] is last frame's result
  int history_index = 0;
  std::array<renodx::utils::render::RenderPass, 2> passes;  // passes[i] reads textures[i] as history
  std::array<reshade::api::resource, 2> pass_target = {};
  uint32_t width = 0;
  uint32_t height = 0;
  reshade::api::format format = reshade::api::format::unknown;
  uint32_t last_frame = 0;  // frame in which the history was written
};
inline std::array<StabilizedEffect, 2> stabilized_effects;  // 0: circle of confusion, 1: light shaft mask
inline int pending_stabilize = -1;                          // effect index to stabilise before the next draw
inline reshade::api::resource stabilize_constants = {0u};   // 1x1 RGBA32F
inline float camera_motion_pixels = 0.f;                    // approximate screen motion of the camera this frame (render px)

// Texture sharpening: every sampler the game creates gets a twin with a LOD bias, bound in its place for the scene passes
// that render below output resolution.
inline std::unordered_map<uint64_t, reshade::api::sampler_desc> sampler_descs;
inline std::unordered_map<uint64_t, uint64_t> sampler_twins;  // original handle -> biased twin (always a sampler WE created)
inline std::unordered_set<uint64_t> samplers_without_twin;    // originals that stay as they are (comparison / unknown)
inline std::unordered_set<uint64_t> own_samplers;             // twins we created ourselves
inline reshade::api::pipeline_layout sampler_layout = {0u};
inline uint32_t sampler_layout_param = 0;
inline float mip_bias = 0.f;   // latched per frame, 0 = no sharpening
inline float twin_bias = 0.f;  // the bias the existing twins were created with

// ---- DLSS ----
struct Dlss {
  ID3D11Device* device = nullptr;
  NVSDK_NGX_Parameter* parameters = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  bool sdk_initialized = false;
  uint32_t feature_width = 0;
  uint32_t feature_height = 0;
  uint32_t feature_target_width = 0;
  uint32_t feature_target_height = 0;
  int feature_mode = 0;
  int feature_preset = 0;
  uint32_t failed_attempts = 0;
  uint32_t next_attempt_frame = 0;
  // The first feature created after NGX is initialised only writes a render-sized corner of its output (observed with the
  // 2560x1440 -> 3840x2160 case; recreating the feature fixes it). The first feature is therefore replaced once, shortly
  // after it was created.
  uint32_t warm_up_steps = 0;  // 0: due 30 frames after creation, 1: due 150 frames after that, 2: done
  uint32_t warm_up_frame = 0;
};
inline Dlss dlss;

inline const char* status_text = "idle";
inline std::string last_ngx_message;
inline uint32_t frames_upscaled = 0;
inline uint32_t evaluate_failures = 0;
inline bool reset_history_request = false;
inline bool status_running = false;  // status_text describes a frame that was upscaled
inline int upscale_hold_frames = 0;  // frames after a scaled -> scaled mode change that are drawn at full size, without DLSS
inline bool frame_holding = false;  // this frame is one of them

}  // namespace ds2
