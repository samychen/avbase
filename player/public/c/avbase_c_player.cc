// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// C ABI: player surface -- docs/08 M15.
//
// Lifecycle, transport control, queries, media info and multi-source handles.
// Every function forwards to avbase::Player; see avbase_c.cc's header comment
// for why there is deliberately no C-side logic here.
//
// NO EXCEPTIONS: the tree builds with -fno-exceptions, so nothing can unwind
// through a C frame and these entry points need no unwinding guard. Failures
// arrive as Status and are converted to AvbaseStatus by ToC().

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "player/public/c/avbase_c_internal.h"

// Version / global
// ---------------------------------------------------------------------------

extern "C" const char* avbase_c_version(void) {
  // Bumped by hand and only on a breaking change to this header. A host built
  // against an older header keeps working against a newer library, because new
  // symbols are purely additive.
  return "1.0";
}

extern "C" const char* avbase_engine_version(void) {
  return avbase::GetVersion();
}

extern "C" void avbase_global_init(void) {
  avbase::GlobalInit();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

extern "C" const char* avbase_status_name(AvbaseStatus status) {
  switch (status) {
    case AVBASE_OK:
      return "kOk";
    case AVBASE_ERR_INVALID_ARGUMENT:
      return "kInvalidArgument";
    case AVBASE_ERR_INVALID_STATE:
      return "kInvalidState";
    case AVBASE_ERR_NOT_IMPLEMENTED:
      return "kNotImplemented";
    case AVBASE_ERR_OUT_OF_MEMORY:
      return "kOutOfMemory";
    case AVBASE_ERR_ABORTED:
      return "kAborted";
    case AVBASE_ERR_TIMEOUT:
      return "kTimeout";
    case AVBASE_ERR_SOURCE_OPEN_FAILED:
      return "kSourceOpenFailed";
    case AVBASE_ERR_SOURCE_NOT_FOUND:
      return "kSourceNotFound";
    case AVBASE_ERR_SOURCE_READ_FAILED:
      return "kSourceReadFailed";
    case AVBASE_ERR_DECODER_NOT_FOUND:
      return "kDecoderNotFound";
    case AVBASE_ERR_DECODER_OPEN_FAILED:
      return "kDecoderOpenFailed";
    case AVBASE_ERR_DECODE_FAILED:
      return "kDecodeFailed";
    case AVBASE_ERR_DECODER_UNSUPPORTED_CODEC:
      return "kDecoderUnsupportedCodec";
    case AVBASE_ERR_SINK_NOT_ATTACHED:
      return "kSinkNotAttached";
    case AVBASE_ERR_STREAM_NOT_FOUND:
      return "kStreamNotFound";
    case AVBASE_ERR_TRACK_NOT_FOUND:
      return "kTrackNotFound";
    case AVBASE_ERR_CONFIG_INVALID:
      return "kConfigInvalid";
    case AVBASE_ERR_EOS:
      return "kEos";
    default:
      return "kUnknown";
  }
}

extern "C" const char* avbase_status_message(AvbaseStatus status) {
  // The engine owns the sentences, so the C ABI never grows a second copy that
  // could drift from the C++ one. An unnamed code yields "" as promised.
  const char* name = avbase::GetErrorCodeName(
      static_cast<avbase::ErrorCode>(status));
  return name != nullptr ? name : "";
}

// ---------------------------------------------------------------------------
// Player lifecycle
// ---------------------------------------------------------------------------

extern "C" AvbasePlayer* avbase_player_create(void) {
  avbase::GlobalInit();
  // The nothrow forms are what make the documented "null means out of memory"
  // contract actually hold: under -fno-exceptions a plain new would terminate
  // the process instead of returning null.
  std::unique_ptr<AvbasePlayer> handle(new (std::nothrow) AvbasePlayer());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->player = std::unique_ptr<avbase::Player>(new (std::nothrow)
                                                        avbase::Player());
  if (handle->player == nullptr) {
    return nullptr;
  }
  return handle.release();
}

extern "C" void avbase_player_destroy(AvbasePlayer* player) {
  // Destroying null is a no-op, so a cleanup path can call it unconditionally
  // after a failed create.
  delete player;
}

extern "C" void avbase_player_stop_sync(AvbasePlayer* player,
                                        int64_t timeout_ms) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  player->player->StopSync(
      avbase::base::Milliseconds(std::max<int64_t>(0, timeout_ms)));
}

