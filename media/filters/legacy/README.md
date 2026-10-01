# `media/filters/legacy/` — LGPL-2.1 quarantine

Everything in this directory is a **line-by-line port of an algorithm from
ijkplayer's `ff_ffplay.c`**, and is therefore a derivative work of ijkplayer
(LGPL-2.1). The rest of avbase is BSD-3-Clause. See the root `LICENSE` §1 and
decision **D10** / risk **R8** in `docs/08` §4 and §5.

Full license text: [`LICENSE.LGPL-2.1`](LICENSE.LGPL-2.1).

## What is in here

| File | Ported from `ff_ffplay.c` | Original authors |
|---|---|---|
| `video_frame_compositor.{h,cc}` | `video_refresh()`, `compute_target_delay()` | Zhang Rui / Bilibili; Fabrice Bellard (ffplay.c) |
| `av_sync_controller.{h,cc}` | `get_master_sync_type()`, `get_master_clock()`, `synchronize_audio()`, `synchronize_audio_to_video()` | idem |
| `clock.{h,cc}` | `struct Clock`, `get_clock()`, `set_clock()`, `set_clock_at()` | idem |

Each file's header comment names the exact functions it ports, under
`ALGORITHM PROVENANCE`. The **thresholds are inherited, not improved**: they
must match the original values, which is what `tools/extract_constants.py`
verifies (docs/05 table 7, docs/01 §6). Do not "optimise" a magic number here
without golden-test evidence (docs/07 §7).

## What is *not* a reason to put a file here

The quarantine is by **provenance of the algorithm**, not by age, difficulty or
convenience:

* ❌ A file that merely *includes* one of these headers stays under BSD-3 where
  it lives. `media/filters/decoder_stream.cc` consumes the compositor; it is not
  a port.
* ❌ A file whose *structure* is inspired by Chromium's `media/` (BSD-3) but
  whose code is original stays BSD-3. That is most of `media/base/`.
* ❌ "This was hard to write, let's be safe" — over-inclusion shrinks what
  downstream integrators get under BSD-3 and makes the boundary meaningless.
* ✅ A file belongs here **iff** a reviewer could point at a function in
  `ff_ffplay.c` and say "this is that function, transliterated".

Structure, naming, threading model and tests in these files are original avbase
work (the compositor's decision logic is a pure static function precisely so it
can be exhaustively unit tested, which `video_refresh()` cannot be — docs/03
§8.1). Original authorship of the *shape* does not change the license of the
*algorithm*, so the files stay here.

## Obligations this creates for integrators

* **Dynamic linking** (`libavbase.so`, the default per Q10): you get LGPL-2.1
  §6(b)'s "suitable shared library mechanism" path — no extra obligation beyond
  keeping this notice and the license text with the distribution.
* **Static linking**: LGPL-2.1 §6(a) requires you to provide the object files
  or the complete corresponding source, so that a user can relink with a
  modified version of these six files.
* Redistribution must include this directory's `LICENSE.LGPL-2.1` and the
  copyright notices. They are already in every file header.
* avbase also links the **system FFmpeg**, whose own license (LGPL-2.1+ or
  GPL-2+, depending on your distribution's build) further bounds what you may
  distribute. See root `LICENSE` §3.

## If the project ever has to be pure BSD-3

docs/08 §5 R8 records the contingency: the algorithms would have to be
**re-derived independently** from the published behaviour rather than ported,
estimated at +2 weeks, and validated by the same golden tests (docs/07 §7) that
currently prove the ports are faithful. Until that happens, this directory is
the license boundary.
