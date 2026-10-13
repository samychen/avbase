// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_PRESENT_H_
#define AVBASE_MEDIA_GL_GL_PRESENT_H_

#include <memory>

#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_hw_texture_importer.h"
#include "media/gl/gl_scene.h"
#include "media/gl/gl_video_layer.h"
#include "media/media_export.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_blitter.h"
#include "render/gl/gl_framebuffer.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"
#include "render/gl/gl_texture.h"
#include "render/gl/gl_transition.h"
#include "render/gl/native_gl_context.h"
#include "render/gl/overlay.h"

namespace avbase::media {

// Uploads a planar YUV frame into three textures and draws it through a
// YUV->RGB program, optionally compositing a host-published overlay, into an
// off-screen canvas; then blits that canvas onto the window.
//
// It draws one of two things, and they share everything after the compose:
//
//   * a single frame -- Compose()/Present() -- the common one-source case;
//   * a GlScene of N ordered layers -- ComposeScene()/PresentScene() -- and a
//     cross-fade between two of them --
//     ComposeTransition()/PresentTransition().
//
// The canvas, the overlay pass, the blit and the swap are the same in all of
// them. That is why the composition surface lives here rather than inside a
// scene: how a frame reaches the window is one question with one answer, and
// only what fills the frame differs.
//
// This owns the frame: the composition surface, the colour-uniform program,
// the overlay pass, and the two-stage canvas -> window hand-off. The video
// itself -- plane splitting, strides, colour matrices, the NV12/YV12/I420
// differences -- lives on GlVideoLayer, which is what lets a scene hold
// several sources through the same code path. Everything that is neither
// video nor composition -- context abstraction, entry-point loading, program
// compilation, textures, the unit quad, the overlay slot, the render target,
// the blit, the cross-fade -- comes from avbase_gl (render/gl), which knows
// nothing about frames. The split exists so that avbase_gl can be used by a
// project that has never heard of VideoFrame.
//
// WHY THE CANVAS EXISTS. Drawing straight into the drawable makes the window
// the composition surface: there is nowhere to put a second source, no way to
// compose at a size of your choosing, and no way to read the result back.
// Putting a GlFramebuffer in between costs one extra full-viewport pass per
// frame -- measured as blit_ms -- and buys a composition surface independent of
// the window. Every frame goes through it, including the single-video case, so
// the path a later scene layer builds on is the path already being exercised;
// a fast path that bypassed the canvas would be the untested one.
//
// Window-system neutral: it holds a gl::NativeGLContext (SDL2 / GLFW / EGL /
// ...) and never names a window-system SDK. Each backend owns the context
// lifetime and hands it in, so the same YUV->RGB path runs unchanged over any
// of them.
//
// THREADING: Init()/Compose()/PresentComposed()/Present()/UploadLayer()/
// ComposeScene()/PresentScene()/ComposeTransition()/PresentTransition()/
// Shutdown() run on the sink's render thread; Init() makes the context current
// there and Shutdown() may be called from the host thread (it re-makes the
// context current before deleting). The host must not make the context current
// anywhere else.
class AVBASE_MEDIA_EXPORT GlPresenter {
 public:
  // Takes ownership of |ctx| (a borrowed wrapper over the host's context:
  // the wrapper does not destroy the host's window/context on destruction).
  explicit GlPresenter(std::unique_ptr<gl::NativeGLContext> ctx);
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  ~GlPresenter();

  // Makes the context current on the calling thread, loads the GL entry
  // points and builds the programs. Must be the first call.
  bool Init();

  // Source for the overlay pass; may be null (no overlay) and may be called
  // before or after Init(). Borrowed: the host keeps the slot alive for the
  // sink's lifetime.
  void SetOverlaySource(gl::TextOverlaySlot* slot) { overlay_slot_ = slot; }

