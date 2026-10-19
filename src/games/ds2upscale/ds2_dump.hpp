// Frame dumps for offline validation (developer builds, cfg key dump_frames=N): the scene colour and motion vectors the upscaler
// reads, its output, the game's depth buffer before and after the depth rebuild and the game's velocity map, written next to the
// game executable at the next present. Release builds get empty stand-ins.
#pragma once

#include "ds2_state.hpp"
#include "ds2_util.hpp"

namespace ds2 {

#if !DS2_DEVELOPER

inline void DumpCaptureInputs(reshade::api::device*, reshade::api::command_list*, reshade::api::resource, uint32_t, uint32_t,
                              uint32_t, uint32_t) {}
inline void DumpCaptureOutput(reshade::api::device*, reshade::api::command_list*) {}
inline void DumpCaptureDepthAfter(reshade::api::device*, reshade::api::command_list*) {}
inline void DumpCaptureVelocity(reshade::api::device*, reshade::api::command_list*, reshade::api::resource_view) {}
inline void FlushDumps(reshade::api::command_queue*) {}

#else

struct DumpFrame {
  uint32_t index = 0;
  reshade::api::resource color = {0u};         // staging copies of the scene and the motion vectors before DLSS runs
  reshade::api::resource motion = {0u};
  reshade::api::resource output = {0u};        // DLSS output right after the evaluate call (full size)
  reshade::api::resource depth_before = {0u};  // the game's depth buffer (r24_g8) as DLSS reads it
  reshade::api::resource depth_after = {0u};   // the same buffer after the depth rebuild (what the later draws see)
  reshade::api::resource velocity = {0u};      // the game's object velocity map (t4 of its motion blur pass), half size RGBA8
  uint32_t velocity_width = 0;
  uint32_t velocity_height = 0;
  uint32_t render_width = 0;
  uint32_t render_height = 0;
  uint32_t full_width = 0;
  uint32_t full_height = 0;
  float jitter_x = 0.f;
  float jitter_y = 0.f;
  int mode = 0;
  uint32_t frame = 0;
  float camera_position[3] = {};
};
inline std::vector<DumpFrame> pending_dumps;
inline int dump_frames_requested = 0;
inline uint32_t dump_counter = 0;

inline bool CreateStagingCopy(reshade::api::device* device, reshade::api::command_list* cmd_list,
                              reshade::api::resource source, reshade::api::resource* staging) {
  const auto desc = device->get_resource_desc(source);
  if (desc.texture.samples != 1) return false;
  const reshade::api::resource_desc staging_desc(reshade::api::resource_type::texture_2d, desc.texture.width,
                                                 desc.texture.height, 1, 1, desc.texture.format, 1,
                                                 reshade::api::memory_heap::gpu_to_cpu,
                                                 reshade::api::resource_usage::copy_dest);
  if (!device->create_resource(staging_desc, nullptr, reshade::api::resource_usage::copy_dest, staging)) {
    *staging = {0u};
    return false;
  }
  cmd_list->copy_resource(source, *staging);
  return true;
}

// Before the DLSS evaluate: copies of what it is about to read.
inline void DumpCaptureInputs(reshade::api::device* device, reshade::api::command_list* cmd_list,
                              reshade::api::resource scene_resource, uint32_t render_w, uint32_t render_h,
                              uint32_t full_w, uint32_t full_h) {
  if (dump_frames_requested <= 0) return;
  DumpFrame frame_dump;
  if (!CreateStagingCopy(device, cmd_list, scene_resource, &frame_dump.color)
      || !CreateStagingCopy(device, cmd_list, motion_vector_texture, &frame_dump.motion)) {
    dump_frames_requested = 0;
    if (frame_dump.color.handle != 0) device->destroy_resource(frame_dump.color);
    return;
  }
  if (main_depth_resource.handle != 0 && live_resources.contains(main_depth_resource.handle)) {
    CreateStagingCopy(device, cmd_list, main_depth_resource, &frame_dump.depth_before);
  }
  frame_dump.index = dump_counter++;
  frame_dump.render_width = render_w;
  frame_dump.render_height = render_h;
  frame_dump.full_width = full_w;
  frame_dump.full_height = full_h;
  frame_dump.jitter_x = jitter_x;
  frame_dump.jitter_y = jitter_y;
  frame_dump.mode = latched_mode;
  frame_dump.frame = frame_number;
  for (int i = 0; i < 3; i++) frame_dump.camera_position[i] = camera_current.position[i];
  pending_dumps.push_back(frame_dump);
  dump_frames_requested--;
}

inline bool DumpPending() {
  return !pending_dumps.empty() && pending_dumps.back().frame == frame_number;
}

// After the DLSS evaluate: its output.
inline void DumpCaptureOutput(reshade::api::device* device, reshade::api::command_list* cmd_list) {
  if (!DumpPending() || pending_dumps.back().output.handle != 0) return;
  CreateStagingCopy(device, cmd_list, upscaled_texture, &pending_dumps.back().output);
}

// After the depth rebuild: the depth buffer the later draws see.
inline void DumpCaptureDepthAfter(reshade::api::device* device, reshade::api::command_list* cmd_list) {
  if (!DumpPending() || pending_dumps.back().depth_before.handle == 0 || pending_dumps.back().depth_after.handle != 0) {
    return;
  }
  CreateStagingCopy(device, cmd_list, main_depth_resource, &pending_dumps.back().depth_after);
}

// At the game's motion blur pass (after the upscale trigger in the same frame): a copy of the velocity map it reads.
inline void DumpCaptureVelocity(reshade::api::device* device, reshade::api::command_list* cmd_list,
                                reshade::api::resource_view velocity_view) {
  if (!DumpPending() || pending_dumps.back().velocity.handle != 0 || velocity_view.handle == 0) return;
  const auto resource = device->get_resource_from_view(velocity_view);
  if (resource.handle == 0) return;
  const auto desc = device->get_resource_desc(resource);
  if (CreateStagingCopy(device, cmd_list, resource, &pending_dumps.back().velocity)) {
    pending_dumps.back().velocity_width = desc.texture.width;
    pending_dumps.back().velocity_height = desc.texture.height;
  }
}

// At present: writes the pending dumps (the render rectangle only for the render-size inputs).
inline void FlushDumps(reshade::api::command_queue* queue) {
  if (pending_dumps.empty()) return;
  auto* device = queue->get_device();
  for (auto& dump : pending_dumps) {
    const std::string base = "ds2upscale_dump_" + std::to_string(dump.index);
    const struct {
      reshade::api::resource resource;
      const char* suffix;
      uint32_t bytes_per_pixel;
      uint32_t width;
      uint32_t height;
    } outputs[] = {{dump.color, "_color.f16", 8, dump.render_width, dump.render_height},
                   {dump.motion, "_motion.f16", 4, dump.render_width, dump.render_height},
                   {dump.output, "_output.f16", 8, dump.full_width, dump.full_height},
                   {dump.depth_before, "_depth_render.u32", 4, dump.render_width, dump.render_height},
                   {dump.depth_after, "_depth_full.u32", 4, dump.full_width, dump.full_height},
                   {dump.velocity, "_velocity.u8", 4, dump.velocity_width, dump.velocity_height}};
    for (const auto& output : outputs) {
      if (output.resource.handle == 0) continue;
      reshade::api::subresource_data mapped = {};
      if (device->map_texture_region(output.resource, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
        std::ofstream file(ExeDirectory() / (base + output.suffix), std::ios::binary | std::ios::trunc);
        for (uint32_t y = 0; y < output.height; y++) {
          file.write(static_cast<const char*>(mapped.data) + static_cast<size_t>(y) * mapped.row_pitch,
                     static_cast<std::streamsize>(output.width) * output.bytes_per_pixel);
        }
        device->unmap_texture_region(output.resource, 0);
      }
      device->destroy_resource(output.resource);
    }
    std::ofstream meta(ExeDirectory() / (base + "_meta.txt"), std::ios::trunc);
    meta << "index=" << dump.index << "\nframe=" << dump.frame << "\nrender_width=" << dump.render_width
         << "\nrender_height=" << dump.render_height << "\nfull_width=" << dump.full_width << "\nmode=" << dump.mode
         << "\njitter_x=" << dump.jitter_x << "\njitter_y=" << dump.jitter_y << "\ncamera=" << dump.camera_position[0]
         << " " << dump.camera_position[1] << " " << dump.camera_position[2] << "\n";
  }
  pending_dumps.clear();
  LogMessage(reshade::log::level::info, "frame dump written");
}

#endif  // DS2_DEVELOPER

}  // namespace ds2
