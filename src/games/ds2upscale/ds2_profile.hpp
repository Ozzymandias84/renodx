// CPU cost of the add-on, for developer builds (DS2_PROFILE): time spent inside each event handler (TSC ticks) and the CPU time of the
// thread that presents (the game's render thread), averaged over 120 frames and written to the status file as the "cpu:" line.
#pragma once

#include "ds2_build.hpp"

#if DS2_PROFILE
#include <intrin.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ds2 {

enum class Probe : int {
  Lock,  // time spent taking the state lock (inside every handler below)
  Draw,
  Upscale,  // RunUpscale (inside Draw)
  BindPipeline,
  PushDescriptors,
  MapBuffer,
  UnmapBuffer,
  UpdateBuffer,
  Viewports,
  Scissors,
  RenderTargets,
  VertexBuffers,
  ResourceEvents,
  Count,
};
constexpr const char* PROBE_NAMES[] = {"lock",   "draw",     "upscale", "pipeline", "descriptors", "map",   "unmap",
                                       "update", "viewport", "scissor", "targets",  "vertexbuf",   "resources"};

struct ProbeCounter {
  std::atomic<uint64_t> ticks{0};
  std::atomic<uint32_t> calls{0};
};
inline std::array<ProbeCounter, static_cast<size_t>(Probe::Count)> probe_counters;
inline std::string profile_text;

class ProbeScope {
 public:
  explicit ProbeScope(Probe probe) : probe_(probe), start_(__rdtsc()) {}
  ~ProbeScope() { Add(probe_, __rdtsc() - start_); }
  ProbeScope(const ProbeScope&) = delete;
  ProbeScope& operator=(const ProbeScope&) = delete;

  static void Add(Probe probe, uint64_t ticks) {
    auto& counter = probe_counters[static_cast<size_t>(probe)];
    counter.ticks.fetch_add(ticks, std::memory_order_relaxed);
    counter.calls.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  Probe probe_;
  uint64_t start_;
};

inline uint64_t FileTimeTicks(const FILETIME& time) {
  return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// Called once per presented frame, on the presenting thread.
inline void ProfileFrame() {
  constexpr uint32_t WINDOW = 120;
  static uint32_t frames = 0;
  static bool started = false;
  static uint64_t start_tsc = 0;
  static LARGE_INTEGER start_qpc = {};
  static uint64_t start_thread_100ns = 0;
  static ULONG64 start_cycles = 0;
  static std::array<uint64_t, static_cast<size_t>(Probe::Count)> start_ticks = {};
  static std::array<uint32_t, static_cast<size_t>(Probe::Count)> start_calls = {};

  const auto snapshot = [&] {
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    start_thread_100ns = FileTimeTicks(kernel) + FileTimeTicks(user);
    QueryThreadCycleTime(GetCurrentThread(), &start_cycles);
    start_tsc = __rdtsc();
    QueryPerformanceCounter(&start_qpc);
    for (size_t i = 0; i < start_ticks.size(); i++) {
      start_ticks[i] = probe_counters[i].ticks.load(std::memory_order_relaxed);
      start_calls[i] = probe_counters[i].calls.load(std::memory_order_relaxed);
    }
    frames = 0;
  };
  if (!started) {
    started = true;
    snapshot();
    return;
  }
  if (++frames < WINDOW) return;

  FILETIME created, exited, kernel, user;
  GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
  ULONG64 cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  LARGE_INTEGER qpc = {}, frequency = {};
  QueryPerformanceCounter(&qpc);
  QueryPerformanceFrequency(&frequency);
  const uint64_t tsc = __rdtsc();
  const double window_ms = static_cast<double>(qpc.QuadPart - start_qpc.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
  const double ticks_per_ms = static_cast<double>(tsc - start_tsc) / window_ms;
  const double thread_ms = static_cast<double>(FileTimeTicks(kernel) + FileTimeTicks(user) - start_thread_100ns) / 10000.0 / WINDOW;
  const double mcycles = static_cast<double>(cycles - start_cycles) / 1.0e6 / WINDOW;

  char buffer[512];
  std::snprintf(buffer, sizeof(buffer), "cpu: frame_ms=%.2f thread_ms=%.3f thread_mcycles=%.3f", window_ms / WINDOW, thread_ms, mcycles);
  std::string text = buffer;
  double hooks_ms = 0.0;
  for (size_t i = 0; i < start_ticks.size(); i++) {
    const double ms = static_cast<double>(probe_counters[i].ticks.load(std::memory_order_relaxed) - start_ticks[i]) / ticks_per_ms / WINDOW;
    const double calls = static_cast<double>(probe_counters[i].calls.load(std::memory_order_relaxed) - start_calls[i]) / WINDOW;
    if (i != static_cast<size_t>(Probe::Lock) && i != static_cast<size_t>(Probe::Upscale)) hooks_ms += ms;
    std::snprintf(buffer, sizeof(buffer), " %s=%.3f/%.0f", PROBE_NAMES[i], ms, calls);
    text += buffer;
  }
  std::snprintf(buffer, sizeof(buffer), " hooks_ms=%.3f", hooks_ms);
  text += buffer;
  profile_text = text;
  snapshot();
}
}  // namespace ds2

#define DS2_PROBE(name) ::ds2::ProbeScope ds2_probe_scope(::ds2::Probe::name)
#else
#define DS2_PROBE(name) ((void)0)
#endif
