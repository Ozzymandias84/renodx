// GPU time of the upscale step (both backends), measured with D3D11 timestamp queries: one disjoint query per frame, four timestamps:
//   start -> [motion vectors, masks] -> before -> [DLSS evaluate / FSR passes] -> after -> [write back, depth rebuild] -> end
// Results are read a few frames later and averaged over 120 frames; the status file shows them as "gpu_ms:".
// Developer builds only: release builds get empty stand-ins.
#pragma once

#include <wrl/client.h>

#include "ds2_state.hpp"

namespace ds2 {

#if !DS2_DEVELOPER

inline void GpuTimerBegin(ID3D11Device*, ID3D11DeviceContext*) {}
inline void GpuTimerStamp(ID3D11DeviceContext*, int) {}
inline void GpuTimerEnd(ID3D11DeviceContext*) {}

#else

constexpr int GPU_TIMER_SLOTS = 4;
constexpr int GPU_TIMER_STAMPS = 4;

struct GpuTimer {
  Microsoft::WRL::ComPtr<ID3D11Query> disjoint[GPU_TIMER_SLOTS];
  Microsoft::WRL::ComPtr<ID3D11Query> stamps[GPU_TIMER_SLOTS][GPU_TIMER_STAMPS];
  bool pending[GPU_TIMER_SLOTS] = {};
  int slot = 0;
  bool open = false;
  bool created = false;
  bool usable = false;
  std::array<double, 3> sums = {};  // prep, backend, post (ms)
  double total_sum = 0.0;
  int samples = 0;
  std::array<double, 4> averages = {};  // total, prep, backend, post of the last full window
};
inline GpuTimer gpu_timer;

inline void GpuTimerCollect(ID3D11DeviceContext* context) {
  auto& t = gpu_timer;
  for (int slot = 0; slot < GPU_TIMER_SLOTS; slot++) {
    if (!t.pending[slot] || (t.open && slot == t.slot)) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    if (context->GetData(t.disjoint[slot].Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    UINT64 stamp[GPU_TIMER_STAMPS] = {};
    bool ready = true;
    for (int i = 0; i < GPU_TIMER_STAMPS && ready; i++) {
      ready = context->GetData(t.stamps[slot][i].Get(), &stamp[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    }
    if (!ready) continue;
    t.pending[slot] = false;
    if (disjoint.Disjoint != 0 || disjoint.Frequency == 0) continue;
    const double to_ms = 1000.0 / static_cast<double>(disjoint.Frequency);
    t.sums[0] += static_cast<double>(stamp[1] - stamp[0]) * to_ms;
    t.sums[1] += static_cast<double>(stamp[2] - stamp[1]) * to_ms;
    t.sums[2] += static_cast<double>(stamp[3] - stamp[2]) * to_ms;
    t.total_sum += static_cast<double>(stamp[3] - stamp[0]) * to_ms;
    if (++t.samples >= 120) {
      t.averages = {t.total_sum / t.samples, t.sums[0] / t.samples, t.sums[1] / t.samples, t.sums[2] / t.samples};
      t.sums = {};
      t.total_sum = 0.0;
      t.samples = 0;
    }
  }
}

// Stamp 0 of a frame: opens the disjoint query. Stamps 1 and 2 follow with GpuTimerStamp; GpuTimerEnd takes stamp 3 and closes it.
inline void GpuTimerEnd(ID3D11DeviceContext* context) {
  auto& t = gpu_timer;
  if (!t.open) return;
  context->End(t.stamps[t.slot][3].Get());
  context->End(t.disjoint[t.slot].Get());
  t.pending[t.slot] = true;
  t.open = false;
}

inline void GpuTimerBegin(ID3D11Device* device, ID3D11DeviceContext* context) {
  auto& t = gpu_timer;
  if (!t.created) {
    t.created = true;
    D3D11_QUERY_DESC disjoint_desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp_desc = {D3D11_QUERY_TIMESTAMP, 0};
    t.usable = true;
    for (int slot = 0; slot < GPU_TIMER_SLOTS && t.usable; slot++) {
      t.usable &= SUCCEEDED(device->CreateQuery(&disjoint_desc, t.disjoint[slot].GetAddressOf()));
      for (int i = 0; i < GPU_TIMER_STAMPS && t.usable; i++) {
        t.usable &= SUCCEEDED(device->CreateQuery(&stamp_desc, t.stamps[slot][i].GetAddressOf()));
      }
    }
  }
  if (!t.usable) return;
  GpuTimerEnd(context);  // a frame that returned early left its query open
  GpuTimerCollect(context);
  t.slot = (t.slot + 1) % GPU_TIMER_SLOTS;
  if (t.pending[t.slot]) return;  // the oldest result is still in flight: skip this frame
  context->Begin(t.disjoint[t.slot].Get());
  context->End(t.stamps[t.slot][0].Get());
  t.open = true;
}

inline void GpuTimerStamp(ID3D11DeviceContext* context, int index) {
  auto& t = gpu_timer;
  if (t.open) context->End(t.stamps[t.slot][index].Get());
}

#endif  // DS2_DEVELOPER

}  // namespace ds2
