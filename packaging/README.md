# packaging · 分发与集成

M13 的「cpack-vcpkg-conan」（PROGRESS.md M13 行）。三条集成路径的现状，
按 docs/10 §12 V4「三种集成方式各有 CI job 验证」的口径如实记录——
**没有验证过的路径标未验证，不假装能跑**。

| 方式 | 交付物 | 状态 |
|---|---|---|
| CMake `find_package(avbase)` | `cmake/AvbaseInstall.cmake` + `cmake/AvbaseConfig.cmake.in` | ✅ **本地实测**：install 到 staging 后，树外 consumer 完成 configure/build/run（`avbase::player` 链接，Player 构造出六个线程） |
| CPack | 同上文件内的 CPack 段（TGZ 全平台 / DEB 加 dpkg 系统） | ✅ **TGZ 本地实跑通过**（macOS `cpack -G TGZ` → `avbase-0.1.0-Darwin.tar.gz`，248 条目：静态库 + `lib/cmake/avbase/*` config/targets + `lib/cmake/avbase_gl/*` 独立 config + 头树保持源布局；`find_package(avbase_gl)` 已用树外 consumer 编译链接验证）；DEB/RPM 待 Linux release 机 `cpack -G DEB`（dpkg）一次 |
| vcpkg | `vcpkg-portfile.cmake` | ⬜ 已写好，**未提交 registry、未跑过**（沙箱无 vcpkg）；SHA512 占位，发 tag 后填 |
| conan | `conanfile.py` | ⬜ 已写好，**未提交 conancenter、未跑过**（沙箱无 conan） |
| 手工拷贝 | 头树 + `.a` 直接进宿主编译 | ✅ 头树布局与树内一致（`player/public/...`、`media/base/...`），`install(DIRECTORY)` 已实测落位正确 |
| pkg-config | — | ⬜ 未做（`avbase.pc.in`）；`libavbase.pc` 不是发版 blocker，V4 的三条路径已由前五行覆盖 |

## 已验证的那条路径怎么复现

```bash
cmake -S . -B build/macos-sdl2          # 或 linux-sdl2 / full
cmake --build build/macos-sdl2
DESTDIR=/tmp/stage cmake --install build/macos-sdl2 --prefix /usr/local

# 树外 consumer（CMake 集成方式）：
mkdir /tmp/consumer && cd /tmp/consumer
#   CMakeLists.txt: project(consumer C CXX); find_package(avbase 0.1.0 REQUIRED)
#                   target_link_libraries(consumer PRIVATE avbase::player)
cmake -S . -B build \
    -DCMAKE_PREFIX_PATH="/tmp/stage/usr/local;<deps 树，含 FFmpeg 与 libyuv>"
cmake --build build && ./build/consumer
```

**consumer 侧三条须知**（都是实测撞出来的，不是猜的）：

1. `project()` 要带 **C**——包内 `FindFFmpeg.cmake` 的版本探针 try_run 一个
   `.c` 源，只启用 CXX 的工程会报 `Unknown extension ".c"`。
2. 需要 **FFmpeg + libyuv** 可达：静态库把 PRIVATE 依赖以
   `$<LINK_ONLY:...>` 带进导出接口，这是静态分发的固有形态（动态库没有
   这个问题）。两个 Find 模块随包分发，`CMAKE_PREFIX_PATH` 指到就行。
3. 版本头是 **`avbase/Version.h`**（generated），不是
   `player/public/version.h`——后者在被删（README 结构树记着）。

## 未验证路径的验证计划（到点即做）

- vcpkg/conan：等发布 tag（v0.1.0）后填 `SHA512`/确认 `version`，在装好
  vcpkg/conan 的机器上跑一次 `vcpkg install avbase` / `conan create .`，
  结果回填本表。**顺序不能反**：没跑过的 portfile 提交上去只会得到红色 CI。
- CPack：release 机上 `cpack -G TGZ`（+ `DEB`）各一次，`dpkg -c` 核对文件
  清单，再在 README 挂下载链接。

返回：[README](../README.md) ｜ [docs/10 §9 打包与集成](docs/10-SDK易用性设计.md)
