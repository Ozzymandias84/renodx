/*
 * Common includes of the DS2 upscaler add-on. The add-on is a single translation unit (addon.cpp includes the ds2_*.hpp
 * headers in dependency order), so the headers define their state directly instead of declaring it.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ds2_build.hpp"

#define ImTextureID ImU64

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../utils/render.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/state.hpp"

#include <d3d11.h>

#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// NVIDIA NGX static library. The repo's CMake links no vendor libraries per add-on, so the library is referenced here
// (the build uses the static CRT: /MT in release, /MTd in debug).
#ifdef _DEBUG
#pragma comment(lib, "../external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib")
#else
#pragma comment(lib, "../external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
#endif
