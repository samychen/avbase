# 06 · CMake 工程与构建体系

> 上一篇：[05 迁移对照表](05-迁移对照表.md) ｜ 下一篇：[07 测试策略与可观测性](07-测试策略与可观测性.md)

> **实现状态**：本篇是设计文档，描述目标形态。**当前已落地到哪一步以
> [PROGRESS.md](PROGRESS.md) 为唯一真相源**；两者的差异（已实现 / 计划中）在 PROGRESS.md 里逐项标注。

**本篇与实际仓库的差异（截至 M0/M1）**：

| 本篇描述 | 仓库现状 | 落地里程碑 |
|---|---|---|
| `cmake/FindFFmpeg.cmake` | ⬜ 未创建（`IJKPP_ENABLE_FFMPEG` 默认 **OFF**，本篇示例写 ON） | M4 |
| `cmake/FindLinuxMediaDeps.cmake` | ⬜ 未创建 | M11/M12 |
| `cmake/IjkppInstall.cmake` + `ijkpp.map` | ⬜ 未创建 | M13 |
| `cmake/IjkppOptions.cmake` 的 `IJKPP_LINUX_*` 细分开关 | ⬜ 未创建 | M12 |
| `ijkpp_platform_null` / `_sdl2` / `_linux` target | ⬜ `platform/CMakeLists.txt` 目前是占位（对 SDL2/Linux 开关直接 `FATAL_ERROR` 指向里程碑） | M10/M11/M12 |
| `tools/gen_options.py` 生成 `option_registry.inc` | ⬜ `player/option_registry.cc` 目前是 9 项手写表 | M1 |
| `examples/` 全部 | ⬜ 未创建 | M8/M11 |
| 已就位 | ✅ 顶层 + `base/` + `media/` + `player/` + `platform/` + `tests/` 六个 CMakeLists、`IjkppOptions`、`IjkppCompilerFlags`、`IjkppThirdParty`、`IjkppCheckInvariants`、`BuildConfig.h.in`、`Version.h.in`、`CMakePresets.json`（12 preset）、`.github/workflows/ci.yml`、`tools/check_invariants.py` | — |

目标：**一条命令从零构建**（G10），Linux 上 `cmake --preset linux-sdl2 && cmake --build && ./build/.../play_sdl2 video.mp4` 直接出画。

---

## 1. 顶层 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" IJKPP_VERSION_RAW)
string(STRIP "${IJKPP_VERSION_RAW}" IJKPP_VERSION)

project(ijkpp
    VERSION   ${IJKPP_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "A C++20 reimplementation of the ijkplayer core, Chromium-style"
    HOMEPAGE_URL "https://example.com/ijkpp")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(IJKPP_IS_TOP_LEVEL ON)
else()
  set(IJKPP_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakeDependentOption)
include(CMakePackageConfigHelpers)
include(IjkppOptions)
include(IjkppCompilerFlags)

# ---------- 全局默认 ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)             # ★Google: 禁用 GNU 扩展
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # 静态库也要 PIC，才能进 .so
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${IJKPP_IS_TOP_LEVEL})
set(CMAKE_DEBUG_POSTFIX "")               # Google 风格不加 d 后缀

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------- 依赖 ----------
find_package(Threads REQUIRED)

if(IJKPP_ENABLE_FFMPEG)
  find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample)
endif()
if(IJKPP_ENABLE_SDL2)
  find_package(SDL2 REQUIRED)
endif()
if(IJKPP_ENABLE_LINUX_NATIVE)
  include(FindLinuxMediaDeps)             # OpenGL/EGL/GLX/X11/Wayland/ALSA/Pulse，全部可选
endif()
if(IJKPP_BUILD_TESTS OR IJKPP_BUILD_BENCH)
  include(IjkppThirdParty)                # GTest/GMock/Benchmark
endif()

configure_file(cmake/BuildConfig.h.in "${CMAKE_BINARY_DIR}/generated/ijkpp/BuildConfig.h" @ONLY)
configure_file(cmake/Version.h.in     "${CMAKE_BINARY_DIR}/generated/ijkpp/Version.h"     @ONLY)

# ---------- 生成物 ----------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/generated/player/option_registry.inc"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_options.py"
            --header "${CMAKE_SOURCE_DIR}/player/public/player_config.h"
            --out    "${CMAKE_BINARY_DIR}/generated/player/option_registry.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/player/public/player_config.h"
            "${CMAKE_SOURCE_DIR}/tools/gen_options.py"
    COMMENT "Generating option_registry.inc from player_config.h")

# ---------- 子目录 ----------
add_subdirectory(base)
add_subdirectory(media)
add_subdirectory(platform)
add_subdirectory(player)
if(IJKPP_BUILD_TESTS)    add_subdirectory(tests)    endif()
if(IJKPP_BUILD_EXAMPLES) add_subdirectory(examples) endif()

include(IjkppInstall)
include(IjkppCheckInvariants)