extern "C" AvbaseStatus avbase_player_set_data_source(AvbasePlayer* player,
                                                     const char* uri) {
  if (player == nullptr || player->player == nullptr || uri == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->SetDataSource(std::string_view(uri)));
}

extern "C" AvbaseStatus avbase_player_prepare(AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->PrepareAsync());
}

extern "C" AvbaseStatus avbase_player_prepare_sync(AvbasePlayer* player,
                                                   int64_t timeout_ms) {
  if (player == nullptr || player->player == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->PrepareSync(
      avbase::base::Milliseconds(std::max<int64_t>(0, timeout_ms))));
}

// ---------------------------------------------------------------------------
// Player transport
// ---------------------------------------------------------------------------

extern "C" void avbase_player_start(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Start();
  }
}

extern "C" void avbase_player_pause(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Pause();
  }
}

extern "C" void avbase_player_stop(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Stop();
  }
}

extern "C" void avbase_player_reset(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Reset();
  }
}

extern "C" void avbase_player_step_once(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->StepOnce();
  }
}

extern "C" void avbase_player_seek_to(AvbasePlayer* player,
                                      int64_t position_ms) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  // Fire and forget on purpose. The C++ SeekTo returns a request id so a host
  // can correlate rapid seeks, but that id also arrives in the event payload --
  // a host needing per-request correlation should use the C++ API.
  player->player->SeekTo(
      avbase::base::Milliseconds(std::max<int64_t>(0, position_ms)));
}

extern "C" void avbase_player_set_playback_rate(AvbasePlayer* player,
                                                double rate) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetPlaybackRate(rate);
  }
}

extern "C" void avbase_player_set_volume(AvbasePlayer* player, double volume) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetVolume(volume);
  }
}

extern "C" void avbase_player_set_muted(AvbasePlayer* player, int muted) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetMuted(muted != 0);
  }
}

extern "C" void avbase_player_set_loop_count(AvbasePlayer* player, int count) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetLoopCount(count);
  }
}

extern "C" AvbaseStatus avbase_player_select_track(AvbasePlayer* player,
                                                    int stream_type,
                                                    int stream_index) {
  if (player == nullptr || player->player == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  const auto type = FromStreamType(stream_type);
  // kUnknown means the caller passed a code this ABI does not name. Saying so
  // beats letting it reach the demuxer and come back as a missing track: the
  // two
  // have completely different fixes.
  if (type == avbase::media::DemuxerStreamType::kUnknown) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->SelectTrack(type, stream_index));
}

extern "C" void avbase_player_set_video_surface(
    AvbasePlayer* player, AvbaseNativeDisplay* display) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  // A null display means "render nowhere but keep the audio" -- the documented
  // background-playback case, not an error.
  player->player->SetVideoSurface(display != nullptr ? display->display
                                                     : nullptr);
}

extern "C" AvbaseStatus avbase_player_take_snapshot(AvbasePlayer* player,
                                                    int64_t at_ms,
                                                    const char* file_path) {
  if (player == nullptr || player->player == nullptr || file_path == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->TakeSnapshot(
      avbase::base::Milliseconds(std::max<int64_t>(0, at_ms)),
      std::string(file_path)));
}

extern "C" void avbase_player_reconnect_now(AvbasePlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->ReconnectNow();
  }
}

// ---------------------------------------------------------------------------
// Player queries: lock-free snapshots, callable from any thread
// ---------------------------------------------------------------------------

extern "C" int avbase_player_state(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return AVBASE_STATE_IDLE;
  }
  return static_cast<int>(player->player->state());
}

extern "C" int64_t avbase_player_media_time_ms(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetMediaTime());
}

extern "C" int64_t avbase_player_buffered_time_ms(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetBufferedTime());
}

extern "C" int64_t avbase_player_duration_ms(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetDuration());
}

extern "C" int avbase_player_is_live(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->is_live() ? 1 : 0;
}

extern "C" int avbase_player_is_playing(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->IsPlaying() ? 1 : 0;
}

extern "C" int32_t avbase_player_video_width(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_natural_size().width;
}

