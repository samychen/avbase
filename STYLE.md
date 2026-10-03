# avbase 代码风格细则（Google C++ Style + Chromium 约定）

> 基线：[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
> 叠加：Chromium 工程约定（`base/` 命名、`DCHECK`、sequence 模型、注释风格）
> 工具：`.clang-format`（唯一格式真相源）、`.clang-tidy`、`cpplint`、`tools/check_invariants.py`
> 违反任一"强制"项 → CI fail，不允许豁免。
>
> ⚠️ **工具落地状态**：`.clang-format` ✅ 存在 · `tools/check_invariants.py` ✅
> 存在（已进 CI）· **`check-format` job ✅ 已接入 CI**（第二十三轮）·
> `.clang-tidy` ✅ 已落盘但**仍未接入 CI** · `cpplint` ❌ 未接入。
> 因此下文标注 clang-tidy / cpplint 的规则**目前只靠人工评审执行**。
>
> **一次性全量重排（第二十三轮）**：此前不做全量重排的理由是"那 323 行里有一部分
> 是模板声明，手工重排有可能改变含义，而收益只有行更短"。这个权衡已经被
> **机器重排**推翻——`clang-format -i` 不改语义，只按 `.clang-format` 重新折行，
> 于是"手工重排可能改变含义"从来不是真实风险，而"收益只有行更短"低估了收益：
> C23 基线从 **286 行降到 37 行**（少 249 行），且 4K 条以上的 diff 一次性付清，
> 之后每个 PR 不再为格式付费。
>
> 重排的两个真实副作用，已如实处理而非回避：`ffmpeg_demuxer.cc` 与
> `audio_renderer_algorithm.cc` 因折行方式变化各涨了几行，触及 C1 上限，
> 上限随之棘轮化（并注明"只许降不许升"）。
>
> **`check-format` 检查全树而非只查 diff**。只查 diff（`git-clang-format` 那种）
> 更便宜、也更容易绕开，而本项的意义正是"`.clang-format` 是格式的唯一真相源"
> ——只有任何位置的漂移都被抓到，这个说法才成立。
>
> **列宽**：`check_invariants.py` 的 **C23** 规则以**棘轮**方式守 80 列——
> 基线 `tools/column_baseline.txt` 记录现存超长行（当前 **37 行 / 7 个文件**），
> 规则只拦"某文件超长行数变多"与"新文件有超长行"，`--update-baseline`
> **拒绝提高任何既有条目**。这 37 行是 `clang-format` 自己也折不动的（宏续行、
> 长字面量等），留给人工判断。
>
> **棘轮的意义是：从此每个因别的原因被改到的文件，都必须顺手清干净，且清完不会退化。**

---

## 1. 语言与编译约束（强制）

| 项 | 规定 | 理由 |
|---|---|---|
| 标准 | **C++20**，禁用 GNU 扩展（`-std=c++20`，不加 `gnu++20`） | Chromium 当前标准 |
| **异常** | **全局禁用**（`-fno-exceptions`） | Google/Chromium 明确规定；`base::expected` 的 Chromium 版本在异常点直接 terminate |
| **RTTI** | **全局禁用**（`-fno-rtti`） | Chromium 规定；类型擦除用 `StorageType` 枚举，不用 `dynamic_cast` |
| `new` / `delete` | 核心代码禁止直接使用 | 用 `std::make_unique` / `base::WrapUnique` / `scoped_refptr` |
| `malloc` / `free` | 仅允许出现在 `base/memory/allocator_shim.cc` 等白名单文件 | — |
| `goto` | **出现次数必须为 0** | check_invariants C9 |
| `reinterpret_cast` | 仅限 `platform/` 与 `media/filters/ffmpeg/`，且必须加注释说明安全性 | — |
| C 风格转换 `(T)x` | 禁止 | `-Wold-style-cast` |
| 裸 `T*` 表示所有权 | 禁止 | 所有权只用 `unique_ptr` / `scoped_refptr`；非拥有用 `raw_ptr<T>` 或 `T&` |
| `using namespace` 在头文件 | 禁止 | check_invariants C14 |
| `std::shared_ptr` | **不推荐**，用 `scoped_refptr` + `base::RefCountedThreadSafe` | 与 Chromium 一致；`scoped_refptr` 无 `weak_ptr` 开销，控制块更小 |
| 共享所有权的三个动词 | `base::MakeRefCounted<T>(...)` **创造** · `base::WrapRefCounted(p)` **共享** · `base::AdoptRef(p)` **接管** | 三者都在 `base/memory/scoped_refptr.h` 底部（不在 `ptr_util.h`：adopt 需要 `scoped_refptr` 的 tag ctor）。选错一个是生命周期 bug 而不是风格问题——`AdoptRef` 曾长期被写成 AddRef，第九轮修掉 |
| 全局可变状态 | 禁止（`base::FeatureList` 与单例 logger 除外，且只读初始化） | ijkplayer 的 `g_ijkmp_*` 教训 |
| `std::function` | 公开 API 可用；内部一律用 `base::OnceCallback` / `RepeatingCallback` | 后者 move-only、零分配路径可测 |

## 2. 文件与命名（强制）

| 项 | 规定 | 示例 |
|---|---|---|
| 源文件后缀 | **`.cc`**（不是 `.cpp`） | `video_frame_compositor.cc` |
| 头文件后缀 | `.h` | `video_frame_compositor.h` |
| 文件名 | `lower_snake_case` | `decoder_buffer.h` |
| 头文件保护 | `#ifndef` 全路径宏，**不用 `#pragma once`** | `#ifndef AVBASE_MEDIA_BASE_VIDEO_FRAME_H_` |
| 类型 / 类 / 枚举 | `CamelCase` | `class VideoFrameCompositor;` |
| **方法 / 函数** | **`CamelCase()`**（Google 风格，不是 `camelCase`） | `void StartPlayingFrom(base::TimeDelta time);` |
| **访问器（getter）** | `snake_case()`，**不加 `Get` 前缀**（Chromium 约定） | `base::TimeDelta timestamp() const;` |
| 变量 / 参数 | `lower_snake_case` | `int frame_count` |
| **成员变量** | `lower_snake_case_`（**尾下划线**） | `base::Lock lock_;` |
| 常量 | `k` + `CamelCase` | `constexpr int kMaxVideoFrames = 200;` |
| 枚举值 | `k` + `CamelCase`（`enum class`） | `enum class DecoderStatus { kOk, kDecodeError };` |
| 命名空间 | 全小写 | `avbase::media` |
| 宏 | `AVBASE_` 前缀 + 全大写下划线 | `AVBASE_MEDIA_EXPORT` |
| 测试文件 | `*_unittest.cc`（单测）/ `*_test.cc`（集成） | `video_frame_compositor_unittest.cc` |
| 测试类 | `XxxTest`，用例名 `CamelCase` 描述行为 | `TEST(VideoFrameCompositorTest, DropsLateFrame)` |

### 2.1 关于 getter 命名的说明

Google Style 原文允许 `Foo()` 或 `foo()`。**Chromium 的实际约定是：与成员变量同名的 `snake_case()` 作为 getter**（如 `video_frame.h` 的 `timestamp()`、`coded_size()`、`visible_rect()`）。avbase 采用 Chromium 约定，因为：
1. 我们全程参照 Chromium media 的代码，命名一致才能"对照阅读"
2. `frame.timestamp()` 比 `frame.GetTimestamp()` 噪音更小
3. 与 `base::TimeDelta::InMilliseconds()` 这类"动作型"方法天然区分：`snake_case()` = 取值，`CamelCase()` = 做事

## 3. 头文件（强制）

```cpp
// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Portions of this file's API mirror Chromium's `media/base/video_frame.h`
// (BSD-3-Clause, Copyright The Chromium Authors).

#ifndef AVBASE_MEDIA_BASE_VIDEO_FRAME_H_
#define AVBASE_MEDIA_BASE_VIDEO_FRAME_H_

#include <stdint.h>

#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "avbase/base_export.h"
#include "media/base/video_frame_metadata.h"

namespace avbase {
namespace media {

class VideoFramePool;

// A video frame, analogous to Chromium's `media::VideoFrame`.
//
// Instances are reference-counted and must always be held via
// `scoped_refptr<VideoFrame>`. ...
class AVBASE_MEDIA_EXPORT VideoFrame
    : public base::RefCountedThreadSafe<VideoFrame> {
 public:
  enum class StorageType { ... };

  VideoFrame(const VideoFrame&) = delete;
  VideoFrame& operator=(const VideoFrame&) = delete;

  base::TimeDelta timestamp() const { return metadata_.timestamp; }
  ...

 private:
  friend class base::RefCountedThreadSafe<VideoFrame>;
  ~VideoFrame();

  VideoFrameMetadata metadata_;
};

}  // namespace media
}  // namespace avbase

#endif  // AVBASE_MEDIA_BASE_VIDEO_FRAME_H_
```

规则：
- **include 顺序**：对应 `.h` → C 系统头 → C++ 标准库 → 其他库 → 本项目（`base/...` → `media/...` → `player/...`）。每块之间空行。
- 只 include 直接依赖（IWYU 原则），CI 跑 `include-what-you-use`。
- 能用前置声明就不 include。
- **公开头文件不得 include 任何 `libav*.h` / `libsw*.h`**（check_invariants C4）。
- 类声明顺序：`public` → `protected` → `private`；每段内 `using`/`enum` → 构造析构 → 方法 → 成员。
- `= delete` 拷贝构造与拷贝赋值必须显式写出（Chromium 要求，防止误拷贝）。

## 4. 注释（强制）

- 文件头版权声明 + （如适用）Chromium API 对照说明。
- 每个 `public` 类写一段"这个类是什么、在哪个 sequence 上用、所有权如何"的注释 —— 这是 Chromium 头文件最有价值的部分，必须照做。
- **线程/sequence 约束必须写在类注释里**，格式固定：

```cpp
// All methods must be called on the same sequence, except for GetMediaTime()
// which is safe to call from any thread.
```

- 参数用 `|param|` 竖线包裹（Chromium 风格）：`// Requests |buffer| to be decoded.`
- 从 ijkplayer 移植的算法必须在函数头注明出处：

```cpp
// Computes the delay before presenting the next frame.
//
// Ported line-by-line from ijkplayer's `compute_target_delay()` in
// ijkmedia/ijkplayer/ff_ffplay.c (LGPL-2.1). Thresholds are intentionally
// identical; see docs/05-迁移对照表.md table 7. Do not "improve" without
// golden-test evidence.
```

- `TODO(username): description  crbug/…` → avbase 用 `TODO(avbase/123): description`，CI 检查超过 90 天未处理的 TODO。
- 不写废话注释（`// 递增 i`）。

## 5. 函数与类设计（强制）

| 规则 | 说明 |
|---|---|
| 单函数 ≤ 80 行 | check_invariants C2；超出必须拆分 |
| 单文件 ≤ 500 行 | check_invariants C1 |
| 单类 public 方法 ≤ 12 | check_invariants C3；`VideoDecoder` 这类接口例外需登记 |
| 参数 ≤ 6 个 | 超出改用 `struct XxxParams` |
| 出参用指针且必须非空 | Google 约定：`bool Parse(std::string_view s, int* out);`，`out` 必须 `DCHECK(out)` |
| 返回值用 `base::expected<T, E>` | 替代"bool + 出参"或错误码 |
| 单参构造函数必须 `explicit` | `-Wextra-semi` + clang-tidy `google-explicit-constructor` |
| 虚析构函数 | 所有有虚函数的类必须 `virtual ~T()` |
| `override` | 强制写，clang-tidy `modernize-use-override` + `-Wsuggest-override` |
| `final` | 叶子实现类一律 `final`（利于 devirtualization） |
| 结构体成员初始化 | 声明处给默认值：`int frame_count{0};`（Google 要求 POD 成员显式初始化） |

## 6. 错误处理（强制）

```cpp
// 返回错误
base::expected<void, MediaError> Initialize(const VideoDecoderConfig& config);

// 调用方
base::expected<DemuxerResult, MediaError> result = demuxer->Initialize(...);
if (!result.has_value()) {
  MEDIA_LOG(ERROR, media_log) << "demuxer init failed: " << result.error();
  return base::unexpected(result.error());
}

// 宏（等价 Chromium base/expected_macros.h）
RETURN_IF_ERROR(demuxer->Initialize(config));
ASSIGN_OR_RETURN(auto info, demuxer->GetMediaInfo());
```

- **不使用异常**。任何第三方库抛出的异常必须在边界处 `try/catch` 转换为 `MediaError`（仅在 `platform/` 与 `media/filters/ffmpeg/` 允许出现 `try`，且需注释说明）。
- `CHECK()` = 不可恢复的编程错误，立即崩溃并打印栈（release 也生效）。
- `DCHECK()` = 可恢复的编程错误，debug 生效。
- **不允许用 `DCHECK` 代替对不可信输入（媒体数据）的校验** —— 媒体数据一律走 `base::expected` 返回错误。
- 错误消息必须包含：出错的上下文、期望值 vs 实际值、以及**建议的修复动作**（见 [docs/10](docs/10-SDK易用性设计.md) §3）。

## 7. 并发（强制）

```cpp
class VideoRendererImpl : public Renderer {
 public:
  VideoRendererImpl(...);

  // Renderer implementation. Must be called on |task_runner|.
  void Initialize(...) override;

  // Thread-safe.
  base::TimeDelta GetMediaTime() override;

 private:
  // Called on |task_runner|.
  void OnDecodeOutput(scoped_refptr<VideoFrame> frame);

  SEQUENCE_CHECKER(sequence_checker_);

  // Owned by |task_runner|. GUARDED_BY_CONTEXT(sequence_checker_).
  std::unique_ptr<VideoFrameCompositor> compositor_;

  // Accessed from any thread.
  base::AtomicSequenceNumber frames_dropped_;
};
```

- 每个类必须在注释与 `SEQUENCE_CHECKER` 中声明它的 sequence 归属。
- 方法注释必须写 `// Called on |task_runner|.` 或 `// Thread-safe.`
- 成员变量用 `GUARDED_BY(lock_)` / `GUARDED_BY_CONTEXT(sequence_checker_)` 注解（clang thread-safety analysis，`-Wthread-safety` 打开）。
- 锁的获取顺序全局唯一，写在 `docs/04` §3，违反由 `LockOrderChecker` 在 debug 构建 assert。
- **跨 sequence 通信只用 `PostTask` + `base::BindOnce` + `WeakPtr`**，禁止直接调另一个 sequence 拥有的对象方法。
- 回调里持有 `this` 必须用 `weak_factory_.GetWeakPtr()`（Chromium 铁律）。

## 8. `.clang-tidy` 配置

> **状态：`.clang-tidy` 已于第十轮落盘，内容即本节**（见文首的工具落地状态），
> 但仍**未接入 CI**；`check-clang-tidy`、`check-cpplint`、`check-format` 三个 job
> 都还欠（docs/07 §13 的门禁清单里有它们）。本节配置就是落盘时的照抄源，保持不动。

```yaml
Checks: >
  -*,
  bugprone-*,
  google-*,
  modernize-*,
  performance-*,
  portability-*,
  readability-identifier-naming,
  readability-redundant-*,
  cppcoreguidelines-interfaces-global-init,
  cppcoreguidelines-pro-type-member-init,
  misc-unused-*,
  -google-readability-todo,
  -modernize-use-trailing-return-type,
  -bugprone-easily-swappable-parameters,
  -readability-identifier-length
CheckOptions:
  readability-identifier-naming.ClassCase: CamelCase
  readability-identifier-naming.StructCase: CamelCase
  readability-identifier-naming.EnumCase: CamelCase
  readability-identifier-naming.EnumConstantPrefix: k
  readability-identifier-naming.FunctionCase: CamelCase
  readability-identifier-naming.MethodCase: CamelCase
  readability-identifier-naming.VariableCase: lower_case
  readability-identifier-naming.MemberSuffix: _
  readability-identifier-naming.PrivateMemberSuffix: _
  readability-identifier-naming.ParameterCase: lower_case
  readability-identifier-naming.ConstantPrefix: k
  readability-identifier-naming.NamespaceCase: lower_case
  readability-identifier-naming.MacroCase: UPPER_CASE
  modernize-use-nullptr.NullMacros: 'NULL'
WarningsAsErrors: '*'
HeaderFilterRegex: '(base|media|player|platform)/.*\.h$'
FormatStyle: file
```

额外：`cpplint --filter=+whitespace/comments,+build/include_order --linelength=80`，以及 Chromium 的 `presubmit` 思路——`tools/check_invariants.py`（见 [docs/06](docs/06-CMake工程与构建体系.md) §9）。

## 9. Git 提交规范

```
<layer>: <一句话祈使句，≤72 字符>

<为什么改，而不是改了什么。必要时引用 docs/ 与 issue。>

Bug: avbase/123
Test: media/filters/video_frame_compositor_unittest.cc
```

前缀 `<layer>` 取 `base` / `media` / `player` / `platform/linux` / `platform/sdl2` / `cmake` / `docs` / `tools` / `tests`。例：

```
media/filters: fix frame drop when master clock serial changes

After a seek, |last_frame| and |next_frame| can belong to different
serials. compute_target_delay() then derived a bogus duration from the
pts delta across the seek boundary, causing a multi-second stall.

This mirrors ijkplayer's `lastvp->serial == nextvp->serial` guard in
video_refresh(); the guard was lost during the port.

Bug: avbase/45
Test: media/filters/video_frame_compositor_unittest.cc (SeekAfterSerialChange)
```

---

## 10. 与 Chromium 的对照速查

| Chromium | avbase | 差异 |
|---|---|---|
| `base::` | `avbase::base::` | 多一层 `avbase` 命名空间，避免与真正的 Chromium 冲突 |
| `media::` | `avbase::media::` | 同上 |
| `MEDIA_EXPORT` | `AVBASE_MEDIA_EXPORT` | — |
| `base/types/expected.h` | `base/types/expected.h` | API 同构；C++23 时 alias `std::expected` |
| `base/expected_macros.h` | 同名 | `RETURN_IF_ERROR` / `ASSIGN_OR_RETURN` |
| `base/functional/callback.h` | 同名 | 仅实现 `OnceCallback` / `RepeatingCallback` 子集 |
| `base/functional/bind.h` | 同名 | `BindOnce` / `BindRepeating`，支持 lambda + 绑定参数 + weak ptr |
| `base/memory/scoped_refptr.h` | 同名 | 完整实现；`MakeRefCounted`/`WrapRefCounted`/`AdoptRef` 都在此文件底部 |
| `base/memory/ptr_util.h` | 同名 | 仅 `WrapUnique`；ref-counted 的三个动词刻意不在此处 |
| `base/memory/weak_ptr.h` | 同名 | 完整实现（sequence-bound） |
| `base/time/time.h` | 同名 | `Time` / `TimeTicks` / `TimeDelta` 完整实现 |
| `base/task/sequenced_task_runner.h` | 同名 | 完整实现 |
| `base/threading/thread.h` | 同名 | Linux 用 epoll-based message pump；其他平台首期不支持 |
| `base/observer_list.h` | 同名 | 完整实现（含 `ScopedObservation`） |
| `base/check.h` / `base/logging.h` | 同名 | 精简版，`LOG()` 走 `base::LoggingDelegate` 可插拔 |
| `base/trace_event/` | `base/trace_event/` | 默认 no-op；可接 Perfetto/Chrome tracing JSON |
| `media::DecoderBuffer` | `media::DecoderBuffer` | 同名同语义（= ijkplayer 的 `AVPacket` 封装） |
| `media::VideoFrame` | `media::VideoFrame` | 同名同语义 |
| `media::AudioBus` | `media::AudioBus` | 同名同语义 |
| `media::DemuxerStream` | `media::DemuxerStream` | `Read(count, ReadCB)` + `Status` 完全对齐 |
| `media::VideoDecoder` | `media::VideoDecoder` | `Initialize/Decode/Reset` 完全对齐 |
| `media::Renderer` | `media::Renderer` | 对齐；去掉 Mojo/CDM/Flinging 等浏览器专属部分 |
| `media::Pipeline` + `PipelineController` | `media::Pipeline` + `PipelineController` | 对齐 |
| `media::VideoFrameCompositor` | `media::VideoFrameCompositor` | ★对应 ijkplayer 的 `video_refresh` + `compute_target_delay` |
| `media::VideoRendererImpl` / `AudioRendererImpl` | 同名 | 含三级 watermark 逻辑 |
| `media::DecoderStream<T,D>` + `DecoderSelector` | 同名 | 解码器选择 + 回退 + 异步泵 |
| `media::AudioRendererSink` | `media::AudioRendererSink` | `RenderCallback::Render(delay, delay_timestamp, glitch, AudioBus*)` 完全对齐 |
| `media::VideoRendererSink` | `media::VideoRendererSink` | `RenderCallback::Render(deadline_min, deadline_max, ...)` |
| `media::MediaLog` | `media::MediaLog` | 结构化媒体事件日志 |
| `media::AudioRendererAlgorithm` | `media::AudioRendererAlgorithm` | ★WSOLA 变速，替代 ijkplayer 的 SoundTouch 依赖 |
| `mojo::` / `blink::` / `cc::` / `viz::` | ❌ 不引入 | 浏览器专属，SDK 不需要 |
| `media::CdmContext` | 🔜 预留接口不实现 | DRM 超范围 |