if(IJKPP_IS_TOP_LEVEL)
  message(STATUS "────────────────────────────────────────────────────────")
  message(STATUS " ijkpp ${PROJECT_VERSION}   (${CMAKE_BUILD_TYPE})")
  message(STATUS "   compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "   exceptions/RTTI : OFF / OFF   (Google style)")
  message(STATUS "   FFmpeg          : ${IJKPP_ENABLE_FFMPEG}  ${FFmpeg_VERSION_STRING}")
  message(STATUS "   SDL2 backend    : ${IJKPP_ENABLE_SDL2}   ${SDL2_VERSION}")
  message(STATUS "   Linux native    : ${IJKPP_ENABLE_LINUX_NATIVE}")
  message(STATUS "     OpenGL/EGL    : ${IJKPP_HAVE_OPENGL} / ${IJKPP_HAVE_EGL}")
  message(STATUS "     X11/Wayland   : ${IJKPP_HAVE_X11} / ${IJKPP_HAVE_WAYLAND}")
  message(STATUS "     ALSA/Pulse/PW : ${IJKPP_HAVE_ALSA} / ${IJKPP_HAVE_PULSE} / ${IJKPP_HAVE_PIPEWIRE}")
  message(STATUS "   sanitizers      : ${IJKPP_SANITIZERS}")
  message(STATUS "   tests/examples  : ${IJKPP_BUILD_TESTS} / ${IJKPP_BUILD_EXAMPLES}")
  message(STATUS "────────────────────────────────────────────────────────")
endif()
```

---

## 2. `cmake/IjkppOptions.cmake`

```cmake
# ---------- 功能开关 ----------
option(IJKPP_ENABLE_FFMPEG       "Build FFmpeg demuxer/decoder adapters"        ON)
option(IJKPP_ENABLE_SDL2         "Build the SDL2 video/audio backend"           ON)
option(IJKPP_ENABLE_LINUX_NATIVE "Build the native Linux backend (GL/EGL/X11/Wayland/ALSA/Pulse)" ON)
option(IJKPP_ENABLE_ANDROID      "Build the Android backend"                    OFF)
option(IJKPP_ENABLE_IOS          "Build the iOS backend"                        OFF)
option(IJKPP_ENABLE_CAPI         "Build the C ABI compatibility layer"          OFF)
option(IJKPP_BUILD_SHARED        "Build shared libraries instead of static"     OFF)

# ---------- Linux 原生后端的细分开关（全部默认 dlopen，弱依赖） ----------
option(IJKPP_LINUX_USE_OPENGL    "Native backend: OpenGL 3.3 video path"   ON)
option(IJKPP_LINUX_USE_EGL       "Native backend: EGL (Wayland/GBM)"       ON)
option(IJKPP_LINUX_USE_GLX       "Native backend: GLX (X11)"               ON)
option(IJKPP_LINUX_USE_X11       "Native backend: X11 window integration"  ON)
option(IJKPP_LINUX_USE_WAYLAND   "Native backend: Wayland window integration" ON)
option(IJKPP_LINUX_USE_ALSA      "Native backend: ALSA audio output"       ON)
option(IJKPP_LINUX_USE_PULSE     "Native backend: PulseAudio output"       ON)
option(IJKPP_LINUX_USE_PIPEWIRE  "Native backend: PipeWire output"         OFF)
option(IJKPP_LINUX_LINK_RUNTIME  "dlopen platform libs at runtime instead of linking" ON)

# ---------- 产物 ----------
option(IJKPP_BUILD_TESTS    "Build unit/contract/integration/golden tests" ${IJKPP_IS_TOP_LEVEL})
option(IJKPP_BUILD_EXAMPLES "Build example programs"                       ${IJKPP_IS_TOP_LEVEL})
option(IJKPP_BUILD_BENCH    "Build benchmarks"                             OFF)
option(IJKPP_BUILD_FUZZ     "Build libFuzzer targets"                      OFF)
option(IJKPP_BUILD_DOCS     "Build Doxygen documentation"                  OFF)
option(IJKPP_INSTALL        "Generate install rules"                       ${IJKPP_IS_TOP_LEVEL})

# ---------- 质量 ----------
option(IJKPP_STRICT_WARNINGS "Enable -Wconversion / -Wold-style-cast / clang-tidy-as-error" OFF)
option(IJKPP_WERROR          "Treat warnings as errors"            ${IJKPP_IS_TOP_LEVEL})
option(IJKPP_ENABLE_LTO      "Enable IPO/LTO for release builds"   OFF)
option(IJKPP_COVERAGE        "Enable gcov/llvm-cov instrumentation" OFF)
option(IJKPP_ENABLE_DCHECK   "Enable DCHECK/CHECK/SEQUENCE_CHECKER" ON)
set(IJKPP_SANITIZERS "" CACHE STRING "Semicolon list: address;thread;undefined;leak")

# ---------- 依赖位置 ----------
set(IJKPP_FFMPEG_ROOT "" CACHE PATH "Prefix where FFmpeg is installed (cross builds)")

# ---------- 校验 ----------
if("address" IN_LIST IJKPP_SANITIZERS AND "thread" IN_LIST IJKPP_SANITIZERS)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(IJKPP_ENABLE_ANDROID AND IJKPP_ENABLE_IOS)
  message(FATAL_ERROR "Android and iOS backends are mutually exclusive")
endif()
if(IJKPP_ENABLE_LINUX_NATIVE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "IJKPP_ENABLE_LINUX_NATIVE requires CMAKE_SYSTEM_NAME=Linux")
endif()
if(NOT IJKPP_ENABLE_FFMPEG AND IJKPP_BUILD_EXAMPLES AND NOT IJKPP_BUILD_TESTS)
  message(WARNING "Without FFmpeg only headless/synthetic examples can run")
endif()
```

> ⚠️ **不要用 `cmake_dependent_option` 把 `IJKPP_BUILD_TESTS` 绑到 `IJKPP_ENABLE_FFMPEG`**。
> `no-ffmpeg` 配置的核心价值就是"没装 FFmpeg 也能编译 base/media/player 并跑大部分单测"（G2 的可执行证明）。需要 FFmpeg 的用例用 CTest 标签 `needs-ffmpeg` 排除。

---

## 3. `cmake/IjkppCompilerFlags.cmake`（Google Style 落地）

```cmake
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

# ---------- 导出宏 ----------
if(IJKPP_BUILD_SHARED)
  if(WIN32)
    set(IJKPP_DLLEXPORT "__declspec(dllexport)")
    set(IJKPP_DLLIMPORT "__declspec(dllimport)")
  else()
    set(IJKPP_DLLEXPORT "__attribute__((visibility(\"default\")))")
    set(IJKPP_DLLIMPORT "__attribute__((visibility(\"default\")))")
  endif()
else()
  set(IJKPP_DLLEXPORT "")
  set(IJKPP_DLLIMPORT "")
endif()

# ---------- 警告（Google Style + Chromium） ----------
set(IJKPP_WARNINGS_COMMON
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wunused
    -Wcast-align -Wnull-dereference -Wdouble-promotion
    -Wimplicit-fallthrough -Wunreachable-code -Wformat=2
    -Werror=return-type -Werror=uninitialized -Werror=parentheses
    -Werror=narrowing -Werror=delete-non-virtual-dtor -Werror=reorder
    -Wno-unknown-pragmas)
set(IJKPP_WARNINGS_STRICT
    -Wconversion -Wsign-conversion -Wold-style-cast -Wuseless-cast
    -Wsuggest-override -Wsuggest-final-types -Wsuggest-final-methods
    -Wzero-as-null-pointer-constant -Wextra-semi -Wpessimizing-move
    -Wundefined-func-template)

function(ijkpp_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  # ★Google / Chromium: 禁用异常与 RTTI
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    target_compile_definitions(${target} PRIVATE IJKPP_NO_EXCEPTIONS=1)
  else()
    target_compile_options(${target} PRIVATE /GR- /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  endif()

  # 警告
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus
        /Zc:preprocessor /wd4251 $<$<BOOL:${IJKPP_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE ${IJKPP_WARNINGS_COMMON}
        $<$<BOOL:${IJKPP_STRICT_WARNINGS}>:${IJKPP_WARNINGS_STRICT}>
        $<$<BOOL:${IJKPP_WERROR}>:-Werror>)
    # clang thread-safety analysis（GUARDED_BY / GUARDED_BY_CONTEXT）
    check_cxx_compiler_flag(-Wthread-safety IJKPP_HAS_THREAD_SAFETY)
    if(IJKPP_HAS_THREAD_SAFETY)
      target_compile_options(${target} PRIVATE -Wthread-safety)
      target_compile_definitions(${target} PRIVATE IJKPP_THREAD_SAFETY_ANALYSIS=1)
    endif()
  endif()

  # DCHECK 开关
  target_compile_definitions(${target} PRIVATE
      $<$<BOOL:${IJKPP_ENABLE_DCHECK}>:IJKPP_ENABLE_DCHECK=1>
      $<$<CONFIG:Debug>:IJKPP_DEBUG=1>
      $<$<CONFIG:Release>:IJKPP_NDEBUG=1>)

  # sanitizers
  if(IJKPP_SANITIZERS)
    set(_sf "")
    foreach(s IN LISTS IJKPP_SANITIZERS)
      list(APPEND _sf "-fsanitize=${s}")
    endforeach()
    target_compile_options(${target} PRIVATE ${_sf} -fno-omit-frame-pointer
                                             -fno-optimize-sibling-calls)
    target_link_options(${target} PRIVATE ${_sf})
  endif()

  # coverage
  if(IJKPP_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()

  target_include_directories(${target} SYSTEM PRIVATE
      "${CMAKE_BINARY_DIR}/generated")
endfunction()

# ---------- LTO ----------
if(IJKPP_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IJKPP_IPO_OK OUTPUT IJKPP_IPO_MSG)
  if(IJKPP_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  else()
    message(WARNING "LTO requested but unsupported: ${IJKPP_IPO_MSG}")
  endif()
endif()

# ---------- std::expected 探测（决定 base/types/expected.h 是 alias 还是自研） ----------
check_cxx_source_compiles("
  #include <expected>
  int main() { std::expected<int, int> e{1}; return *e - 1; }"
  IJKPP_HAVE_STD_EXPECTED)
if(IJKPP_HAVE_STD_EXPECTED)
  message(STATUS "base::expected aliases std::expected (C++23 library available)")
endif()

# ---------- 符号隐藏 ----------
function(ijkpp_hide_vendor_symbols target)
  if(NOT IJKPP_BUILD_SHARED)
    return()
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_options(${target} PRIVATE
        "-Wl,--exclude-libs,ALL"
        "-Wl,--version-script=${CMAKE_SOURCE_DIR}/cmake/ijkpp.map")
  elseif(APPLE)
    target_link_options(${target} PRIVATE
        "-Wl,-exported_symbols_list,${CMAKE_SOURCE_DIR}/cmake/ijkpp_exported.txt")
  endif()
endfunction()
```

### 3.1 为什么 `-fno-exceptions -fno-rtti` 是硬约束

Google C++ Style 禁用异常；Chromium 额外禁用 RTTI。这直接影响设计：

| 语言特性 | 替代方案 |
|---|---|
| `throw` / `try` / `catch` | `base::expected<T, MediaError>` + `RETURN_IF_ERROR` / `ASSIGN_OR_RETURN` |
| `std::optional::value()`（会抛） | `*opt` + 前置 `CHECK(opt.has_value())` |
| `std::expected::value()`（会抛） | Chromium 版 `base::expected` 在错误态**直接 terminate**，语义一致 |
| `dynamic_cast` | `VideoFrame::StorageType` 枚举 + `storage_as<T>()`（静态地址比较） |
| `typeid` | `Storage::TypeId()`（`static const void*` 地址） |
| `std::vector::at()`（会抛） | `operator[]` + `DCHECK_LT(i, size())` |
| `std::stoi` 等（会抛） | `base::StringToInt` 返回 bool |
| `new` 失败 | `CHECK(ptr)` |

**唯一例外**：`platform/` 与 `media/filters/ffmpeg_*` 中若必须调用会抛异常的第三方（如某些 GL 加载器），在该 `.cc` 文件里局部 `try/catch` 并转换为 `MediaError`，且必须注释说明。CI 检查 `base/ media/base/ player/` 中 `throw|try|catch` 出现次数为 0（C18）。

`-fno-rtti` 还有一个实际收益：包体减小约 8%，且 `storage_as<T>()` 的静态地址比较比 `dynamic_cast` 快一个数量级（@hot 路径每帧调用）。

### 3.2 `cmake/ijkpp.map` — 符号导出白名单

```
{
  global:
    extern "C++" {
      ijkpp::*;
      ijkpp::base::*;
      ijkpp::media::*;
      ijkpp::player::*;
    };
    ijkpp_*;                 # C ABI 层（若启用）
    JNI_OnLoad;
    JNI_OnUnload;
  local:
    *;                       # av* / swr_* / sws_* / SDL_* / gl* 全部隐藏
};
```

解决 ijkplayer 用户的**头号集成痛点**：`libijkffmpeg.so` 导出全部 `av*` 符号，与 App 里另一个 FFmpeg 冲突。CI 检查 `nm -D --defined-only libijkpp.so | grep -E ' T (av|swr_|sws_|SDL_)'` 必须为空。

---

## 4. `cmake/FindFFmpeg.cmake`（三级查找）

```cmake
# FindFFmpeg.cmake
# 输出：FFmpeg_FOUND / FFmpeg_<comp>_FOUND / FFmpeg_<comp>_VERSION_MAJOR
#       imported targets FFmpeg::avformat / avcodec / avutil / swscale / swresample
# 查找顺序：① CMake Config（vcpkg 等）② pkg-config（发行版）③ 手工 find（NDK/xcframework/自编译）

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS avformat avcodec avutil swscale swresample)
endif()

set(_hints "")
if(IJKPP_FFMPEG_ROOT)
  list(APPEND _hints "${IJKPP_FFMPEG_ROOT}")
elseif(DEFINED ENV{FFMPEG_ROOT})
  list(APPEND _hints "$ENV{FFMPEG_ROOT}")
endif()

# ---- ① CMake Config ----
find_package(FFMPEG CONFIG QUIET HINTS ${_hints})
if(FFMPEG_FOUND)
  foreach(comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFMPEG::${comp} AND NOT TARGET FFmpeg::${comp})
      add_library(FFmpeg::${comp} INTERFACE IMPORTED)
      target_link_libraries(FFmpeg::${comp} INTERFACE FFMPEG::${comp})
      set(FFmpeg_${comp}_FOUND TRUE)
    endif()
  endforeach()
endif()

# ---- ② pkg-config ----
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND NOT ANDROID AND NOT IOS)
  if(_hints)
    set(ENV{PKG_CONFIG_PATH} "${_hints}/lib/pkgconfig:${_hints}/lib/x86_64-linux-gnu/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  endif()
  foreach(comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${comp})
      continue()
    endif()
    pkg_check_modules(PC_${comp} QUIET IMPORTED_TARGET lib${comp})
    if(PC_${comp}_FOUND)
      add_library(FFmpeg::${comp} INTERFACE IMPORTED)
      target_link_libraries(FFmpeg::${comp} INTERFACE PkgConfig::PC_${comp})
      string(REGEX REPLACE "^([0-9]+).*" "\\1" FFmpeg_${comp}_VERSION_MAJOR
             "${PC_${comp}_VERSION}")
      set(FFmpeg_${comp}_FOUND TRUE)
      set(FFmpeg_${comp}_VERSION "${PC_${comp}_VERSION}")
    endif()
  endforeach()
endif()

# ---- ③ 手工查找 ----
foreach(comp IN LISTS FFmpeg_FIND_COMPONENTS)
  if(TARGET FFmpeg::${comp})
    continue()
  endif()
  find_path(FFmpeg_${comp}_INCLUDE_DIR NAMES "lib${comp}/${comp}.h"
            HINTS ${_hints} PATH_SUFFIXES include)
  find_library(FFmpeg_${comp}_LIBRARY NAMES ${comp} lib${comp}
               HINTS ${_hints} PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu)
  if(FFmpeg_${comp}_INCLUDE_DIR AND FFmpeg_${comp}_LIBRARY)
    set(FFmpeg_${comp}_FOUND TRUE)
    # 从 version_major.h（FFmpeg ≥5.1）或 version.h 解析主版本
    set(_vfile "${FFmpeg_${comp}_INCLUDE_DIR}/lib${comp}/version_major.h")
    if(NOT EXISTS "${_vfile}")
      set(_vfile "${FFmpeg_${comp}_INCLUDE_DIR}/lib${comp}/version.h")
    endif()
    file(STRINGS "${_vfile}" _vline
         REGEX "^#define[ \t]+LIB${comp}_VERSION_MAJOR[ \t]+[0-9]+")
    string(REGEX REPLACE ".*_MAJOR[ \t]+([0-9]+).*" "\\1"
           FFmpeg_${comp}_VERSION_MAJOR "${_vline}")
    add_library(FFmpeg::${comp} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${comp} PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_${comp}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${comp}_INCLUDE_DIR}")
  endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_avutil_FOUND
    HANDLE_COMPONENTS
    VERSION_VAR FFmpeg_avformat_VERSION)

if(FFmpeg_FOUND)
  set(FFmpeg_LIBRARIES "")
  set(FFmpeg_INCLUDE_DIRS "")
  # 链接顺序敏感：avformat → avcodec → swresample/swscale → avutil
  foreach(comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${comp})
      list(APPEND FFmpeg_LIBRARIES FFmpeg::${comp})
      get_target_property(_i FFmpeg::${comp} INTERFACE_INCLUDE_DIRECTORIES)
      if(_i) list(APPEND FFmpeg_INCLUDE_DIRS ${_i}) endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES FFmpeg_INCLUDE_DIRS)
  set(FFmpeg_LIBRARIES "${FFmpeg_LIBRARIES}" CACHE INTERNAL "")
  set(FFmpeg_INCLUDE_DIRS "${FFmpeg_INCLUDE_DIRS}" CACHE INTERNAL "")
  set(IJKPP_FFMPEG_MAJOR "${FFmpeg_avcodec_VERSION_MAJOR}" CACHE INTERNAL "")
  set(FFmpeg_VERSION_STRING "${FFmpeg_avformat_VERSION}" CACHE INTERNAL "")
  # 最低版本门禁（Q2：默认 4.4）
  if(IJKPP_FFMPEG_MAJOR LESS 58)
    message(FATAL_ERROR
        "ijkpp requires FFmpeg >= 4.4 (libavcodec >= 58), found ${IJKPP_FFMPEG_MAJOR}. "
        "Set IJKPP_FFMPEG_ROOT to a newer FFmpeg, or see docs/06 §5 for the "
        "supported version matrix.")
  endif()
  message(STATUS "FindFFmpeg: avcodec major ${IJKPP_FFMPEG_MAJOR} (${FFmpeg_VERSION_STRING})")
endif()
```

---

## 5. FFmpeg 多版本兼容层

**规则**：所有 `#if LIBAV*_VERSION_*` 只允许出现在 `platform/ffmpeg/av_includes.h` 与 `platform/ffmpeg/compat.h` 两个文件（check_invariants C8）。

```cpp
// platform/ffmpeg/av_includes.h —— 全项目唯一的 extern "C" 包裹点
#ifndef IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_
#define IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// ---- 版本开关（全项目唯一出现处） ----
#if (LIBAVUTIL_VERSION_MAJOR > 57) || \
    (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR >= 28)
#define IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT 1
#else
#define IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT 0
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 58   // FFmpeg 4.4: AVPacket 时间字段改 int64
#define IJKPP_FFMPEG_PACKET_INT64 1
#else
#define IJKPP_FFMPEG_PACKET_INT64 0
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 61   // FFmpeg 7.x
#define IJKPP_FFMPEG_7_OR_NEWER 1
#else
#define IJKPP_FFMPEG_7_OR_NEWER 0
#endif

#endif  // IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_
```

```cpp
// platform/ffmpeg/compat.h（节选）
namespace ijkpp::platform::ffmpeg {

// ---------- RAII deleters（消灭原版 50+ 处 goto fail） ----------
struct FormatCtxDeleter { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter  { void operator()(AVCodecContext* p)  const { if (p) avcodec_free_context(&p); } };
struct FrameDeleter     { void operator()(AVFrame* p)         const { if (p) av_frame_free(&p); } };
struct PacketDeleter    { void operator()(AVPacket* p)        const { if (p) av_packet_free(&p); } };
struct DictDeleter      { void operator()(AVDictionary* p)    const { if (p) av_dict_free(&p); } };
struct BufferDeleter    { void operator()(AVBufferRef* p)     const { if (p) av_buffer_unref(&p); } };
struct SwrDeleter       { void operator()(SwrContext* p)      const { if (p) swr_free(&p); } };
struct SwsDeleter       { void operator()(SwsContext* p)      const { if (p) sws_freeContext(p); } };
struct IoCtxDeleter     { void operator()(AVIOContext* p)     const { if (p) avio_context_free(&p); } };

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtxPtr  = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using FramePtr     = std::unique_ptr<AVFrame,     FrameDeleter>;
using PacketPtr    = std::unique_ptr<AVPacket,    PacketDeleter>;
using DictPtr      = std::unique_ptr<AVDictionary, DictDeleter>;
using SwrPtr       = std::unique_ptr<SwrContext,   SwrDeleter>;
using SwsPtr       = std::unique_ptr<SwsContext,   SwsDeleter>;

// ---------- 声道布局：屏蔽 AVChannelLayout 差异 ----------
int ChannelCount(const AVCodecContext* ctx);
uint64_t ChannelLayoutMask(const AVCodecContext* ctx);
void SetChannelLayout(AVCodecContext* ctx, uint64_t mask, int nb_channels);

// ---------- swr 分配：swr_alloc_set_opts → swr_alloc_set_opts2 ----------
SwrPtr MakeSwrContext(AVSampleFormat out_fmt, uint64_t out_layout, int out_rate,
                      AVSampleFormat in_fmt,  uint64_t in_layout,  int in_rate);

// ---------- 时间戳归一化 ----------
base::TimeDelta ToTimeDelta(int64_t ts, AVRational time_base);   // AV_NOPTS → kNoTimestamp
int64_t FromTimeDelta(base::TimeDelta t, AVRational time_base);

// ---------- 错误 ----------
MediaError ToMediaError(int av_error, std::string_view context);  // 内部调 av_strerror
DecoderStatus ToDecoderStatus(int av_error, std::string_view context);

// ---------- AVDictionary 与 std::map 互转 ----------
DictPtr ToAvDict(const std::map<std::string, std::string>& m);
std::map<std::string, std::string> FromAvDict(const AVDictionary* d);

}  // namespace ijkpp::platform::ffmpeg
```

**支持矩阵**（CI 全覆盖）：

| FFmpeg | libavcodec major | 状态 | 主要差异 |
|---|---|---|---|
| 4.4 | 58 | ✅ 最低支持 | `AVPacket` 时间字段刚改 int64 |
| 5.1 | 59 | ✅ | `AVChannelLayout`、`swr_alloc_set_opts2` |
| 6.1 | 60 | ✅ | 移除若干废弃 API |
| 7.1 | 61 | ✅ 主推 | `ticks_per_frame` 废弃 |
| 8.x | 62 | 🧪 实验 | 发布后验证 |

---

## 6. `cmake/FindLinuxMediaDeps.cmake`（★Linux 后端）

全部**可选** + 默认 **`dlopen` 运行时加载**（`IJKPP_LINUX_LINK_RUNTIME=ON`），这样单个 `libijkpp.so` 可以在只有 ALSA 的机器上跑，也可以在只有 PipeWire 的机器上跑，不会因为缺库而无法加载。

```cmake
# OpenGL / GLES
if(IJKPP_LINUX_USE_OPENGL)
  find_package(OpenGL COMPONENTS OpenGL GLX QUIET)
  if(OpenGL_FOUND)
    set(IJKPP_HAVE_OPENGL ON)
  endif()
endif()
if(IJKPP_LINUX_USE_EGL)
  find_path(EGL_INCLUDE_DIR EGL/egl.h)
  find_library(EGL_LIBRARY NAMES EGL)
  if(EGL_INCLUDE_DIR AND EGL_LIBRARY)
    set(IJKPP_HAVE_EGL ON)
  endif()
endif()

# X11
if(IJKPP_LINUX_USE_X11)
  find_package(X11 QUIET COMPONENTS X11 Xext Xrandr)
  if(X11_FOUND)
    set(IJKPP_HAVE_X11 ON)
    # Present 扩展（精确 vsync）与 XShm（零拷贝上传）
    find_path(X11_PRESENT_INCLUDE_DIR X11/extensions/Xpresent.h HINTS ${X11_INCLUDE_DIR})
    if(X11_PRESENT_INCLUDE_DIR)
      set(IJKPP_HAVE_X11_PRESENT ON)
    endif()
    find_path(X11_SHM_INCLUDE_DIR X11/extensions/XShm.h HINTS ${X11_INCLUDE_DIR})
    if(X11_SHM_INCLUDE_DIR)
      set(IJKPP_HAVE_X11_SHM ON)
    endif()
  endif()
endif()

# Wayland（需要 wayland-scanner 生成协议代码）
if(IJKPP_LINUX_USE_WAYLAND)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(WAYLAND QUIET wayland-client wayland-egl wayland-cursor)
    pkg_check_modules(XKBCOMMON QUIET xkbcommon)
    find_program(WAYLAND_SCANNER wayland-scanner)
    find_path(WAYLAND_PROTOCOLS_DIR NAMES xdg-shell/xdg-shell.xml
              PATH_SUFFIXES share/wayland-protocols)
    if(WAYLAND_FOUND AND XKBCOMMON_FOUND AND WAYLAND_SCANNER AND WAYLAND_PROTOCOLS_DIR)
      set(IJKPP_HAVE_WAYLAND ON)
      set(IJKPP_WAYLAND_PROTOCOLS
          xdg-shell/xdg-shell.xml
          xdg-decoration/unstable/v1/xdg-decoration-unstable-v1.xml
          linux-dmabuf/unstable/v1/linux-dmabuf-unstable-v1.xml
          presentation-time/presentation-time.xml
          viewporter/viewporter.xml)
    endif()
  endif()
endif()

# 音频
if(IJKPP_LINUX_USE_ALSA)
  find_package(ALSA QUIET)
  if(ALSA_FOUND) set(IJKPP_HAVE_ALSA ON) endif()
endif()
if(IJKPP_LINUX_USE_PULSE)
  pkg_check_modules(PULSE QUIET libpulse-simple libpulse)
  if(PULSE_FOUND) set(IJKPP_HAVE_PULSE ON) endif()
endif()
if(IJKPP_LINUX_USE_PIPEWIRE)
  pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
  if(PIPEWIRE_FOUND) set(IJKPP_HAVE_PIPEWIRE ON) endif()
endif()

# dmabuf 零拷贝（VAAPI/硬解输出直接 EGLImage）
include(CheckIncludeFileCXX)
check_include_file_cxx("linux/dmabuf.h" IJKPP_HAVE_DMABUF_H)
check_include_file_cxx("drm_fourcc.h"   IJKPP_HAVE_DRM_FOURCC)

# SDL2（另一个后端）
if(IJKPP_ENABLE_SDL2)
  find_package(SDL2 REQUIRED)
endif()

# ---------- 汇总报告 ----------
foreach(v IJKPP_HAVE_OPENGL IJKPP_HAVE_EGL IJKPP_HAVE_X11 IJKPP_HAVE_X11_PRESENT
          IJKPP_HAVE_X11_SHM IJKPP_HAVE_WAYLAND IJKPP_HAVE_ALSA IJKPP_HAVE_PULSE
          IJKPP_HAVE_PIPEWIRE IJKPP_HAVE_DMABUF_H)
  if(NOT DEFINED ${v}) set(${v} OFF) endif()
endforeach()

# 至少要有一个视频后端和一个音频后端，否则只能 headless
if(IJKPP_ENABLE_LINUX_NATIVE AND NOT (IJKPP_HAVE_OPENGL OR IJKPP_HAVE_EGL))
  message(WARNING "Neither OpenGL nor EGL found: ijkpp_platform_linux will be "
                  "built without a video path (headless only).")
endif()
```

### 6.1 Wayland 协议代码生成

```cmake
# platform/linux/CMakeLists.txt（片段）
if(IJKPP_HAVE_WAYLAND)
  set(WL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-gen")
  file(MAKE_DIRECTORY "${WL_GEN_DIR}")
  foreach(proto IN LISTS IJKPP_WAYLAND_PROTOCOLS)
    get_filename_component(name "${proto}" NAME_WE)
    add_custom_command(
        OUTPUT  "${WL_GEN_DIR}/${name}-client-protocol.h"
                "${WL_GEN_DIR}/${name}-client-protocol.cc"
        COMMAND ${WAYLAND_SCANNER} client-header
                "${WAYLAND_PROTOCOLS_DIR}/${proto}"
                "${WL_GEN_DIR}/${name}-client-protocol.h"
        COMMAND ${WAYLAND_SCANNER} private-code
                "${WAYLAND_PROTOCOLS_DIR}/${proto}"
                "${WL_GEN_DIR}/${name}-client-protocol.cc"
        DEPENDS "${WAYLAND_PROTOCOLS_DIR}/${proto}"
        COMMENT "wayland-scanner: ${name}")
    list(APPEND WL_GEN_SOURCES "${WL_GEN_DIR}/${name}-client-protocol.h"
                               "${WL_GEN_DIR}/${name}-client-protocol.cc")
  endforeach()
  # 生成的 .cc 不参与 -Wconversion 等严格警告（第三方代码）
  set_source_files_properties(${WL_GEN_SOURCES} PROPERTIES
      COMPILE_FLAGS "-Wno-pedantic -Wno-unused-parameter -Wno-old-style-cast")
endif()
```

---

## 7. Target 定义

### 7.1 `base/CMakeLists.txt`

```cmake
set(IJKPP_BASE_SOURCES
    base/check.cc
    base/logging.cc
    base/observer_list.cc
    base/feature_list.cc
    base/types/expected.cc
    base/memory/weak_ptr.cc
    base/memory/ref_counted.cc
    base/functional/callback_internal.cc
    base/functional/bind_internal.cc
    base/time/time.cc
    base/time/tick_clock.cc
    base/time/default_tick_clock.cc
    base/synchronization/lock.cc
    base/synchronization/condition_variable_posix.cc
    base/synchronization/waitable_event_posix.cc
    base/task/task_runner.cc
    base/task/sequenced_task_runner.cc
    base/task/single_thread_task_runner.cc
    base/task/task_runner_util_internal.cc
    base/task/task_queue.cc
    base/message_loop/message_pump.cc
    base/message_loop/message_pump_epoll.cc
    base/message_loop/run_loop.cc
    base/threading/thread.cc
    base/threading/platform_thread_posix.cc
    base/containers/circular_deque.cc
    base/files/file_posix.cc
    base/trace_event/trace_event.cc
    base/strings/string_number_conversions.cc)

add_library(ijkpp_base ${IJKPP_BASE_SOURCES})
add_library(ijkpp::base ALIAS ijkpp_base)
target_include_directories(ijkpp_base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_base PUBLIC Threads::Threads)
target_compile_definitions(ijkpp_base PRIVATE IJKPP_IMPLEMENTING_BASE=1)
ijkpp_configure_target(ijkpp_base)

# ★base 零外部依赖，可独立发布
if(IJKPP_BUILD_TESTS)
  add_library(ijkpp_base_test_support
      base/test/task_environment.cc
      base/test/mock_callback.cc
      base/test/scoped_feature_list.cc
      base/test/test_waitable_event.cc
      base/time/simple_test_tick_clock.cc
      base/test/lock_order_checker.cc)
  target_link_libraries(ijkpp_base_test_support PUBLIC ijkpp_base GTest::gtest GTest::gmock)
  ijkpp_configure_target(ijkpp_base_test_support)
endif()
```

### 7.2 `media/CMakeLists.txt`

```cmake
# ---------- media/base + media/filters（不含 FFmpeg） ----------
set(IJKPP_MEDIA_SOURCES
    media/base/decoder_buffer.cc
    media/base/decoder_buffer_queue.cc
    media/base/video_frame.cc
    media/base/video_frame_pool.cc
    media/base/video_frame_metadata.cc
    media/base/video_color_space.cc
    media/base/audio_buffer.cc
    media/base/audio_bus.cc
    media/base/audio_parameters.cc
    media/base/audio_renderer_algorithm.cc      # WSOLA
    media/base/channel_layout.cc
    media/base/sample_format.cc
    media/base/video_decoder_config.cc
    media/base/audio_decoder_config.cc
    media/base/decoder_status.cc
    media/base/media_log.cc
    media/base/media_types.cc
    media/base/pipeline_status.cc
    media/base/wall_clock_time.cc
    media/base/seekable_buffer.cc
    media/filters/legacy/video_frame_compositor.cc   # ★ LGPL 隔离区（第九轮移入）
    media/base/video_frame_queue.cc
    media/filters/legacy/av_sync_controller.cc
    media/filters/legacy/clock.cc
    media/filters/decoder_selector.cc
    media/filters/display_geometry.cc
    media/filters/stream_selector.cc
    media/filters/text_renderer.cc
    media/filters/null_decoder.cc
    media/audio/audio_manager.cc
    media/audio/audio_output_device.cc
    media/audio/null/null_audio_output_stream.cc
    media/audio/fake/fake_audio_output_stream.cc
    media/renderers/default_renderer_factory.cc)

add_library(ijkpp_media ${IJKPP_MEDIA_SOURCES})
add_library(ijkpp::media ALIAS ijkpp_media)
target_include_directories(ijkpp_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_media PUBLIC ijkpp::base)
target_compile_definitions(ijkpp_media PRIVATE IJKPP_IMPLEMENTING_MEDIA=1)
ijkpp_configure_target(ijkpp_media)
# ★不链接 FFmpeg

# ---------- platform/ffmpeg（唯一链接 FFmpeg 的 target） ----------
if(IJKPP_ENABLE_FFMPEG)
  add_library(ijkpp_platform_ffmpeg
      platform/ffmpeg/compat.cc
      platform/ffmpeg/av_packet_storage.cc
      platform/ffmpeg/av_frame_storage.cc
      platform/ffmpeg/log_bridge.cc
      platform/ffmpeg/dict_converter.cc
      platform/ffmpeg/interrupt_callback.cc
      media/filters/ffmpeg_glue.cc
      media/filters/ffmpeg_demuxer.cc
      media/filters/ffmpeg_demuxer_stream.cc
      media/filters/ffmpeg_video_decoder.cc
      media/filters/ffmpeg_audio_decoder.cc
      media/filters/ffmpeg_video_frame_converter.cc
      media/filters/ffmpeg_audio_converter.cc
      media/filters/ffmpeg_decoder_factory.cc
      media/filters/file_data_source.cc
      media/filters/buffered_data_source.cc)
  add_library(ijkpp::platform_ffmpeg ALIAS ijkpp_platform_ffmpeg)
  target_link_libraries(ijkpp_platform_ffmpeg
      PUBLIC  ijkpp::media
      PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
              FFmpeg::swscale FFmpeg::swresample)
  target_compile_definitions(ijkpp_platform_ffmpeg PRIVATE IJKPP_ENABLE_FFMPEG=1)
  target_include_directories(ijkpp_platform_ffmpeg PRIVATE ${CMAKE_SOURCE_DIR})
  ijkpp_configure_target(ijkpp_platform_ffmpeg)
  target_precompile_headers(ijkpp_platform_ffmpeg PRIVATE
      "${CMAKE_SOURCE_DIR}/platform/ffmpeg/av_includes.h")
endif()
```

> **D11 边界说明**：`media/filters/ffmpeg_*.cc` 会 include `platform/ffmpeg/av_includes.h`。也就是说，`media/filters/` 里 FFmpeg 相关实现**确实**会见到 `libav*.h`，但它们被单独放进 `ijkpp_platform_ffmpeg` target，链接隔离仍然成立（`ijkpp_media` 不含任何 FFmpeg 符号）。
> 若要更严格（`media/filters/ffmpeg_*.cc` 也不 include FFmpeg，全部走 `platform/ffmpeg/` 的薄封装），实现成本约 +15%。**这是需要拍板的 D11**；本设计取"链接隔离 + 目录隔离，放弃 include 隔离"。

### 7.3 `platform/CMakeLists.txt`

```cmake
# ---------- null ----------
add_library(ijkpp_platform_null
    platform/null/null_video_renderer_sink.cc
    platform/null/null_audio_renderer_sink.cc
    platform/null/null_backend.cc)
add_library(ijkpp::platform_null ALIAS ijkpp_platform_null)
target_link_libraries(ijkpp_platform_null PUBLIC ijkpp::media)
target_include_directories(ijkpp_platform_null PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
ijkpp_configure_target(ijkpp_platform_null)

# ---------- sdl2 ----------
if(IJKPP_ENABLE_SDL2)
  add_library(ijkpp_platform_sdl2
      platform/sdl2/sdl2_video_renderer_sink.cc
      platform/sdl2/sdl2_audio_renderer_sink.cc
      platform/sdl2/sdl2_window.cc
      platform/sdl2/sdl2_gl_renderer.cc
      platform/sdl2/sdl2_backend.cc)
  add_library(ijkpp::platform_sdl2 ALIAS ijkpp_platform_sdl2)
  target_link_libraries(ijkpp_platform_sdl2 PUBLIC ijkpp::media PRIVATE SDL2::SDL2)
  ijkpp_configure_target(ijkpp_platform_sdl2)
endif()

# ---------- linux native ----------
if(IJKPP_ENABLE_LINUX_NATIVE)
  set(LINUX_SOURCES
      platform/linux/linux_backend.cc
      platform/linux/native_display_linux.cc
      platform/linux/gl/gl_video_renderer_sink.cc
      platform/linux/gl/gl_video_renderer.cc
      platform/linux/gl/gl_shaders.cc
      platform/linux/gl/egl_context.cc
      platform/linux/gl/glx_context.cc
      platform/linux/window/x11_surface.cc
      platform/linux/window/wayland_surface.cc
      platform/linux/window/present_extension.cc
      platform/linux/audio/alsa_audio_renderer_sink.cc
      platform/linux/audio/pulse_audio_renderer_sink.cc
      platform/linux/audio/pipewire_audio_renderer_sink.cc
      platform/linux/zero_copy/dmabuf_video_frame.cc)

  add_library(ijkpp_platform_linux ${LINUX_SOURCES} ${WL_GEN_SOURCES})
  add_library(ijkpp::platform_linux ALIAS ijkpp_platform_linux)
  target_link_libraries(ijkpp_platform_linux PUBLIC ijkpp::media PRIVATE ${CMAKE_DL_LIBS})

  # dlopen 模式：不硬链接平台库，运行时探测（★SDK 分发友好）
  if(IJKPP_LINUX_LINK_RUNTIME)
    target_compile_definitions(ijkpp_platform_linux PRIVATE IJKPP_LINUX_DLOPEN=1)
  else()
    if(IJKPP_HAVE_OPENGL) target_link_libraries(ijkpp_platform_linux PRIVATE OpenGL::GL OpenGL::GLX) endif()
    if(IJKPP_HAVE_EGL)    target_link_libraries(ijkpp_platform_linux PRIVATE ${EGL_LIBRARY})           endif()
    if(IJKPP_HAVE_X11)    target_link_libraries(ijkpp_platform_linux PRIVATE X11::X11 X11::Xext X11::Xrandr) endif()
    if(IJKPP_HAVE_WAYLAND)target_link_libraries(ijkpp_platform_linux PRIVATE ${WAYLAND_LIBRARIES} ${XKBCOMMON_LIBRARIES}) endif()
    if(IJKPP_HAVE_ALSA)   target_link_libraries(ijkpp_platform_linux PRIVATE ALSA::ALSA)               endif()
    if(IJKPP_HAVE_PULSE)  target_link_libraries(ijkpp_platform_linux PRIVATE ${PULSE_LIBRARIES})       endif()
  endif()

  target_compile_definitions(ijkpp_platform_linux PRIVATE
      IJKPP_HAVE_OPENGL=$<BOOL:${IJKPP_HAVE_OPENGL}>
      IJKPP_HAVE_EGL=$<BOOL:${IJKPP_HAVE_EGL}>
      IJKPP_HAVE_X11=$<BOOL:${IJKPP_HAVE_X11}>
      IJKPP_HAVE_X11_PRESENT=$<BOOL:${IJKPP_HAVE_X11_PRESENT}>
      IJKPP_HAVE_X11_SHM=$<BOOL:${IJKPP_HAVE_X11_SHM}>
      IJKPP_HAVE_WAYLAND=$<BOOL:${IJKPP_HAVE_WAYLAND}>
      IJKPP_HAVE_ALSA=$<BOOL:${IJKPP_HAVE_ALSA}>
      IJKPP_HAVE_PULSE=$<BOOL:${IJKPP_HAVE_PULSE}>
      IJKPP_HAVE_PIPEWIRE=$<BOOL:${IJKPP_HAVE_PIPEWIRE}>)
  if(IJKPP_HAVE_WAYLAND)
    target_include_directories(ijkpp_platform_linux PRIVATE "${WL_GEN_DIR}")
  endif()
  ijkpp_configure_target(ijkpp_platform_linux)
  ijkpp_hide_vendor_symbols(ijkpp_platform_linux)
endif()
```

### 7.4 `player/CMakeLists.txt`

```cmake
add_library(ijkpp_player
    player/player_impl.cc
    player/state_machine.cc
    player/event_hub.cc
    player/seek_controller.cc
    player/buffer_controller.cc
    player/option_registry.cc
    player/diagnostics.cc
    player/global.cc
    "${CMAKE_BINARY_DIR}/generated/player/option_registry.inc")
add_library(ijkpp::player ALIAS ijkpp_player)

target_include_directories(ijkpp_player PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_player PUBLIC ijkpp::media ijkpp::platform_null)
target_compile_definitions(ijkpp_player PRIVATE IJKPP_IMPLEMENTING_PLAYER=1)
ijkpp_configure_target(ijkpp_player)

# ---------- 聚合 INTERFACE target ----------
add_library(ijkpp INTERFACE)
add_library(ijkpp::ijkpp ALIAS ijkpp)
target_link_libraries(ijkpp INTERFACE ijkpp::player)
if(IJKPP_ENABLE_FFMPEG)    target_link_libraries(ijkpp INTERFACE ijkpp::platform_ffmpeg) endif()
if(IJKPP_ENABLE_SDL2)      target_link_libraries(ijkpp INTERFACE ijkpp::platform_sdl2)   endif()
if(IJKPP_ENABLE_LINUX_NATIVE) target_link_libraries(ijkpp INTERFACE ijkpp::platform_linux) endif()

if(IJKPP_BUILD_SHARED)
  add_library(ijkpp_shared SHARED platform/shared/empty_translation_unit.cc)
  target_link_libraries(ijkpp_shared PRIVATE ijkpp::ijkpp)
  set_target_properties(ijkpp_shared PROPERTIES OUTPUT_NAME ijkpp)
  ijkpp_hide_vendor_symbols(ijkpp_shared)
endif()

if(IJKPP_ENABLE_CAPI) add_subdirectory(player/public/c) endif()
```

### 7.5 `examples/CMakeLists.txt`

```cmake
function(ijkpp_add_example name)
  cmake_parse_arguments(EX "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${EX_SOURCES})
  target_link_libraries(${name} PRIVATE ijkpp::ijkpp ${EX_DEPS})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  ijkpp_configure_target(${name})
endfunction()

ijkpp_add_example(play_sdl2     SOURCES examples/play_sdl2/main.cc
                                      examples/play_sdl2/simple_window.cc)
ijkpp_add_example(play_native   SOURCES examples/play_native/main.cc)
ijkpp_add_example(play_embed    SOURCES examples/play_embed/main.cc)
ijkpp_add_example(headless      SOURCES examples/headless/main.cc)
ijkpp_add_example(stats_dump    SOURCES examples/stats_dump/main.cc)
ijkpp_add_example(ijkpp_inspect SOURCES examples/ijkpp_inspect/main.cc
                                       examples/ijkpp_inspect/dump.cc
                                       examples/ijkpp_inspect/golden.cc)
```

### 7.6 源文件列举：什么时候可以 glob

一条规则，不再按文件数做判断题：

> **目录里的 `.cc` 全部属于同一个目标、且没有任何文件是按平台或按构建开关挑选的
> → 用 `file(GLOB ... CONFIGURE_DEPENDS)`；否则逐个列出。**

| 目录 | 处理 | 原因 |
|---|---|---|
| `media/base/` · `media/renderers/` · `media/filters/legacy/` | glob | 全部属于 `ijkpp_media`，无平台/开关选择 |
| `player/` | glob | 单目录单目标 |
| `platform/ffmpeg/` · `platform/sdl2/` | glob | 目录自身单目标；`platform_ffmpeg` 跨目录取的那五个 `ffmpeg_*.cc` 仍逐个列出 |
| `tools/inspect/` | glob | 单目录单目标：新增子命令不必再改 CMake |
| `tests/unit/{base,media_base,player}/` | glob | 测试目录与目标一一对应，也正是新增文件最频繁的地方 |
| `media/filters/` | 显式 | 同目录下有 5 个 `ffmpeg_*.cc` 属于 `ijkpp_platform_ffmpeg`，glob 会把 `libav*` 扫进核心库，破坏 G2 |
| `tests/unit/media_filters/` | 显式 | 同上：4 个进 `media_unittests`，3 个进 `media_ffmpeg_unittests` |
| `base/` | 显式 | 这一层的抽象就是"每平台一个文件"（今天是 `threading/platform_thread_posix.cc`，§7.1 计划里的 `synchronization/` posix/win 成对文件同理），glob 会编进错误平台的那一个 |
| `examples/` | 显式 | 两个可执行文件同在一个目录、却在两个不同的开关下 |

`CONFIGURE_DEPENDS` 不是可选项：没有它，新增文件不会触发 CMake 重跑，"我加了文件、
但没被编译"会静默发生。**不要用 `aux_source_directory`**：它既不递归，也没有重跑语义
（CMake 官方文档正是拿这一点劝阻它）——省下一行显式列表，换来一个静默陷阱。

**门禁 C25** 兜住另一半：每个 `.cc` 必须被某个目标覆盖（显式列表或 glob 展开），否则
退出码非 0。"忘了加进构建"于是从静默不编译变成响亮的失败。某个文件**不该**被编译时
（DRAFT），写进 `tools/check_invariants.py` 的 `DRAFT_FILES`——注意 glob 改变了 DRAFT
的放法：**留在一个被 glob 的目录里的 DRAFT 会被编译**，它应该待在没有任何 glob 能到达
的子目录里（glob 不递归），直到能编译再移回原位。

---

## 8. `CMakePresets.json`

```json
{
  "version": 4,
  "cmakeMinimumRequired": { "major": 3, "minor": 20, "patch": 0 },
  "configurePresets": [
    {
      "name": "base", "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "IJKPP_BUILD_TESTS": "ON",
        "IJKPP_BUILD_EXAMPLES": "ON"
      }
    },
    { "name": "default",   "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
    { "name": "debug",     "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "IJKPP_STRICT_WARNINGS": "ON", "IJKPP_ENABLE_DCHECK": "ON" } },
    { "name": "release",   "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release", "IJKPP_ENABLE_LTO": "ON", "IJKPP_ENABLE_DCHECK": "OFF", "IJKPP_BUILD_TESTS": "OFF", "IJKPP_BUILD_SHARED": "ON" } },

    { "name": "no-ffmpeg", "inherits": "base", "displayName": "Core only — no FFmpeg needed",
      "cacheVariables": { "IJKPP_ENABLE_FFMPEG": "OFF", "IJKPP_ENABLE_SDL2": "OFF", "IJKPP_ENABLE_LINUX_NATIVE": "OFF" } },

    { "name": "linux-sdl2",   "inherits": "default",
      "cacheVariables": { "IJKPP_ENABLE_SDL2": "ON", "IJKPP_ENABLE_LINUX_NATIVE": "OFF" } },
    { "name": "linux-native", "inherits": "default",
      "cacheVariables": { "IJKPP_ENABLE_LINUX_NATIVE": "ON", "IJKPP_ENABLE_SDL2": "OFF" } },
    { "name": "linux-all",    "inherits": "default",
      "cacheVariables": { "IJKPP_ENABLE_LINUX_NATIVE": "ON", "IJKPP_ENABLE_SDL2": "ON" } },
    { "name": "linux-static-deps", "inherits": "linux-all",
      "cacheVariables": { "IJKPP_LINUX_LINK_RUNTIME": "OFF" } },

    { "name": "asan",     "inherits": "debug", "cacheVariables": { "IJKPP_SANITIZERS": "address;undefined" } },
    { "name": "tsan",     "inherits": "debug", "cacheVariables": { "IJKPP_SANITIZERS": "thread" } },
    { "name": "ubsan",    "inherits": "debug", "cacheVariables": { "IJKPP_SANITIZERS": "undefined" } },
    { "name": "coverage", "inherits": "debug", "cacheVariables": { "IJKPP_COVERAGE": "ON" } },
    { "name": "fuzz",     "inherits": "asan",  "cacheVariables": { "IJKPP_BUILD_FUZZ": "ON", "CMAKE_CXX_COMPILER": "clang++" } },

    { "name": "android-arm64", "inherits": "base",
      "cacheVariables": {
        "CMAKE_TOOLCHAIN_FILE": "$env{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake",
        "ANDROID_ABI": "arm64-v8a", "ANDROID_PLATFORM": "android-21", "ANDROID_STL": "c++_shared",
        "IJKPP_ENABLE_ANDROID": "ON", "IJKPP_ENABLE_SDL2": "OFF", "IJKPP_ENABLE_LINUX_NATIVE": "OFF",
        "IJKPP_FFMPEG_ROOT": "${sourceDir}/prebuilt/ffmpeg/android/arm64-v8a",
        "IJKPP_BUILD_TESTS": "OFF", "IJKPP_BUILD_EXAMPLES": "OFF" } },
    { "name": "ios-device", "inherits": "base",
      "cacheVariables": {
        "CMAKE_SYSTEM_NAME": "iOS", "CMAKE_OSX_ARCHITECTURES": "arm64",
        "CMAKE_OSX_DEPLOYMENT_TARGET": "12.0",
        "IJKPP_ENABLE_IOS": "ON", "IJKPP_ENABLE_SDL2": "OFF", "IJKPP_ENABLE_LINUX_NATIVE": "OFF",
        "IJKPP_FFMPEG_ROOT": "${sourceDir}/prebuilt/ffmpeg/ios/arm64",
        "IJKPP_BUILD_TESTS": "OFF" } }
  ],
  "buildPresets": [
    { "name": "default",      "configurePreset": "default" },
    { "name": "debug",        "configurePreset": "debug" },
    { "name": "release",      "configurePreset": "release" },
    { "name": "no-ffmpeg",    "configurePreset": "no-ffmpeg" },
    { "name": "linux-sdl2",   "configurePreset": "linux-sdl2",   "targets": ["play_sdl2"] },
    { "name": "linux-native", "configurePreset": "linux-native", "targets": ["play_native"] },
    { "name": "linux-all",    "configurePreset": "linux-all" },
    { "name": "asan",         "configurePreset": "asan" },
    { "name": "tsan",         "configurePreset": "tsan" }
  ],
  "testPresets": [
    { "name": "default", "configurePreset": "default",
      "output": { "outputOnFailure": true },
      "execution": { "noTestsAction": "error", "stopOnFailure": false, "jobs": 8 } },
    { "name": "no-ffmpeg", "configurePreset": "no-ffmpeg",
      "output": { "outputOnFailure": true },
      "filter": { "exclude": { "label": "needs-ffmpeg" } } },
    { "name": "headless", "configurePreset": "linux-all",
      "output": { "outputOnFailure": true },
      "filter": { "exclude": { "label": "needs-display" } } },
    { "name": "asan", "configurePreset": "asan", "output": { "outputOnFailure": true } },
    { "name": "tsan", "configurePreset": "tsan", "output": { "outputOnFailure": true },
      "filter": { "exclude": { "label": "no-tsan" } } }
  ]
}
```

---

## 9. `cmake/IjkppCheckInvariants.cmake` — 把设计约束变成 CI

架构约束只写在文档里，半年后一定失效。这里把它变成构建目标。

```cmake
add_custom_target(check-invariants
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_invariants.py
            --root ${CMAKE_SOURCE_DIR}
            --config ${CMAKE_SOURCE_DIR}/tools/invariants.yaml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking architecture & style invariants")

add_custom_target(check-format
    COMMAND ${CMAKE_SOURCE_DIR}/tools/check_format.sh
    COMMENT "Checking clang-format compliance")

add_custom_target(check-no-vendor-leak
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_symbols.py
            $<TARGET_FILE:ijkpp_shared>
            --forbid-regex "^[TDBRWV] (av_|avcodec_|avformat_|swr_|sws_|SDL_|gl[A-Z]|wl_|xcb_)"
    DEPENDS ijkpp_shared
    COMMENT "Verifying no vendor symbols are exported")

add_custom_target(check-cpplint
    COMMAND ${Python3_EXECUTABLE} -m cpplint
            --linelength=80 --quiet --recursive
            ${CMAKE_SOURCE_DIR}/base ${CMAKE_SOURCE_DIR}/media
            ${CMAKE_SOURCE_DIR}/player ${CMAKE_SOURCE_DIR}/platform)

if(IJKPP_IS_TOP_LEVEL)
  add_custom_target(ci-quick DEPENDS check-invariants check-format)
  add_custom_target(ci-full  DEPENDS ci-quick check-cpplint check-no-vendor-leak)
endif()
```

`tools/invariants.yaml`（C1–C19）：

| # | 规则 | 实现 |
|---|---|---|
| C1 | 单文件 ≤ 500 行（白名单：`video_frame_compositor.cc` 700、`ffmpeg_demuxer.cc` 600） | 统计 |
| C2 | 单函数 ≤ 80 行 | libclang AST |
| C3 | 单类 public 方法 ≤ 12（接口类白名单） | libclang AST |
| C4 | `base/` `media/base/` `player/public/` 不含 `libav`/`libsw` include | grep |
| C5 | `base/` `media/` `player/` 不含 `__linux__`/`__APPLE__`/`_WIN32`/`SDL2/`/`X11/`/`wayland-`/`alsa/`/`GL/` | grep |
| C6 | `player/public/*.h` 不含 `media/` `base/` 内部头（白名单：`base/time/time.h`） | grep |
| C7 | `base/` `media/` `player/` 不含裸 `new`/`delete`/`malloc`/`free`（白名单：`base/memory/`） | grep |
| C8 | `#if LIBAV.*VERSION` 只出现在 `platform/ffmpeg/av_includes.h` | grep |
| C9 | `goto` 出现次数 = 0 | grep |
| C10 | include 图无环 | 构建 DAG 检测 |
| C11 | 每个 `<layer>/<module>/` 都有对应 `tests/unit/<layer>_<module>/` | 目录存在性 |
| C12 | 每个 `.cc` 有对应 `.h`（除 `*_main.cc`、`*_unittest.cc`） | 文件配对 |
| C13 | `option_registry.inc` 与 `player_config.h` 同步 | 重新生成后 diff |
| C14 | 头文件无 `using namespace` | grep |
| C15 | `TODO` 不超过 90 天 | git blame |
| C16 | `media/base/` 不 include `media/filters/` | grep |
| C17 | 后缀必须是 `.cc`/`.h`（不允许 `.cpp`） | 文件名 |
| C18 | `base/` `media/base/` `media/filters/` `player/` 中 `throw`/`try`/`catch` = 0 | grep |
| C19 | `WeakPtrFactory` 成员必须是类的**最后一个**成员 | libclang AST |

---

## 10. CI 矩阵（GitHub Actions）

```yaml
name: ci
on: [push, pull_request]

jobs:
  # ---------- 快速门禁：< 3 分钟，不装 FFmpeg、不装 X11 ----------
  quick:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y ninja-build python3-pip
      - run: pip install libclang cpplint
      - run: cmake --preset no-ffmpeg
      - run: cmake --build --preset no-ffmpeg -j
      - run: cmake --build build/no-ffmpeg --target ci-quick
      - run: ctest --preset no-ffmpeg --output-on-failure

  # ---------- 完整矩阵 ----------
  build:
    needs: quick
    strategy:
      fail-fast: false
      matrix:
        include:
          - { os: ubuntu-22.04, preset: linux-all,    ffmpeg: "5.1", label: ffmpeg51 }
          - { os: ubuntu-24.04, preset: linux-all,    ffmpeg: "6.1", label: ffmpeg61 }
          - { os: ubuntu-24.04, preset: linux-all,    ffmpeg: "7.1", label: ffmpeg71 }
          - { os: ubuntu-24.04, preset: linux-native, ffmpeg: "7.1", label: native-only }
          - { os: ubuntu-24.04, preset: linux-sdl2,   ffmpeg: "7.1", label: sdl2-only }
          - { os: ubuntu-24.04, preset: asan,         ffmpeg: "7.1", label: asan }
          - { os: ubuntu-24.04, preset: tsan,         ffmpeg: "7.1", label: tsan }
          - { os: ubuntu-24.04, preset: ubsan,        ffmpeg: "7.1", label: ubsan }
          - { os: ubuntu-24.04, preset: coverage,     ffmpeg: "7.1", label: coverage }
          - { os: ubuntu-24.04, preset: release,      ffmpeg: "7.1", label: release-shared }
          - { os: macos-14,     preset: default,      ffmpeg: "7.1", label: macos }
          - { os: windows-2022, preset: default,      ffmpeg: "7.1", label: windows }
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/setup-deps
        with: { ffmpeg: "${{ matrix.ffmpeg }}", linux_native: "true" }
      - run: cmake --preset ${{ matrix.preset }}
      - run: cmake --build --preset ${{ contains(matrix.preset,'linux') && matrix.preset || 'default' }} -j
      - run: cmake --build build/${{ matrix.preset }} --target ci-full || true
      - run: xvfb-run -a ctest --preset ${{ matrix.preset }} --output-on-failure
      - if: matrix.label == 'coverage'
        run: |
          lcov --capture --directory build/coverage -o cov.info \
               --exclude '*/tests/*' --exclude '*/third_party/*'
          genhtml cov.info -o cov-html
      - uses: actions/upload-artifact@v4
        if: matrix.label == 'coverage'
        with: { name: coverage, path: cov-html }
      - name: Symbol leak check
        if: matrix.label == 'release-shared'
        run: |
          ! nm -D --defined-only build/release/lib/libijkpp.so \
              | grep -E ' T (av_|avcodec_|avformat_|swr_|sws_|SDL_|wl_|xcb_)'

  # ---------- 交叉编译 ----------
  cross:
    strategy:
      matrix: { preset: [android-arm64, android-armv7, ios-device, ios-simulator] }
    runs-on: ${{ contains(matrix.preset, 'ios') && 'macos-14' || 'ubuntu-24.04' }}
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/setup-cross@v1
      - run: cmake --preset ${{ matrix.preset }}
      - run: cmake --build --preset ${{ matrix.preset }} -j

  # ---------- 端到端播放验收（Linux，xvfb + 软渲染） ----------
  e2e-linux:
    needs: quick
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - uses: ./.github/actions/setup-deps
        with: { ffmpeg: "7.1", linux_native: "true" }
      - run: sudo apt-get install -y xvfb mesa-utils libgl1-mesa-dri
      - run: cmake --preset linux-all && cmake --build --preset linux-all -j
      - name: SDL2 backend, 5s playback, verify frame count
        run: |
          xvfb-run -a -s "-screen 0 1280x720x24" \
            ./build/linux-all/bin/play_sdl2 \
            --url tests/testdata/small_h264_aac_3s.mp4 \
            --duration 5s --stats-json /tmp/sdl2.json --exit-code-on-error
          python3 tools/verify_e2e.py /tmp/sdl2.json --min-frames 140 --max-av-diff-ms 40
      - name: Native GL backend (llvmpipe software rasterizer)
        run: |
          export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
          xvfb-run -a -s "-screen 0 1280x720x24" \
            ./build/linux-all/bin/play_native \
            --url tests/testdata/small_h264_aac_3s.mp4 \
            --duration 5s --stats-json /tmp/native.json --exit-code-on-error
          python3 tools/verify_e2e.py /tmp/native.json --min-frames 100 --max-av-diff-ms 60

  # ---------- Fuzz（每日） ----------
  fuzz:
    if: github.event_name == 'schedule'
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: cmake --preset fuzz && cmake --build --preset fuzz -j
      - run: ./build/fuzz/bin/fuzz_demux tests/testdata/corpus -max_total_time=3600 -jobs=4
```

**`e2e-linux` 是"最终要实现 Linux 播放视频"的自动化验收**：用 xvfb + llvmpipe 软渲染，在无显卡的 CI 机器上真实跑起两个后端并断言帧数与 av_diff。这是本设计与纯文档设计最大的差别 —— 目标可被机器验证。

---

## 11. Install / Export

```cmake
# cmake/IjkppInstall.cmake
if(NOT IJKPP_INSTALL)
  return()
endif()

set(_targets ijkpp_base ijkpp_media ijkpp_player ijkpp_platform_null ijkpp)
if(IJKPP_ENABLE_FFMPEG)       list(APPEND _targets ijkpp_platform_ffmpeg) endif()
if(IJKPP_ENABLE_SDL2)         list(APPEND _targets ijkpp_platform_sdl2)   endif()
if(IJKPP_ENABLE_LINUX_NATIVE) list(APPEND _targets ijkpp_platform_linux)  endif()

install(TARGETS ${_targets} EXPORT ijkppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ijkpp)

# ★只装公开头：player/public/ + media/base/ + base/（供写自定义 sink/decoder 的高级用户）
install(DIRECTORY player/public/  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ijkpp/player/public
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY media/base/     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ijkpp/media/base
        FILES_MATCHING PATTERN "*.h" PATTERN "*_export.h")
install(DIRECTORY base/           DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ijkpp/base
        FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_BINARY_DIR}/generated/ijkpp/Version.h"
              "${CMAKE_BINARY_DIR}/generated/ijkpp/BuildConfig.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ijkpp)
# media/filters/ 与 platform/ 的实现头一律不装

install(EXPORT ijkppTargets FILE ijkppTargets.cmake NAMESPACE ijkpp::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ijkpp)
configure_package_config_file(cmake/ijkppConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ijkppConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ijkpp)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ijkppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ijkppConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ijkppConfigVersion.cmake"
              "${CMAKE_SOURCE_DIR}/cmake/FindFFmpeg.cmake"
              "${CMAKE_SOURCE_DIR}/cmake/FindLinuxMediaDeps.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ijkpp)

configure_file(cmake/ijkpp.pc.in "${CMAKE_CURRENT_BINARY_DIR}/ijkpp.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ijkpp.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
```

下游：

```cmake
find_package(ijkpp 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE ijkpp::ijkpp)
```

或不用 CMake：

```bash
g++ -std=c++20 -fno-exceptions -fno-rtti main.cc $(pkg-config --cflags --libs ijkpp) -o app
```

---

## 12. Linux 构建依赖（`docs/BUILDING.md`）

```bash
# Debian / Ubuntu 24.04 —— 完整（双后端）
sudo apt install -y \
  build-essential cmake ninja-build pkg-config python3 python3-pip git \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libsdl2-dev \
  libgl1-mesa-dev libegl1-mesa-dev libglx-dev \
  libx11-dev libxext-dev libxrandr-dev libxpresent-dev libxshmfence-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev libwayland-bin \
  libasound2-dev libpulse-dev \
  libdrm-dev xvfb mesa-utils
pip3 install libclang cpplint

# 最小（只跑核心单测，无需 FFmpeg / X11 / SDL2）
sudo apt install -y build-essential cmake ninja-build python3 python3-pip
pip3 install libclang

cmake --preset no-ffmpeg && cmake --build --preset no-ffmpeg && ctest --preset no-ffmpeg

# 出画（SDL2）
cmake --preset linux-sdl2 && cmake --build --preset linux-sdl2
./build/linux-sdl2/bin/play_sdl2 tests/testdata/small_h264_aac_3s.mp4

# 出画（原生 OpenGL，零第三方播放器依赖）
cmake --preset linux-native && cmake --build --preset linux-native
./build/linux-native/bin/play_native tests/testdata/small_h264_aac_3s.mp4

# 嵌入模式（渲染到你自己的 X11 Window / wl_surface）
./build/linux-all/bin/play_embed --wid 0x04600007 --url video.mp4

# 无显示（CI）
xvfb-run -a ./build/linux-all/bin/play_sdl2 --url video.mp4 --duration 5s \
  --stats-json /tmp/s.json

# 质量构建
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
lcov --capture --directory build/coverage -o cov.info && genhtml cov.info -o cov-html
```

**硬性验证要求**（进 CI）：`cmake --preset no-ffmpeg` 必须在**只装了 g++/cmake/ninja/python3** 的干净容器里成功。这是 G2（核心与 FFmpeg 隔离）的可执行证明。

---

## 13. 产物清单

| 产物 | 内容 | 大小估算 |
|---|---|---|
| `libijkpp_base.a` | `base/` | ~600 KB |
| `libijkpp_media.a` | `media/base` + `media/filters`（非 FFmpeg）+ `media/audio` + `media/renderers` | ~1.4 MB |
| `libijkpp_player.a` | `player/` | ~500 KB |
| `libijkpp_platform_null.a` | Null 后端 | ~30 KB |
| `libijkpp_platform_ffmpeg.a` | FFmpeg 适配 | ~450 KB |
| `libijkpp_platform_sdl2.a` | SDL2 后端 | ~180 KB |
| `libijkpp_platform_linux.a` | 原生 GL + X11/Wayland + ALSA/Pulse | ~700 KB |
| `libijkpp.so`（静态链 FFmpeg，符号隐藏） | 全部 | ~3.8 MB（strip 后 ~1.9 MB） |
| `include/ijkpp/{player/public,media/base,base}/*.h` | 公开头（~60 个） | ~5000 行 |
| `ijkppConfig.cmake` / `ijkpp.pc` | 集成入口 | — |

对比：ijkplayer 的 `libijkplayer.so` + `libijkffmpeg.so` 通常 8–15 MB。

---

## 14. 第三方依赖策略

| 依赖 | 用途 | 引入方式 | 理由 |
|---|---|---|---|
| FFmpeg | 解复用/解码 | **系统**（`find_package`） | 用户已选定；避免 FetchContent 编译 15 分钟 |
| SDL2 | Linux 快速后端 | 系统 | 可选（`IJKPP_ENABLE_SDL2`） |
| OpenGL/EGL/X11/Wayland/ALSA/Pulse | Linux 原生后端 | 系统 + **dlopen** | 弱依赖，单个 .so 到处能跑 |
| GoogleTest / GoogleMock | 测试 | `FetchContent` + `FIND_PACKAGE_ARGS` | 优先系统包 |
| Google Benchmark | 基准 | 同上 | — |
| Abseil | ❌ 不引入 | — | 我们自研 `base/`，引入 absl 会与 `base::expected` 等冲突且增加体积 |
| spdlog / fmt | ❌ 不引入 | — | 自研 `base/logging.h`（~300 行），避免 ABI 传染 |
| SoundTouch | ❌ 不引入 | — | 自研 `AudioRendererAlgorithm`（WSOLA），Δ17 |
| libass | ⏳ 不引入（首期） | — | 字幕输出 `raw_ass` 交上层渲染 |
| libclang (Python) | `check_invariants.py` | pip，仅 CI | 不进产物 |

---

下一篇：[07 测试策略与可观测性](07-测试策略与可观测性.md)
