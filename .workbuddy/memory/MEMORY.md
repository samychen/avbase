# avbase 项目长期约定

Chromium 风格分层的 C++ 媒体播放器。目录即 target（`docs/06 §7.6`）。

## 分层判据（docs/02 §6，决策 D11）

`platform/` 装的是「**被宿主选中**」的代码。可移植、各宿主行为一致、不看宿主 SDK 的东西
不进 platform/：FFmpeg 在 `media/ffmpeg/`，GL 渲染层在 `render/gl/`（第五十二轮从
`platform/gl/` 迁出）。

两条正交轴不要压平到同一层：**图形 API 轴**（GL / 未来 Metal、D3D11）与**窗口系统轴**
（SDL2 / GLFW / EGL）。否则加第二个图形 API 就是 N×M 个目录。
参考 OBS：`libobs-opengl/` 是根级模块，窗口系统只是它内部的文件（gl-x11-egl.c / gl-cocoa.m）。

## 提交前必跑的门禁

```sh
python3 tools/check_invariants.py --root .          # C1/C4/C5/C22-C27；C23 列宽棘轮
clang-format --dry-run --Werror   # 必须扫全树 385 个自有文件
cmake -S . -B <dir> -G Ninja -DAVBASE_WERROR=ON -DAVBASE_ENABLE_FFMPEG={ON,OFF} \
      -DAVBASE_ENABLE_SDL2=ON -DAVBASE_BUILD_EXAMPLES=ON -DAVBASE_BUILD_TESTS=ON
ctest --output-on-failure                            # no-ffmpeg 471 / ffmpeg 592
```

- `avbase` 是 INTERFACE 库，ninja 不能 `cmake --build --target avbase`。
- 两个预设都要跑：默认 `macos-sdl2` 预设**不开** `-Werror`，告警只在手动加 `-Werror` 时暴露。
- `tools/deps`(2471) / `tools/build`(160) 属第三方与产物，排除出格式与不变量检查；
  `tools/inspect`(6) 是自有代码，要查。

## 操作教训（都是真踩过的）

- **声称「全树通过」前必须真的扫全树。** 曾连续几轮报告「clang-format 0 偏离」，
  实际只扫了当轮触达目录，全树有 12 个文件偏离。
- **clang-format 与构建不能并发**跑同一批文件：会读到半格式化文件，产生假的超长行报错。
- **后台命令末尾别用 `grep -c` 收尾**：计数为 0 时退出码 1，整条任务被误报 failed。
- 批量改名要用词边界：`platform_gl` 前缀替换会误伤 `platform_glfw`。
- 改 include 路径后必然影响 include 排序，要跟着跑一次 clang-format。
- macOS sed 的 `-i ''` 空参数会被吃掉；用 `-i.bak` 再删。
- 本机无 glfw3 / EGL 开发库（brew TLS 被代理拦），GLFW/EGL 后端只能做结构验证。
-  GitHub raw/API 被代理拦截（CONNECT 502），核实外部仓库布局用 WebSearch。

## 可见性

`CMAKE_CXX_VISIBILITY_PRESET hidden`，导出类需 `AVBASE_MEDIA_EXPORT`。从隐藏可见性的基类
派生导出类会触发 `-Wvisibility` —— 共享基类本身也要导出。

## 文档引用

`docs/09-Linux平台实现方案.md` 已归档到 `docs/archive/`。代码注释用短形式
`docs/archive/09 §N`（沿用仓库按编号引用的约定）；**不要写完整中文文件名**，会撑破 80 列。
