// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: DRAFT — NOT YET IN THE BUILD, for the same reason as the header it
// implements (media/base/pipeline_status.h): neither has been compiled in the
// environment that wrote them. To finish bringing this pair in, add the .cc to
// media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/pipeline_status.h"

#include <string>

namespace ijkpp::media {

const char* PipelineStatusToString(PipelineStatus status) {
  // Exhaustive on purpose: no `default`, so adding an enumerator to the header
  // makes this a -Wswitch error under the debug preset's -Werror rather than a
  // silent "invalid" at runtime. The same shape as GetWaitingReasonName().
  switch (status) {
    case PipelineStatus::kOk: return "ok";
    case PipelineStatus::kDemuxerError: return "demuxer-error";
    case PipelineStatus::kDemuxerInitializationError:
      return "demuxer-initialization-error";
    case PipelineStatus::kMissingDemuxerStreams:
      return "missing-demuxer-streams";
    case PipelineStatus::kAudioInitializationError:
      return "audio-initialization-error";
    case PipelineStatus::kVideoInitializationError:
      return "video-initialization-error";
    case PipelineStatus::kDecoderError: return "decoder-error";
    case PipelineStatus::kVideoDecoderDoesNotSupportHardwareProtection:
      return "video-decoder-no-hardware-protection";
    case PipelineStatus::kRendererError: return "renderer-error";
    case PipelineStatus::kAudioRendererInitializationError:
      return "audio-renderer-initialization-error";
    case PipelineStatus::kVideoRendererInitializationError:
      return "video-renderer-initialization-error";
    case PipelineStatus::kInitializationError: return "initialization-error";
    case PipelineStatus::kAborted: return "aborted";
    case PipelineStatus::kFailedToCreatePipeline:
      return "failed-to-create-pipeline";
    // kMaxValue aliases kFailedToCreatePipeline, so it has no case of its own
    // (same convention as DemuxerStreamType::kMaxValue in media_types.cc).
  }
  return "invalid";
}

MediaError PipelineStatusToMediaError(PipelineStatus status) {
  // TOTAL MAPPING, per the rule in pipeline_status.h: no status may fall
  // through to a generic "playback failed". docs/10 §4 exists because
  // "internal error" is not actionable, and a fall-through default here would
  // reintroduce exactly that.
  //
  // LIMITATION worth stating: this function receives only a status, so the
  // `detail` below cannot contain the runtime values docs/10 §4.2 demands (uri,
  // codec, resolution, native code). Callers that have them -- PipelineImpl,
  // and the sub-renderers through MediaLog -- must re-wrap with MediaError's
  // `context` argument and append the real values before reporting upward. What
  // is guaranteed here is that the summary names the failing stage and the
  // suggestion names an API, a config field or a command.
  switch (status) {
    case PipelineStatus::kOk:
      return MediaError::Ok();

    case PipelineStatus::kDemuxerInitializationError:
      return MediaError(
          ErrorCode::kSourceOpenFailed, "cannot open the media source",
          "the demuxer failed while opening or probing the container",
          "check that the path or URL is reachable and that the container is "
          "one FFmpeg can parse; for an unusual container set "
          "config.demux.forced_format, and for a slow source raise "
          "config.demux.probe_size");

    case PipelineStatus::kDemuxerError:
      return MediaError(
          ErrorCode::kSourceReadFailed, "cannot read from the media source",
          "the demuxer failed after the container was opened",
          "for a network source check connectivity and "
          "config.net.reconnect*; for a local file check that it is not "
          "truncated (ffprobe \"<uri>\")");

    case PipelineStatus::kMissingDemuxerStreams:
      return MediaError(
          ErrorCode::kStreamNotFound, "the source has no playable stream",
          "the container opened but contained no audio or video stream this "
          "renderer can consume",
          "a subtitle-only or data-only file cannot be played by this "
          "renderer; check MediaInfo::streams after PrepareAsync() to see "
          "what the container actually holds");

    case PipelineStatus::kAudioInitializationError:
      return MediaError(
          ErrorCode::kDecoderOpenFailed, "cannot initialise audio decoding",
          "no audio decoder accepted the stream's configuration",
          "set config.audio.disabled = true to play video only, or check "
          "that your FFmpeg build includes this codec "
          "(ffmpeg -decoders | grep <codec>)");

    case PipelineStatus::kVideoInitializationError:
      return MediaError(
          ErrorCode::kDecoderOpenFailed, "cannot initialise video decoding",
          "no video decoder accepted the stream's configuration",
          "set config.video.decoder_preference = kSoftware to rule out a "
          "hardware decoder, or config.video.disabled = true to play audio "
          "only; a kDecoderFallback event names the decoder that declined");

    case PipelineStatus::kDecoderError:
      return MediaError(
          ErrorCode::kDecodeFailed, "decoding failed after startup",
          "a decoder that initialised successfully returned a fatal error",
          "the stream may be corrupt or may change format mid-play; retry, "
          "and if it reproduces, capture Player::DumpDiagnostics() and the "
          "output of ijkpp-inspect decode \"<uri>\"");

    case PipelineStatus::kVideoDecoderDoesNotSupportHardwareProtection:
      return MediaError(
          ErrorCode::kDecoderUnsupportedCodec,
          "the video decoder cannot protect this content",
          "the stream requires hardware-protected decoding and the selected "
          "decoder does not provide it",
          "DRM is not implemented in this build (decision D8); play an "
          "unprotected source instead");

    case PipelineStatus::kAudioRendererInitializationError:
      return MediaError(
          ErrorCode::kSinkConfigureFailed, "cannot open the audio output",
          "the audio renderer or its sink rejected the stream's parameters",
          "set config.audio.backend to a specific backend (kAlsa, kPulse, "
          "kSdl2) to rule out auto-detection, or config.audio.disabled = "
          "true to play video only; ijkpp-inspect doctor reports which audio "
          "services are reachable");

    case PipelineStatus::kVideoRendererInitializationError:
      return MediaError(
          ErrorCode::kSinkConfigureFailed, "cannot open the video output",
          "the video renderer or its sink rejected the stream's format",
          "if no display is available use config.render."
          "disable_video_output = true for audio-only playback; otherwise "
          "check that the NativeDisplay passed to SetVideoSurface() is still "
          "valid and that config.render.linux_backend is not pinned to a "
          "backend this system lacks");

    case PipelineStatus::kRendererError:
      // ErrorCode has no renderer-specific value, so this is the least-bad fit
      // rather than a good one. Extending ErrorCode is additive and therefore
      // not a breaking change to a frozen enum; doing so is the right fix and
      // is recorded as a finding in docs/PROGRESS.md rather than being quietly
      // papered over by a worse message here.
      return MediaError(
          ErrorCode::kInvalidState, "the renderer failed during playback",
          "a renderer reported a fatal error that is not attributable to the "
          "demuxer, a decoder or a sink",
          "capture Player::DumpDiagnostics() and file it with the media "
          "sample; this path should be unreachable and reaching it is a bug");

    case PipelineStatus::kInitializationError:
      return MediaError(
          ErrorCode::kInvalidState, "playback failed to start",
          "a pipeline stage failed without naming itself",
          "capture Player::DumpDiagnostics(); the stage list in "
          "PlaybackStats::stages shows how far startup got");

    case PipelineStatus::kAborted:
      return MediaError(
          ErrorCode::kAborted, "playback was stopped during startup",
          "Stop() or destruction arrived while the pipeline was still "
          "initialising",
          "this is not an error in the media: it is the expected result of "
          "cancelling a start. Ignore it unless you did not call Stop()");

    case PipelineStatus::kFailedToCreatePipeline:
      return MediaError(
          ErrorCode::kConfigInvalid, "cannot build a renderer for this setup",
          "the renderer factory declined every renderer type it was offered, "
          "or could not provide a sink",
          "check player::Deps: video_decoder_factories and "
          "audio_decoder_factories must be non-empty or left null for "
          "auto-detection, and a custom video_sink_factory must return a "
          "working sink for the NativeDisplay in use");
  }
  // Unreachable for every declared enumerator. Kept because a caller can cast
  // an arbitrary integer into the enum, and returning a default-constructed
  // MediaError (which reports kOk) would turn a failure into a success.
  return MediaError(
      ErrorCode::kInvalidArgument, "unknown pipeline status",
      "PipelineStatusToMediaError received a value outside the declared range",
      "this is a programming error in the caller; report the value from "
      "PipelineStatusToString()");
}

}  // namespace ijkpp::media
