// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// C ABI: the legacy event channel -- docs/08 M15.
//
// The one place the C ABI has real work to do. An ijkplayer's mEventHandler is
// written against a (what, arg1, arg2, obj) quadruple, and this forwards
// avbase::PlayerEvent into exactly that shape so an existing Java handler keeps
// working unchanged (docs/05). Player::ToLegacyEvent() already does the
// mapping; what is added here is the ownership of the C function pointer and
// its user_data, which have to survive as a pair for as long as the Player
// can still emit.

#include <memory>

#include "player/public/c/avbase_c_internal.h"
#include "player/public/player.h"
#include "player/public/player_event.h"

// Events
// ---------------------------------------------------------------------------

extern "C" void avbase_player_set_event_callback(AvbasePlayer* player,
                                                 AvbaseEventCallback cb,
                                                 void* user_data) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  // The bridge is a shared_ptr so the C function pointer and its user_data stay
  // alive until the Player destroys the handler.
  auto bridge = std::make_shared<EventBridge>();
  bridge->callback = cb;
  bridge->user_data = user_data;

  player->player->SetEventHandler(avbase::Player::EventHandler(
      [bridge](const avbase::PlayerEvent& event) {
        if (bridge->callback == nullptr) {
          return;
        }
        avbase::LegacyEvent legacy;
        if (!avbase::ToLegacyEvent(event, &legacy)) {
          // Not expressible in the legacy vocabulary. Reported as what==0
          // rather than dropped, so a host can tell "the engine said something
          // this bridge cannot express" from "nothing happened".
          bridge->callback(bridge->user_data, 0, 0, 0, "");
          return;
        }
        bridge->callback(bridge->user_data, legacy.what, legacy.arg1,
                         legacy.arg2, legacy.obj.c_str());
      }));
}
