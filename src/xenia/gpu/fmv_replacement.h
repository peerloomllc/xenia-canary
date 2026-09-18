/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_FMV_REPLACEMENT_H_
#define XENIA_GPU_FMV_REPLACEMENT_H_

#include <array>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace xe {
namespace gpu {

// Plays a replacement video (for instance an upscaled copy of a game's FMV)
// in place of the guest's front buffer while the game plays the original.
//
// A movie is recognised by the game reading a disc file at a known offset: the
// replacement for the read of `<archive>` at byte `<offset>` is
// `<fmv_replacement_dir>/<title id>/<media id>/<archive>@<offset hex>.ivf`
// (VP9 in IVF). Playback is clocked by the guest uptime from that read, so it
// follows pause and speed changes, and ends when the video ends or the game
// stops reading the movie early (a skip).
class FmvReplacement {
 public:
  // A frame boiled down to luma, for telling whether the guest is showing
  // this movie and where in it.
  static constexpr uint32_t kThumbWidth = 32;
  static constexpr uint32_t kThumbHeight = 18;
  using Thumb = std::array<uint8_t, kThumbWidth * kThumbHeight>;

  struct Frame {
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
  };

  static FmvReplacement& Get();

  // The running title and disc; rescans the replacement folder.
  void SetTitle(uint32_t title_id, uint32_t media_id);

  // After the replacement folder setting changes.
  void RescanFolder();

  // From the disc file system, on the reading thread.
  void OnDiscRead(std::string_view file_name, uint64_t offset,
                  uint64_t length);

  // True while a movie has been read but not finished: the swap should
  // capture a thumbnail of the guest's own picture.
  bool WantsGuestThumbnail();

  // A kThumbWidth x kThumbHeight RGBA8 thumbnail of the guest's own frame.
  void OnGuestThumbnail(const uint8_t* rgba);

  // The swap cannot capture thumbnails on this host, so fall back to judging
  // by the guest's draw count.
  void SetGuestThumbnailUnavailable();

  // From the swap, with the number of draws the guest made for this frame:
  // the frame to show at the video's own size (the caller scales it), or null
  // when no replacement should be on screen. A game reads a movie from the
  // disc seconds before it plays it, often while an in-engine scene is on
  // screen, so the draw count is what says the movie is actually playing.
  std::shared_ptr<const Frame> GetFrame(uint32_t guest_draws);

 private:
  struct Movie {
    std::string archive;  // Lower case.
    uint64_t offset = 0;
    std::string path;
  };

  FmvReplacement() = default;
  ~FmvReplacement();

  void Rescan();
  void Stop();
  void DecodeThread(std::string path, uint64_t generation);

  std::mutex mutex_;
  uint32_t title_id_ = 0;
  uint32_t media_id_ = 0;
  std::vector<Movie> movies_;
  std::atomic<bool> have_movies_ = false;

  // The movie playing, guarded by mutex_.
  const Movie* playing_ = nullptr;
  uint64_t generation_ = 0;
  uint32_t start_guest_ms_ = 0;   // When the guest's own movie started.
  uint32_t arm_guest_ms_ = 0;     // When the movie was read from the disc.
  uint32_t last_video_guest_ms_ = 0;
  bool started_ = false;
  uint32_t last_swap_guest_ms_ = 0;
  float swap_interval_ms_ = 0.0f;
  bool thumbnails_ok_ = true;
  // The movie's opening, decoded when it is read, so its first frame on
  // screen can be recognised; then the frames around the one being shown.
  std::vector<std::pair<int32_t, Thumb>> start_thumbs_;
  std::deque<std::pair<int32_t, Thumb>> recent_thumbs_;
  bool matched_recently_ = false;
  uint64_t thumbnails_seen_ = 0;
  uint64_t duration_ms_ = 0;  // 0 until the decoder knows.
  bool decoder_finished_ = false;
  std::shared_ptr<const Frame> frame_;
  std::thread decoder_;
  std::condition_variable wake_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_FMV_REPLACEMENT_H_
