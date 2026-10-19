// Small helpers: logging, hashing, the low-discrepancy jitter sequence and the lock every event handler takes.
#pragma once

#include "ds2_common.hpp"
#include "ds2_profile.hpp"

namespace ds2 {

inline void LogMessage(reshade::log::level level, const std::string& text) {
  reshade::log::message(level, ("[DS2Upscale] " + text).c_str());
}

inline std::filesystem::path ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

// Radical inverse in `base` (Halton sequence), index >= 1.
inline float Halton(uint32_t index, uint32_t base) {
  float result = 0.f;
  float fraction = 1.f / static_cast<float>(base);
  while (index > 0) {
    result += static_cast<float>(index % base) * fraction;
    index /= base;
    fraction /= static_cast<float>(base);
  }
  return result;
}

inline uint32_t ComputeCrc32(const uint8_t* data, size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256> t = {};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; bit++) value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : (value >> 1);
      t[i] = value;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; i++) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
  return ~crc;
}

template <typename Flags>
inline bool HasFlag(Flags flags, Flags flag) {
  return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0u;
}

// The lock every event handler takes. The handlers run on whichever thread the game or RenoDX calls from while other
// RenoDX/ReShade locks may be held, and a blocking lock deadlocked an earlier probe when FXAA was toggled: taking this one
// gives up after 100 ms. It is recursive (the add-on's own passes re-enter the resource events) and costs one atomic
// compare-exchange when nobody else holds it, which matters: the handlers run thousands of times per frame.
class StateMutex {
 public:
  bool TryLockFor(std::chrono::milliseconds timeout) {
    const DWORD self = GetCurrentThreadId();
    if (owner_.load(std::memory_order_relaxed) == self) {
      depth_++;
      return true;
    }
    DWORD expected = 0;
    if (!owner_.compare_exchange_strong(expected, self, std::memory_order_acquire) && !WaitForOwner(self, timeout)) return false;
    depth_ = 1;
    return true;
  }

  void Unlock() {
    if (--depth_ == 0) owner_.store(0, std::memory_order_release);
  }

 private:
  bool WaitForOwner(DWORD self, std::chrono::milliseconds timeout) {
    const ULONGLONG start = GetTickCount64();
    for (uint32_t attempt = 0;; attempt++) {
      DWORD expected = 0;
      if (owner_.load(std::memory_order_relaxed) == 0 && owner_.compare_exchange_weak(expected, self, std::memory_order_acquire)) return true;
      if (attempt < 64) {
        YieldProcessor();
      } else if (attempt < 128) {
        SwitchToThread();
      } else {
        Sleep(1);
      }
      if ((attempt & 15u) == 0 && GetTickCount64() - start >= static_cast<ULONGLONG>(timeout.count())) return false;
    }
  }

  std::atomic<DWORD> owner_{0};  // id of the owning thread, 0 = free
  uint32_t depth_ = 0;           // recursion depth, only touched by the owner
};
inline StateMutex state_mutex;
inline std::atomic<uint32_t> lock_timeouts = 0;

class StateLock {
 public:
  StateLock() {
#if DS2_PROFILE
    const uint64_t probe_start = __rdtsc();
    locked_ = state_mutex.TryLockFor(std::chrono::milliseconds(100));
    ProbeScope::Add(Probe::Lock, __rdtsc() - probe_start);
#else
    locked_ = state_mutex.TryLockFor(std::chrono::milliseconds(100));
#endif
    if (!locked_) {
      const uint32_t count = ++lock_timeouts;
      if (count == 1 || count % 200 == 0) {
        reshade::log::message(reshade::log::level::warning, "[DS2Upscale] state lock timed out (deadlock avoided)");
      }
    }
  }
  ~StateLock() {
    if (locked_) state_mutex.Unlock();
  }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
  [[nodiscard]] bool ok() const { return locked_; }

 private:
  bool locked_ = false;
};
#define DS2_LOCK_OR_RETURN(value) \
  ::ds2::StateLock state_lock;    \
  if (!state_lock.ok()) return value

}  // namespace ds2
