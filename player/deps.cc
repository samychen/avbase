// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/deps.h"

#include "base/time/default_tick_clock.h"
// Complete types for the scoped_refptr fields: releasing one calls
// RefCountedThreadSafe<T>::Release(), which needs T to be complete. This is
// exactly why Deps' special members are out-of-line -- so that the public
// header can keep forward-declaring the media interfaces (invariant C22 is
// not at stake here, player/ may include media/, but the public surface stays
// small and does not drag five media headers into every SDK user).
#include "media/base/audio_decoder_factory.h"
#include "media/base/data_source.h"
#include "media/base/video_decoder_factory.h"

namespace ijkpp {

// Out-of-line special members: Deps holds scoped_refptrs to forward-declared
// interfaces, and releasing one needs the complete type. Keeping them here
// means player/public/deps.h itself never has to include a media/base
// interface header. (This comment predates the ninth round and described the
// fields correctly before the header did; the two duplicated paragraphs that
// used to follow it said the same thing twice and are gone.)
Deps::Deps() = default;
Deps::Deps(Deps&&) noexcept = default;
Deps& Deps::operator=(Deps&&) noexcept = default;
Deps::~Deps() = default;

// static
Deps Deps::CreateDefault() {
  Deps deps;
  // A non-owning alias to the process-wide singleton, whose lifetime is the
  // whole process, so holding it by shared_ptr with a no-op deleter is safe.
  deps.tick_clock = std::shared_ptr<const base::TickClock>(
      base::DefaultTickClock::GetInstance(), [](const base::TickClock*) {});
  // The remaining fields stay null: the platform backend selected at M10-M12
  // fills them in, and a null field means "auto-detect" (design rule E1).
  return deps;
}

}  // namespace ijkpp
