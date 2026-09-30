// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: DRAFT — NOT YET IN THE BUILD, for the same reason as the header it
// implements (media/base/pipeline.h): neither has been compiled in the
// environment that wrote them. To finish bringing this pair in, add the .cc to
// media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/pipeline.h"

namespace ijkpp::media {

// Out-of-line `= default`, same convention and same reason as
// media/base/demuxer.cc: this translation unit becomes the key function for
// Pipeline's vtable, so it is emitted once rather than in every includer.
// Pipeline::Client needs nothing -- every member is pure virtual and its
// destructor is inline-defaulted.
Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

}  // namespace ijkpp::media