extern "C" int32_t avbase_player_video_height(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_natural_size().height;
}

extern "C" int32_t avbase_player_video_rotation(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_rotation();
}

extern "C" double avbase_player_playback_rate(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 1.0;
  }
  return player->player->playback_rate();
}

extern "C" double avbase_player_volume(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0.0;
  }
  return player->player->volume();
}

extern "C" int avbase_player_muted(const AvbasePlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->muted() ? 1 : 0;
}

extern "C" int32_t avbase_player_title(AvbasePlayer* player, char* out,
                                       int32_t capacity) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  // MediaInfo has no title field; the URI is what a window title would show
  // anyway, and synthesising one from the codec would be a lie.
  const auto info = player->player->media_info();
  return CopyOut(info ? info->uri : std::string(), out, capacity);
}

extern "C" int32_t avbase_player_dump_diagnostics(const AvbasePlayer* player,
                                                  char* out,
                                                  int32_t capacity) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return CopyOut(player->player->DumpDiagnostics(), out, capacity);
}

// ---------------------------------------------------------------------------
// Media info
// ---------------------------------------------------------------------------

extern "C" AvbaseStatus avbase_player_get_media_info(const AvbasePlayer* player,
                                                     AvbaseMediaInfo* out) {
  if (player == nullptr || player->player == nullptr || out == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  const auto info = player->player->media_info();
  if (!info) {
    // Before prepare there is genuinely no answer, and zeros with AVBASE_OK
    // would be the classic lie.
    return AVBASE_ERR_INVALID_STATE;
  }
  out->duration_ms = ToMs(info->duration);
  out->bitrate = info->bit_rate;
  out->is_live = info->is_live ? 1 : 0;
  out->seekable = info->seekable ? 1 : 0;
  out->video_width = 0;
  out->video_height = 0;
  out->video_rotation = 0;
  out->video_track_count = 0;
  out->audio_track_count = 0;
  out->subtitle_track_count = 0;
  // Size and rotation live on the stream, not the container. The FIRST video
  // stream wins, which is the same rule MediaInfo::FirstStreamOfKind applies.
  for (const auto& stream : info->streams) {
    switch (stream.kind) {
      case avbase::StreamKind::kVideo:
        ++out->video_track_count;
        if (out->video_width == 0 && out->video_height == 0) {
          out->video_width = stream.natural_size.width;
          out->video_height = stream.natural_size.height;
          out->video_rotation = stream.rotation;
        }
        break;
      case avbase::StreamKind::kAudio:
        ++out->audio_track_count;
        break;
      case avbase::StreamKind::kText:
        ++out->subtitle_track_count;
        break;
      case avbase::StreamKind::kUnknown:
        break;
    }
  }
  return AVBASE_OK;
}

// ---------------------------------------------------------------------------
// Multi-source composition
// ---------------------------------------------------------------------------

extern "C" AvbaseSource* avbase_player_add_source(AvbasePlayer* player,
                                                  const char* uri,
                                                  const char* name,
                                                  int32_t z_order) {
  if (player == nullptr || player->player == nullptr || uri == nullptr) {
    return nullptr;
  }
  avbase::DataSourceDescriptor descriptor;
  descriptor.uri = std::string(uri);
  auto source = player->player->AddSource(
      descriptor, name != nullptr ? name : "", z_order);
  if (source == nullptr) {
    return nullptr;
  }
  std::unique_ptr<AvbaseSource> handle(new (std::nothrow) AvbaseSource());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->source = std::move(source);
  return handle.release();
}

extern "C" void avbase_source_release(AvbaseSource* source) {
  // Null is a no-op. This only drops this reference, matching the C++
  // shared_ptr semantics -- it does not unregister the source.
  delete source;
}

extern "C" AvbaseStatus avbase_source_set_uri(AvbaseSource* source,
                                              const char* uri) {
  if (source == nullptr || source->source == nullptr || uri == nullptr) {
    return AVBASE_ERR_INVALID_ARGUMENT;
  }
  // SceneSource has no mutating URI in the C++ API today. NOT_IMPLEMENTED is
  // the honest answer; inventing one here would create behaviour with no C++
  // counterpart, and a divergence that only shows up on the C side.
  return AVBASE_ERR_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------
