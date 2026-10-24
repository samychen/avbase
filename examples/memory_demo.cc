// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// memory_demo.cc — 演示 base/memory/ 的核心所有权与生命周期机制。
//
// 运行方式（手动，针对已构建的 build/full；库里已含 DCHECK）：
//   clang++ -std=c++20 -DAVBASE_ENABLE_DCHECK -I. \
//       examples/memory_demo.cc build/full/lib/libavbase_base.a \
//       -lpthread -o /tmp/memory_demo
//   /tmp/memory_demo
//
// 或通过 CMake（examples/CMakeLists.txt 已加入 memory_demo 目标）：
//   cmake --build build/full --target memory_demo
//   ./build/full/examples/memory_demo     # 路径取决于生成器（Ninja 下通常如此）
//
// 覆盖四个形状：
//   1. scoped_refptr + MakeRefCounted / WrapRefCounted / AdoptRef（三种所有权动词）
//   2. WeakPtr / WeakPtrFactory（跨序列安全 —— avbase 最重要的安全机制）
//   3. ptr_util::WrapUnique（独占所有权）
//   4. raw_ptr<T>（非拥有注解）
//
// 关键教训（见 base/memory/ref_counted.h 的 AddRef 修复）：任何有副作用的表达
// 式都不能藏在断言参数里；这里所有计数变化都是显式语句，与 DCHECK 开关无关。

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"

using namespace avbase;  // 示例而非 SDK：base/ 与 media/ 直接以 base:: / media:: 引用

namespace {

// ---------------------------------------------------------------------------
// 1. 一个可被引用计数的对象
// ---------------------------------------------------------------------------
class Foo : public base::RefCountedThreadSafe<Foo> {
 public:
  Foo() { LOG(INFO) << "[Foo] 构造"; }
  // 暴露内部计数，便于在 demo 里观察引用变化（真实代码一般不放这个方法）。
  int use_count() const { return ref_count(); }
  void Do() const {
    LOG(INFO) << "[Foo::Do] 存活，use_count=" << use_count();
  }

 private:
  // 析构设为 private 并授权给 RefCountedThreadSafe<Foo>：这样禁止在栈上或
  // 用裸 new 直接构造，引用计数归零时由 base 负责 delete。
  friend class base::RefCountedThreadSafe<Foo>;
  ~Foo() { LOG(INFO) << "[Foo] 析构（最后一个引用释放）"; }
};

// ---------------------------------------------------------------------------
// 2. 一个拥有 WeakPtrFactory 的对象
// ---------------------------------------------------------------------------
class Worker {
 public:
  Worker() { LOG(INFO) << "[Worker] 构造"; }
  ~Worker() { LOG(INFO) << "[Worker] 析构"; }
  void Do() const { LOG(INFO) << "[Worker::Do] 收到任务"; }
  base::WeakPtr<Worker> GetWeakPtr() { return factory_.GetWeakPtr(); }

