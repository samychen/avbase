// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
#define AVBASE_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_

namespace avbase::base {

template <typename Sig>
class OnceCallback;
template <typename Sig>
class RepeatingCallback;

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

}  // namespace avbase::base

#endif  // AVBASE_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