  // Installs the host's hardware-import path; |importer| is BORROWED and may
  // be null, which is what a host without an interop backend supplies (see
  // GlHwTextureImporter and platform/hwimport). It is a property of the
  // CONTEXT rather than of a frame, so a host sets it once per presenter --
  // and must keep it alive until Shutdown(), because every upload reaches for
  // it without a null check of its own.
  //
  // With an importer installed, a frame whose pixels live on the GPU is
  // imported instead of copied and the present reports
  // UploadPath::kHwZeroCopy. Without one -- or when the backend accepts the
  // frame and the driver then refuses it -- the layer reads the frame back
  // itself and reports UploadPath::kCpuSync, so a host that cannot import is
  // expensive rather than broken. See GlVideoLayer::Upload().
  void SetHwTextureImporter(GlHwTextureImporter* importer) {
    hw_importer_ = importer;
  }

  // Composition resolution, in pixels. The frame is drawn into a canvas of
  // this size and then scaled to the window, so this decouples what is
  // composed from what is displayed -- a 1920x1080 canvas in an 800x450
  // window, or the reverse. A non-positive width or height means "no canvas of
  // its own: follow the drawable", which is the behaviour this path had before
  // the canvas existed, and is what SetCanvasSize(0, 0) restores.
  //
  // The size is a request, not an allocation: the render target is created (or
  // recreated) by the next Compose(), and a failure there is reported as a
  // compose failure. Call it before starting playback; the sink applies it on
  // the render thread.
  void SetCanvasSize(int width, int height);

  // Stage 1: uploads |frame| (kI420 / kYV12 / kNV12) and draws it -- plus the
  // overlay -- into the canvas. Returns false when the format or geometry is
  // not drawable, or when the canvas cannot be allocated; the caller reports a
  // submit failure. Honors the frame's colorspace/range by uploading the
  // matching conversion matrix.
  //
  // "Uploads" covers both routes -- a CPU copy and a hardware import -- because
  // which one applies is decided by the frame, not by the caller; see
  // GlVideoLayer::Upload. |timing->upload_path| says which one ran.
  //
  // |timing|, when non-null, receives upload_ms and compose_ms. A null
  // |timing| means no clock is read at all, which is what lets a caller that
  // never looks at the numbers pay nothing for them.
  bool Compose(const VideoFrame& frame, PresentTiming* timing = nullptr);

  // Stage 2: blits the canvas onto the default framebuffer at the drawable
  // size, scaling when the two differ. Does NOT swap -- that is Present()'s
  // job, and keeping it out is what lets a caller read the drawn pixels back
  // before they are handed to the window. |timing|, when non-null, receives
  // blit_ms.
  void PresentComposed(PresentTiming* timing = nullptr);

  // Compose + PresentComposed + SwapBuffers: the whole frame, and the one call
  // the sink makes. |timing|, when non-null, receives every stage.
  //
  // Each stage times itself, so each number measures exactly its own work and
  // present_ms measures exactly the swap. The inter-stage gaps (tens of
  // nanoseconds per frame) belong to no stage: the sum is a lower bound on the
  // frame, not a partition of it.
  bool Present(const VideoFrame& frame, PresentTiming* timing = nullptr);

  // ---- Composition with several sources ------------------------------------
  //
  // A scene is a parameter, not state. Which scene is on screen is a decision
  // the caller remakes every frame (that is what a cut between scenes is), and
  // a presenter that remembered one would then need a rule for a scene that
  // outlived its owner.
  //
  // The calls split along the line a frame does. Uploading happens when a
  // frame ARRIVES -- with N sources that is a different moment for each of
  // them, and often not the moment of the displayed frame; composing happens
  // once per displayed frame. Folding them together would force every source
  // to be in hand at present time, which is exactly what a live compositor
  // cannot assume.