 private:
  // WeakPtrFactory 必须是最后一个成员（门禁规则 C19）：它最先析构，在对象其余
  // 部分销毁之前先把所有悬空 WeakPtr 置失效，避免成员已死后还被解引用。
  base::WeakPtrFactory<Worker> factory_{this};
};

// ---------------------------------------------------------------------------
// 3. 一个独占所有权的对象
// ---------------------------------------------------------------------------
class Bar {
 public:
  Bar() { LOG(INFO) << "[Bar] 构造"; }
  ~Bar() { LOG(INFO) << "[Bar] 析构"; }
};

// ===========================================================================
void Part1_ScopedRefPtr() {
  LOG(INFO) << "========== 第 1 部分：scoped_refptr 三种所有权动词 ==========";

  // (a) MakeRefCounted —— 创造，持有唯一引用（计数 0 -> 1）
  auto a = base::MakeRefCounted<Foo>();
  LOG(INFO) << "(a) MakeRefCounted            a->use_count() = " << a->use_count();

  // (b) 拷贝 scoped_refptr —— 共享（+1）
  base::scoped_refptr<Foo> b = a;
  LOG(INFO) << "(b) 拷贝 scoped_refptr        = " << a->use_count();

  // (c) WrapRefCounted —— 共享一个已由他人持有的裸指针（+1）
  auto c = base::WrapRefCounted(a.get());
  LOG(INFO) << "(c) WrapRefCounted            = " << a->use_count();

  // (d) 释放 b、c
  b.reset();
  c.reset();
  LOG(INFO) << "(d) 释放 b、c 后             = " << a->use_count();

  // (e) AdoptRef —— 接管一个“已经带引用（count>=1）”的裸指针，不再 +1。
  //     典型来源是另一个 scoped_refptr 的 .release()：交出引用但计数保持不变。
  //     （陷阱：把全新的 new Foo() 交给 AdoptRef 会泄漏 —— 那种对象计数为 0，
  //      AdoptRef 不再 +1，析构时 fetch_sub 从 0 减到 -1，永不 delete。）
  auto holder = base::MakeRefCounted<Foo>();  // count = 1
  Foo* raw = holder.release();  // 交出引用但不 Release —— count 仍为 1
  auto d = base::AdoptRef(raw);  // 接管：count 仍为 1（不是 2）
  LOG(INFO) << "(e) AdoptRef(已计数的引用) = " << d->use_count()
            << "   （若误用 WrapRefCounted 会变成 2 -> 析构时漏一次引用）";

  // a 与 d 在此离开作用域：a 降到 0、d 降到 0 -> 各触发一次 [Foo] 析构。
  LOG(INFO) << "(f) a、d 离开作用域 -> 各自触发一次析构";
}

void Part2_WeakPtr() {
  LOG(INFO) << "========== 第 2 部分：WeakPtr / WeakPtrFactory ==========";

  // (a) 拥有者存活时取弱指针并安全解引用
  Worker worker;
  base::WeakPtr<Worker> wp = worker.GetWeakPtr();
  LOG(INFO) << "(a) 拥有者存活：wp 有效 -> ";
  if (wp) {
    wp->Do();
  }

  // (b) 把 WeakPtr 绑进回调：拥有者死后，回调自动变成 no-op（不崩溃）。
  //     这正是 avbase 最重要的安全机制 —— 从结构上消灭 ijkplayer 防不住的
  //     “player 已释放但回调还在飞” 类崩溃。
  base::OnceClosure task_dead;
  {
    Worker w1;
    base::BindOnce(&Worker::Do, w1.GetWeakPtr()).Run();  // 存活 -> 正常执行
    Worker w2;
    task_dead = base::BindOnce(&Worker::Do, w2.GetWeakPtr());  // 绑定，暂不执行
  }  // w2 析构：其 WeakPtrFactory 先把所有 WeakPtr 失效
  LOG(INFO) << "(b) 拥有者已死，再跑绑定的回调 -> ";
  std::move(task_dead).Run();  // BindOnce 检测到弱指针失效 -> 整段 no-op

  // (c) 直接解引用已失效的 WeakPtr 会 CHECK 失败（设计如此：强制你先判空）。
  base::WeakPtr<Worker> dead_wp;
  {
    Worker w3;
    dead_wp = w3.GetWeakPtr();
  }  // w3 析构，dead_wp 失效
  LOG(INFO) << "(c) 正确用法：先判空再解引用 -> ";
  if (dead_wp) {
    dead_wp->Do();
  } else {
    LOG(INFO) << "    dead_wp 为空，安全跳过（若直接解引用会触发 CHECK 失败）";
  }
}

void Part3_WrapUnique() {
  LOG(INFO) << "========== 第 3 部分：ptr_util::WrapUnique（独占）==========";
  auto u = base::WrapUnique(new Bar());
  LOG(INFO) << "(a) WrapUnique 拿到独占所有权；离开作用域时 [Bar] 析构";
}

void Part4_RawPtr() {
  LOG(INFO) << "========== 第 4 部分：raw_ptr<T>（非拥有注解）==========";
  auto foo = base::MakeRefCounted<Foo>();
  base::raw_ptr<Foo> rp = foo.get();  // 仅注解“我不拥有”，运行时等同 T*
  LOG(INFO) << "(a) raw_ptr 解引用：rp->use_count() = " << rp->use_count();
  LOG(INFO) << "(b) raw_ptr 转 bool 判空："
            << (rp ? "非空" : "空");
  rp = nullptr;
  LOG(INFO) << "(c) 置空后判空：" << (rp ? "非空" : "空");
}

}  // namespace

int main() {
  LOG(INFO) << "########## base/memory 演示开始 ##########";
  Part1_ScopedRefPtr();
  Part2_WeakPtr();
  Part3_WrapUnique();
  Part4_RawPtr();
  LOG(INFO) << "########## base/memory 演示结束 ##########";
  return 0;
}
