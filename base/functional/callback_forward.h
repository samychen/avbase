// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
#define IJKPP_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_

namespace ijkpp::base {

template <typename Sig>
class OnceCallback;
template <typename Sig>
class RepeatingCallback;

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
