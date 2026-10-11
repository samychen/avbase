# glad (vendored)

Generated OpenGL loader. **Nothing here was written by this project and nothing
here is edited by hand** — every file other than this README is output of the
glad 2.0.8 generator.

## Why it is here

`render/gl/` used to carry a hand-written entry-point table (`gl_api.h`, 73
rows) and a hand-written enumerant table (`gl_constants.h`, 116 entries).
Writing the second one from scratch produced three silent value errors in a
single sitting, and one of them shipped: `GL_VERTEX_SHADER` spelled `0x8B33`
made `glCreateShader()` return 0 with an *empty* info log, so the render path
looked like a broken driver while the card was fine. The first answer was a
machine that checked the handwritten table against the platform headers
(`tools/check_gl_constants.py`, retired once this landed). The better answer is
not to hand-write the table at all: glad derives enumeration values, prototypes
and the loader from Khronos' own XML, and it is what the rest of the industry
uses (OBS among them).

The other half of the motivation is size. glad's table is all of GL 3.3 core,
not the 48 functions this module happened to need, so "add a function" stopped
being a thing anyone has to remember to do.

## Regenerating

glad is a generator, not a library: the output is checked in so that building
this project needs no network, no Python and no glad installation.

```sh
pip install glad2==2.0.8
python -m glad --api gl:core=3.3 \
  --extensions "GL_EXT_texture_sRGB_decode,GL_ARB_copy_image,GL_NV_copy_image,\
GL_EXT_texture_filter_anisotropic,GL_KHR_debug,GL_ARB_debug_output,\
GL_EXT_EGL_image_storage" \
  --out-path render/gl/third_party/glad --quiet c --mx
```

Every option is load-bearing:

| Option | Why |
| --- | --- |
| `gl:core=3.3` | the version this module targets, core profile — no deprecated entry points to resolve |
| `--mx` | one `GladGLContext` per GL context, which is what `GlApi` already was |
| *no* `--loader` | symbol resolution stays with `NativeGLContext::GetProcAddress`; glad's built-in loaders would add dlopen/WGL paths this module deliberately does not have |
| *no* `--alias` | it silently pulls in every extension that aliases a core 3.3 function: the 7 requested extensions became **75**. Without it the table is the seven named above and nothing else, and `CopyImageSubData` / `CopyImageSubDataNV` stay separately detectable, which is what `has_copy_image()` asks |

`glEGLImageTargetTexture2DOES` is **not** in that list and cannot be: it belongs
to `GL_OES_EGL_image`, an ES extension, and glad rejects it outright for a
desktop `gl` API. `GlApi` resolves that single symbol itself, through the same
`NativeGLContext` — see the comment in `gl_api.h`.

The header carries a generation timestamp, so a regeneration never produces an
empty diff. Review the diff for the API/extension set, not for cleanliness.

## Layout

| Path | What |
| --- | --- |
| `include/glad/gl.h` | enumerants, GL types, `GladGLContext`, `gladLoadGLContext*` |
| `include/KHR/khrplatform.h` | the fixed-width types glad's header expects; it includes this as `<KHR/khrplatform.h>`, which is why `include/` has to be on the search path |
| `src/gl.c` | the loader itself |

## Build treatment

Compiled into the `avbase_gl_vendor` object library (see
`render/CMakeLists.txt`) so that the project's warning flags — which exist to
keep *our* code honest — do not apply to generated output. `third_party/` is
also skipped by `tools/check_invariants.py` and by CI's format sweep, for the
same reason.

## License

glad's output is `(WTFPL OR CC0-1.0) AND Apache-2.0`, stated on the first line
of each generated file. Both are permissive and compatible with this project's
BSD-style license. The files are kept verbatim so that SPDX line stays the
authoritative statement; the Apache-2.0 half covers the Khronos registry data
the generator reads, which is why the notice has to survive rather than be
folded into this project's own header.