  // Creates -- on the first call -- a scene owned by this presenter, and
  // returns it. Null if it could not be created.
  //
  // Ownership lives here for one reason: a scene's framebuffers, textures and
  // programs are GL objects of THIS context, so they have to be deleted while
  // it is current, and Shutdown() is the one place that already arranges that
  // on the host thread. A caller that kept its own scene would have to
  // reproduce the ordering, and the failure when it does not is a driver
  // warning at best and a crash at worst -- both after the window is gone.
  //
  // The presenter does not compose this scene on its own: it is a place to
  // build one, not a mode. Pass it to ComposeScene()/PresentScene() when it is
  // what should be on screen.
  GlScene* EnsureScene();

  // Uploads |frame| into one layer of |scene|, adding the elapsed time to
  // |timing->upload_ms|. Adding rather than assigning is deliberate: one
  // displayed frame can feed several layers, and what the frame cost to reach
  // the GPU is the sum of them. Assigning would report the last layer's upload
  // as if it were the whole frame's.
  //
  // |timing->upload_path| follows the merge rule stated on PresentTiming: one
  // layer copied means the frame was copied, so kHwZeroCopy from this call
  // only survives if every layer it was asked about reported it.
  bool UploadLayer(GlScene& scene, GlScene::LayerId id, const VideoFrame& frame,
                   PresentTiming* timing = nullptr);

  // Stage 1, multi-source: composes |scene| into the canvas, then the overlay
  // over it. The overlay sits above the scene rather than inside it -- it is
  // the host's HUD, and a scene is content.
  //
  // |timing| receives compose_ms only; an upload that fed this frame was timed
  // by the UploadLayer() call that performed it, which is where it happened.
  bool ComposeScene(GlScene& scene, PresentTiming* timing = nullptr);

  // Stage 1 + 2 + 3 for a scene.
  bool PresentScene(GlScene& scene, PresentTiming* timing = nullptr);

  // Which weight curve ComposeTransition() uses; see gl::TransitionKind.
  //
  // A presenter-wide setting rather than an argument to ComposeTransition(),
  // because the program is a function of it: passed per frame it would invite
  // a shader compile in the middle of a transition, which is the one place a
  // hitch is guaranteed to be visible. Changing it between transitions costs
  // one compile, once.
  //
  // Returns false for a value that is not one of the enum's named kinds.
  bool SetTransitionKind(gl::TransitionKind kind);
  gl::TransitionKind transition_kind() const { return transition_kind_; }

  // Stage 1, transition: composes |from| and |to| into two off-screen targets
  // AT THE COMPOSITION SIZE, then mixes them onto the canvas at |progress|
  // (0 = |from|, 1 = |to|) by whichever curve is currently selected.
  //
  // Both sides are composed at the output size rather than mixed at differing
  // sizes, so the mix is a combination of two images rather than a resample of
  // one into the other. A transition that also changed the output size would
  // be a resize, which is a separate effect with its own question (what
  // happens to the aspect ratio halfway through).
  //
  // The cost of a transition is one extra full composition per frame: two
  // scenes are drawn, not one. That is inherent -- to dissolve between two
  // pictures both have to exist -- and it is why compose_ms, not present_ms,
  // is the number that moves.
  bool ComposeTransition(GlScene& from, GlScene& to, float progress,
                         PresentTiming* timing = nullptr);

  // Stage 1 + 2 + 3 for a cross-fade.
  bool PresentTransition(GlScene& from, GlScene& to, float progress,
                         PresentTiming* timing = nullptr);

  // Destroys GL objects. Safe to call twice; after the sink's render thread
  // has stopped, this runs on the host thread and re-makes the context
  // current first.
  void Shutdown();

  // The loaded entry points, for callers that want to log the driver's
  // renderer/version or gate an optional feature. Null before Init().
  const gl::GlApi* api() const { return api_.get(); }

