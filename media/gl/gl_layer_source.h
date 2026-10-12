// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_LAYER_SOURCE_H_
#define AVBASE_MEDIA_GL_GL_LAYER_SOURCE_H_

#include <cstdint>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/latest_frame_queue.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/media_export.h"

namespace avbase::media {

// One input stream and the scene layer it feeds.
//
// This is the object a compositor needs per source and the player's single
// video path never had: a producer that runs at its own rate, a slot for the
// newest frame, and a layer whose textures outlive both. The two halves meet
// in Sync(), on the GL thread, once per displayed frame.
//
// WHY THE MAILBOX AND NOT A CALL. A second source is decoded by another
// pipeline on another thread, so "here is a frame" arrives at a moment that
// has nothing to do with the display's. Handing the frame straight to the
// layer would either block the producer on the display or drop the frame on
// the floor; the slot does neither, and the cost of the decoupling is that a
// frame can be replaced before it is shown -- which is the right trade for a
// picture.
//
// WHY IT HOLDS THE FRAME IT UPLOADED. Two reasons, and the second is not
// obvious:
//
//   * a source that stops producing must not go black. The layer's textures
//     still hold the last picture, so "no new frame" and "no upload" are the
//     same thing here and the composition keeps what it had.
//   * an IMPORTED frame is not copied. Its textures point at the decoder's own
//     surface, which the frame's release callback frees -- so dropping the
//     reference right after the upload would leave the layer sampling a
//     surface that is already gone. Holding it is what makes zero-copy safe,
//     not merely tidy.
//
// THREADING: Publish() from any thread, any number of them. EnsureLayer(),
// Sync() and Detach() on the thread holding the GL context. The counts may be
// read from anywhere; they come from the mailbox, which owns them.
class AVBASE_MEDIA_EXPORT GlLayerSource {
 public:
  // |name| is for logs and for finding a source again; |z| is where its layer
  // sits in the scene (higher is drawn later, i.e. on top).
  GlLayerSource(std::string name, int z);
  GlLayerSource(const GlLayerSource&) = delete;
  GlLayerSource& operator=(const GlLayerSource&) = delete;
  ~GlLayerSource();

  const std::string& name() const { return name_; }
  int z() const { return z_; }

  // ---- Producer side. Any thread. ------------------------------------------
  //
  // Hands the newest frame over. A frame that was waiting and has not been
  // consumed yet is superseded; see LatestFrameQueue::Publish, which returns
  // it to nobody here because a decoded frame has nowhere to go back to -- a
  // producer that wants the number reads frames_superseded().
  void Publish(base::scoped_refptr<VideoFrame> frame);

  // ---- Consumer side. GL thread. -------------------------------------------

  // Creates this source's layer in |scene| if it does not have one, and
  // returns it. kNoLayer if the scene refused one.
  //
  // Separate from Sync() so a host can place the layer -- rect, opacity, z --
  // before the first frame reaches it, which it cannot do through a handle
  // that only exists after an upload.
  GlScene::LayerId EnsureLayer(GlScene& scene);

  // Takes the newest frame, uploads it into the layer, and keeps it. True when
  // the layer is in a drawable state, which includes the case where NO frame
  // arrived: a source that is late keeps its last picture, and that is a
  // success, not a failure. False only when the layer cannot be created or the
  // upload was refused, both of which are the caller's problem to report.
  //
  // |timing| accumulates this source's upload, so N sources cost the sum of
  // their uploads in the frame that displayed them -- see
  // GlPresenter::UploadLayer.
  bool Sync(GlPresenter& presenter, GlScene& scene,
            PresentTiming* timing = nullptr);

  // Forgets the layer and drops the frame being held. Call it when the scene
  // is going away; |scene| may be null, which is what Shutdown() passes once
  // the scene is already gone. GL thread, or the host's after the render
  // thread has stopped.
  void Detach(GlScene* scene);

  bool has_layer() const { return layer_ != GlScene::kNoLayer; }
  GlScene::LayerId layer() const { return layer_; }

  // ---- Counts. Readable from any thread. -----------------------------------
  uint64_t frames_published() const;
  uint64_t frames_consumed() const;
  uint64_t frames_superseded() const;

 private:
  const std::string name_;
  const int z_;

  // The producer/consumer seam. Owned here rather than by the scene: the
  // scene is a GL object and lives and dies with a context, while the stream
  // feeding this source does not stop because the window was rebuilt.
  LatestFrameQueue queue_;

  // Both GL-thread-only.
  GlScene::LayerId layer_{GlScene::kNoLayer};
  // The frame whose pixels the layer is currently showing. See the class
  // comment for why it is held and not released after the upload.
  base::scoped_refptr<VideoFrame> displayed_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_LAYER_SOURCE_H_
