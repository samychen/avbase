# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""The legacy-option map: every ijkplayer option key and the PlayerConfig field
it lands on.

Data for tools/gen_options.py, kept in its own module for the same reason as
tools/ported_constants.py: this table is what grows when a milestone adds
config surface, and "can an existing ijkplayer configuration be migrated?"
should be answerable by reading one file.

Authority and drift
-------------------
docs/05 table 4 is the specification (its header says 已核对原文 -- checked
against ff_ffplay_options.h). player/public/player_config.h is the code. This
table is the mapping between them, and tools/gen_options.py checks all three
against each other, so a key added to the doc without a field, or a field
renamed without updating the key, fails rather than silently shipping. That is
acceptance criterion A10 ("every legacy option has an equivalent entry point"),
which at the ninth round stood at 9 keys implemented out of this table's 63.

Kinds
-----
A kind is how a scalar option value becomes a field assignment. They exist
because the migration is not always an identity: three keys are negated
(`an`/`vn`/`skip-calc-frame-rate` set a *disabled* flag), `volume` changes
range (Δ6, 0..100 -> 0.0..1.0), and four keys carry milliseconds into a
base::TimeDelta field. Encoding that per key is what makes the emitted C++
mechanical instead of clever.
"""

from __future__ import annotations

# --- kinds -----------------------------------------------------------------
KIND_BOOL = "bool"                    # field = (value != 0)
KIND_BOOL_NEGATED = "bool_negated"    # field = (value == 0); an / vn / skip-calc-*
KIND_INT = "int"                      # field = value, range checked
KIND_INT_SIZE = "int_size"            # size_t field, rejects negatives
KIND_INT64 = "int64"                  # int64_t field
KIND_LOOP_COUNT = "loop_count"        # INT_MIN means infinite (-1), per ffplay
KIND_MS_TO_TIMEDELTA = "ms_to_timedelta"
KIND_VOLUME_PERCENT = "volume_percent"          # Δ6: 0..100 -> 0.0..1.0
KIND_DOUBLE = "double"
KIND_STRING = "string"
KIND_STRING_MAP = "string_map"      # "Name: v\\r\\n..." blob -> std::map
KIND_ENUM = "enum"                    # static_cast<Enum>(value), named values
KIND_BITMASK = "bitmask"              # field |= bit
KIND_ENUM_SELECT = "enum_select"      # nonzero picks an enumerator (mediacodec)
KIND_MASK_ALL = "mask_all"            # nonzero sets every bit (all-videos)

PLAYER = "kPlayer"
FORMAT = "kFormat"
CODEC = "kCodec"

# Value types as the SDK sees them.
T_BOOL = "kBool"
T_INT = "kInt"
T_INT64 = "kInt64"
T_DOUBLE = "kDouble"
T_STRING = "kString"
T_ENUM = "kEnum"


class Opt:
    """One legacy option key."""

    def __init__(self, key, field, kind, vtype, lo=None, hi=None, *,
                 category=PLAYER, default=None, doc="", deviation="",
                 enum=None, bit=None, proposed=False):
        # lo/hi are positional because almost every entry spells them that way
        # (Opt("framedrop", ..., KIND_INT, T_INT, -1, 120, default=0)); category
        # is keyword-only so that unpacking a range can never land on it. The
        # first version had category fourth and `*BOOL01` silently became the
        # category, which is the kind of mistake a positional-or-keyword
        # parameter list makes easy and a keyword-only one makes impossible.
        self.key = key
        self.field = field          # PlayerConfig path, e.g. "buffer.max_bytes"
        self.kind = kind
        self.vtype = vtype
        self.category = category
        self.lo = lo
        self.hi = hi
        self.default = default
        self.doc = doc
        self.deviation = deviation  # "Δ6" when behaviour intentionally differs
        self.enum = enum            # {int: enumerator} for KIND_ENUM*
        self.bit = bit              # bit value for KIND_BITMASK
        self.proposed = proposed    # True => name not yet ratified, do not emit


def fcc(chars: str) -> int:
    """Builds a FourCC the way ijksdl_fourcc.h does:

        #define SDL_FOURCC(a, b, c, d) \
            ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
             ((uint32_t)(d) << 24))

    Derived from the four characters rather than hand-typed in hex, because the
    hex was hand-typed once already and one of the six values came out wrong
    (kNative was 0x32565220, which spells ' RV2', instead of 0x3273655f =
    '_es2'). The character spelling is what docs/05 §4.2 lists, so deriving from
    it makes the table agree with the document by construction instead of by
    luck. This is the same mistake risk R1 mitigation 4 exists to prevent,
    recurring in the tool that was supposed to prevent it.
    """
    assert len(chars) == 4, chars
    return (ord(chars[0]) | (ord(chars[1]) << 8) | (ord(chars[2]) << 16)
            | (ord(chars[3]) << 24))


BOOL01 = (0, 1)

# --- docs/05 §4.1  PLAYER category (36 keys) -------------------------------
OPTIONS: list[Opt] = [
    Opt("an", "audio.disabled", KIND_BOOL_NEGATED, T_BOOL, *BOOL01, default=0,
        doc="disable audio"),
    Opt("vn", "video.disabled", KIND_BOOL_NEGATED, T_BOOL, *BOOL01, default=0,
        doc="disable video"),
    Opt("nodisp", "render.disable_video_output", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="disable graphical display"),
    Opt("volume", "audio.startup_volume", KIND_VOLUME_PERCENT, T_INT, 0, 100,
        default=100, doc="startup volume, 0..100", deviation="Δ6"),
    Opt("fast", "fast", KIND_BOOL, T_BOOL, *BOOL01, default=0,
        doc="non spec compliant style (fewer checks)"),
    Opt("loop", "loop_count", KIND_LOOP_COUNT, T_INT, default=1,
        doc="number of times to play; INT_MIN means infinite"),
    Opt("infbuf", "buffer.unlimited", KIND_BOOL, T_BOOL, *BOOL01, default=0,
        doc="do not limit the input buffer size (realtime streams)"),
    Opt("framedrop", "video.max_frame_drop", KIND_INT, T_INT, -1, 120,
        default=0, doc="drop frames when the CPU is too slow"),
    Opt("seek-at-start", "seek.seek_at_start", KIND_MS_TO_TIMEDELTA, T_INT64,
        default=0, doc="start position offset in milliseconds"),
    Opt("subtitle", "subtitle.enabled", KIND_BOOL, T_BOOL, *BOOL01, default=0,
        doc="enable subtitle"),
    Opt("af", "audio.filter_graph", KIND_STRING, T_STRING,
        doc="audio filter graph description"),
    Opt("vf0", "video.filter_graph", KIND_STRING, T_STRING,
        doc="video filter graph description for chain 0"),
    Opt("rdftspeed", "rdft_speed", KIND_INT, T_INT, lo=0, default=0,
        doc="rdft speed, in milliseconds"),
    Opt("find_stream_info", "demux.find_stream_info", KIND_BOOL, T_BOOL,
        *BOOL01, default=1, doc="call avformat_find_stream_info"),
    Opt("max-fps", "video.max_fps", KIND_INT, T_INT, -1, 121, default=31,
        doc="drop frames from video whose fps is greater than max-fps"),
    Opt("overlay-format", "video.overlay_format", KIND_ENUM, T_ENUM,
        default="kRgb32", doc="output overlay pixel format (FourCC in legacy)",
        # docs/05 §4.2, in the same order: SDL_FCC__GLES2 / I420 / YV12 /
        # RV16 / RV24 / RV32. kNV12 and kP010 are avbase additions with no
        # legacy FourCC, so they are reachable only through the typed field.
        enum={fcc("_es2"): "kNative", fcc("I420"): "kI420",
              fcc("YV12"): "kYv12", fcc("RV16"): "kRgb16",
              fcc("RV24"): "kRgb24", fcc("RV32"): "kRgb32"}),
    Opt("start-on-prepared", "start_on_prepared", KIND_BOOL, T_BOOL, *BOOL01,
        default=1, doc="automatically start playing on prepared"),
    Opt("video-pictq-size", "video.frame_queue_size", KIND_INT, T_INT, 2, 16,
        default=3, doc="video picture queue size"),
    Opt("max-buffer-size", "buffer.max_bytes", KIND_INT_SIZE, T_INT64, lo=0,
        default=15 * 1024 * 1024, doc="max buffer size to pre-read, in bytes"),
    Opt("min-frames", "buffer.min_frames", KIND_INT_SIZE, T_INT64, 2, 50,
        default=5, doc="minimal frames to stop pre-reading"),
    Opt("first-high-water-mark-ms", "buffer.first_high_water_mark",
        KIND_MS_TO_TIMEDELTA, T_INT64, lo=0, default=100,
        doc="first buffering high water mark, in milliseconds"),
    Opt("next-high-water-mark-ms", "buffer.next_high_water_mark",
        KIND_MS_TO_TIMEDELTA, T_INT64, lo=0, default=1000,
        doc="second buffering high water mark, in milliseconds"),
    Opt("last-high-water-mark-ms", "buffer.last_high_water_mark",
        KIND_MS_TO_TIMEDELTA, T_INT64, lo=0, default=5000,
        doc="last buffering high water mark, in milliseconds"),
    Opt("packet-buffering", "buffer.enabled", KIND_BOOL, T_BOOL, *BOOL01,
        default=1, doc="pause output until enough packets are buffered"),
    Opt("sync-av-start", "buffer.sync_av_start", KIND_BOOL, T_BOOL, *BOOL01,
        default=1, doc="align audio and video at playback start"),
    Opt("iformat", "demux.forced_format", KIND_STRING, T_STRING,
        doc="force the input format name"),
    Opt("no-time-adjust", "no_time_adjust", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="disable the timestamp adjustment for live streams"),
    Opt("preset-5-1-center-mix-level", "audio.preset_5_1_center_mix_level",
        KIND_DOUBLE, T_DOUBLE, -32.0, 32.0, default=0.70710678,
        doc="5.1 channel center mix level"),
    Opt("enable-accurate-seek", "seek.accurate", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="decode from the previous keyframe and drop to target",
        deviation="Δ19"),
    Opt("accurate-seek-timeout", "seek.accurate_timeout",
        KIND_MS_TO_TIMEDELTA, T_INT64, lo=0, default=5000,
        doc="accurate seek timeout, in milliseconds"),
    Opt("skip-calc-frame-rate", "video.calc_frame_rate", KIND_BOOL_NEGATED,
        T_BOOL, *BOOL01, default=0, doc="do not calculate the frame rate"),
    Opt("get-frame-mode", "get_frame_mode", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="decode to frames without rendering (thumbnailing)"),
    Opt("async-init-decoder", "async_init_decoder", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="initialise the decoder asynchronously"),
    Opt("video-mime-type", "video.forced_mime_type", KIND_STRING, T_STRING,
        doc="force the video MIME type for codec selection"),
    Opt("ijkmeta-delay-init", "demux.delay_meta_init", KIND_BOOL, T_BOOL,
        *BOOL01, default=0, doc="delay stream metadata until first start"),
    Opt("render-wait-start", "render.render_wait_start", KIND_BOOL, T_BOOL,
        *BOOL01, default=0, doc="render the first frame only after Start()"),
]

# --- docs/05 §4.3  Android-specific (12 keys) ------------------------------
OPTIONS += [
    Opt("mediacodec", "video.decoder_preference", KIND_ENUM_SELECT, T_BOOL,
        *BOOL01, default=0, doc="prefer hardware decoding (MediaCodec)",
        enum={1: "kHardwareFirst"}),
    Opt("mediacodec-avc", "video.hw_codecs", KIND_BITMASK, T_BOOL, *BOOL01,
        default=0, doc="enable MediaCodec for H.264/AVC", bit=1 << 0),
    Opt("mediacodec-hevc", "video.hw_codecs", KIND_BITMASK, T_BOOL, *BOOL01,
        default=0, doc="enable MediaCodec for H.265/HEVC", bit=1 << 1),
    Opt("mediacodec-mpeg2", "video.hw_codecs", KIND_BITMASK, T_BOOL, *BOOL01,
        default=0, doc="enable MediaCodec for MPEG-2", bit=1 << 2),
    Opt("mediacodec-mpeg4", "video.hw_codecs", KIND_BITMASK, T_BOOL, *BOOL01,
        default=0, doc="enable MediaCodec for MPEG-4", bit=1 << 3),
    Opt("mediacodec-all-videos", "video.hw_codecs", KIND_MASK_ALL, T_BOOL,
        *BOOL01, default=0, doc="enable MediaCodec for every video codec"),
    Opt("mediacodec-auto-rotate", "video.auto_rotate", KIND_BOOL, T_BOOL,
        *BOOL01, default=0, doc="let the codec apply the rotation matrix"),
    Opt("mediacodec-handle-resolution-change",
        "video.handle_resolution_change", KIND_BOOL, T_BOOL, *BOOL01,
        default=1, doc="handle mid-stream resolution changes", deviation="Δ3"),
    Opt("mediacodec-sync", "video.hw_sync_mode", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="use synchronous MediaCodec decoding"),
    Opt("mediacodec-default-name", "video.hw_decoder_name", KIND_STRING,
        T_STRING, doc="preferred MediaCodec component name"),
    Opt("opensles", "audio.backend", KIND_ENUM_SELECT, T_BOOL, *BOOL01,
        default=0, doc="use OpenSL ES for audio output",
        enum={1: "kOpenSLES"}),
    Opt("soundtouch", "audio.tempo_stretch", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="time-stretch audio when the rate is not 1.0",
        deviation="Δ17"),
]

# --- docs/05 §4.4  iOS-specific (5 keys) -----------------------------------
OPTIONS += [
    Opt("videotoolbox", "video.decoder_preference", KIND_ENUM_SELECT, T_BOOL,
        *BOOL01, default=0, doc="prefer hardware decoding (VideoToolbox)",
        enum={1: "kHardwareFirst"}),
    Opt("videotoolbox-max-frame-width", "video.hw_max_frame_width", KIND_INT,
        T_INT, lo=0, default=0,
        doc="max frame width for VideoToolbox; 0 means unlimited"),
    Opt("videotoolbox-async", "video.hw_async", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, doc="use asynchronous VideoToolbox decoding"),
    Opt("videotoolbox-wait-async", "video.hw_wait_async", KIND_BOOL, T_BOOL,
        *BOOL01, default=1, doc="wait for asynchronous VideoToolbox frames"),
    Opt("videotoolbox-handle-resolution-change",
        "video.handle_resolution_change", KIND_BOOL, T_BOOL, *BOOL01, default=1,
        doc="handle mid-stream resolution changes", deviation="Δ3"),
]

# --- docs/05 §4.7  restored from FFP_MERGE (4 keys) ------------------------
OPTIONS += [
    Opt("ast", "audio.selected_stream", KIND_INT, T_INT, lo=-1, default=-1,
        doc="select the audio stream by index; -1 means automatic"),
    Opt("vst", "video.selected_stream", KIND_INT, T_INT, lo=-1, default=-1,
        doc="select the video stream by index; -1 means automatic"),
    Opt("sst", "subtitle.selected_stream", KIND_INT, T_INT, lo=-1, default=-1,
        doc="select the subtitle stream by index; -1 means automatic"),
    Opt("sync", "sync_master", KIND_ENUM, T_ENUM, default="kAudio",
        doc="which clock is authoritative",
        enum={0: "kAudio", 1: "kVideo", 2: "kExternal"}),
]

# --- docs/05 §4.6  FORMAT/CODEC keys that map to a typed field (6 keys) ----
# The rest of that table is verbatim passthrough into extra_format_options /
# extra_codec_options / extra_scaler_options, which OptionRegistry::SetString
# already handles for any non-kPlayer category, so those need no entry here.
OPTIONS += [
    Opt("probesize", "demux.probe_size", KIND_INT64, T_INT64, lo=0,
        default=5 * 1024 * 1024, category=FORMAT,
        doc="bytes to probe when identifying streams"),
    Opt("analyzeduration", "demux.analyze_duration", KIND_MS_TO_TIMEDELTA,
        T_INT64, lo=0, default=5000, category=FORMAT,
        doc="microseconds in legacy; avbase takes milliseconds"),
    Opt("timeout", "demux.timeout", KIND_MS_TO_TIMEDELTA, T_INT64, lo=0,
        default=0, category=FORMAT, doc="socket I/O timeout in milliseconds"),
    Opt("dns_cache_clear", "net.dns_cache_clear", KIND_BOOL, T_BOOL, *BOOL01,
        default=0, category=FORMAT, doc="clear the DNS cache before resolving"),
    Opt("user-agent", "net.user_agent", KIND_STRING, T_STRING, category=FORMAT,
        doc="HTTP User-Agent header"),
    Opt("skip_loop_filter", "video.skip_loop_filter", KIND_INT, T_INT, lo=0,
        default=0, category=CODEC, doc="AVDiscard value for the loop filter"),
    Opt("headers", "net.headers", KIND_STRING_MAP, T_STRING, category=FORMAT,
        doc="extra HTTP headers, one \"Name: value\" per line"),
    Opt("reconnect", "net.reconnect", KIND_BOOL, T_BOOL, *BOOL01, default=1,
        category=FORMAT, doc="reconnect automatically after a network error"),
    Opt("reconnect_delay_max", "net.reconnect_delay", KIND_MS_TO_TIMEDELTA,
        T_INT64, lo=0, default=100, category=FORMAT,
        doc="maximum delay between reconnect attempts, in milliseconds"),
    # docs/05 §4.6 also lists `reconnect_streamed` and `stimeout`/`rw_timeout`.
    # Those have no dedicated PlayerConfig field of their own: `stimeout` and
    # `rw_timeout` are legacy aliases of `timeout` (already mapped above), and
    # `reconnect_streamed` has no field at all. Adding a field is a config
    # decision, not a generator decision, so they are recorded here rather than
    # silently mapped onto a neighbouring field.
]

# --- docs/05 §4.5  Linux-specific, new keys (6) ----------------------------
# These have NO legacy name, so their key strings are an API decision that has
# not been ratified. They are listed so the table is complete against docs/05,
# and marked proposed=True so --emit skips them and the report shows them as
# awaiting a naming decision at M8 rather than shipping a guessed spelling.
OPTIONS += [
    Opt("render.linux_backend", "render.linux_backend", KIND_ENUM, T_ENUM,
        default="kAuto", doc="video backend selection", proposed=True),
    Opt("render.linux_audio", "render.linux_audio", KIND_ENUM, T_ENUM,
        default="kAuto", doc="audio backend selection", proposed=True),
    Opt("render.prefer_dmabuf_zero_copy", "render.prefer_dmabuf_zero_copy",
        KIND_BOOL, T_BOOL, *BOOL01, default=1,
        doc="prefer the dmabuf zero-copy upload path", proposed=True),
    Opt("render.use_present_extension_for_vsync",
        "render.use_present_extension_for_vsync", KIND_BOOL, T_BOOL, *BOOL01,
        default=1, doc="use the X11 Present extension for vsync", proposed=True),
    Opt("render.gl_context_sharing", "render.gl_context_sharing", KIND_BOOL,
        T_BOOL, *BOOL01, default=0,
        doc="share the host GL context (embedded mode)", proposed=True),
    Opt("audio.output_device_id", "audio.output_device_id", KIND_STRING,
        T_STRING, doc="output device id for SwitchableAudioRendererSink",
        proposed=True),
]


def by_key() -> dict:
    return {o.key: o for o in OPTIONS}


def emittable() -> list:
    return [o for o in OPTIONS if not o.proposed]