 private:
  bool BuildPrograms();
  // The transition program for the currently selected kind: the one from
  // Init() while the kind is unchanged, or a freshly compiled one the first
  // time it is used after a change. Null if the compile failed.
  gl::GlTransition* EnsureTransition();
  // Resolves the requested composition size -- following the drawable when the
  // request is non-positive -- and makes sure the canvas matches it. Shared by
  // every path that composes, because "how big is the composition" must not
  // have two answers depending on which one ran.
  bool EnsureCanvas();
  // PresentComposed + SwapBuffers: the tail every path ends with.
  bool Swap(PresentTiming* timing);
  // (Re)creates the overlay texture when the slot published a new version.
  bool EnsureOverlayTexture(const gl::TextOverlay& overlay);
  // Second draw pass: alpha-blends the slot's current bitmap over the video.
  // A no-op without a slot or with nothing published.
  void DrawOverlay();

  std::unique_ptr<gl::NativeGLContext>
      gl_ctx_;  // Borrowed wrapper, owned here.
  std::unique_ptr<gl::GlApi> api_;
  bool gl_current_{false};

  // Video pass. Shared with the scene this presenter builds, which is why it
  // is not a unique_ptr: see GlScene::CreateWithPrograms.
  std::shared_ptr<gl::GlProgram> program_;
  // The same pass over rectangle textures, for a layer whose last upload was a
  // hardware import. Both are built in Init(): which one a frame needs is not
  // known there, and a shader compile failure has to be reported before the
  // first frame rather than during it.
  std::shared_ptr<gl::GlProgram> rect_program_;
  std::unique_ptr<gl::GlProgram> overlay_program_;  // Overlay pass.
  std::unique_ptr<gl::GlQuad> quad_;
  // The one video source the single-frame path draws. The multi-source path
  // does not use it: a scene owns its own layers, so nothing is drawn twice.
  std::unique_ptr<GlVideoLayer> layer_;
  std::unique_ptr<gl::GlTexture> overlay_texture_;

  // The optional scene EnsureScene() hands out. Owned here so its GL objects
  // die in Shutdown() with the rest; see EnsureScene().
  std::unique_ptr<GlScene> scene_;

  // The composition surface and the pass that puts it on the window.
  std::unique_ptr<gl::GlFramebuffer> canvas_;
  std::unique_ptr<gl::GlBlitter> blitter_;

  // The two sides of a transition and the pass that blends them. Allocated in
  // Init() with everything else that compiles a shader, sized lazily by the
  // first ComposeTransition() at whatever the composition size then is.
  std::unique_ptr<gl::GlFramebuffer> transition_from_;
  std::unique_ptr<gl::GlFramebuffer> transition_to_;
  // Rebuilt by EnsureTransition() when |transition_kind_| moves away from the
  // kind |transition_| was compiled for, so the two never disagree.
  std::unique_ptr<gl::GlTransition> transition_;
  gl::TransitionKind transition_kind_{gl::TransitionKind::kCrossFade};

  // Requested composition size; non-positive in either axis means "follow the
  // drawable". The ALLOCATED size lives in canvas_ -- a second copy here is
  // how the two drift apart.
  int canvas_width_{0};
  int canvas_height_{0};

  gl::TextOverlaySlot* overlay_slot_{nullptr};  // Borrowed; may be null.
  // Borrowed; null when the host has no interop backend. Held as a raw pointer
  // because the sink owns it: it must outlive every presenter built for the
  // same context, including a rebuild after SetOutputTarget().
  GlHwTextureImporter* hw_importer_{nullptr};
  int overlay_version_{-1};
  int overlay_width_{0};
  int overlay_height_{0};
  // The last snapshot, and which slot version it holds. Kept as members so a
  // frame that republished nothing copies nothing: the version is read first
  // (one int, under a short lock) and the bitmap only moves when it changed.
  // Assigning into |overlay_| reuses its rgba capacity, so even then the
  // steady state does not allocate.
  gl::TextOverlay overlay_;
  int snapshot_version_{-1};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_PRESENT_H_
