/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/fmv_replacement.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

extern "C" {
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavformat/avformat.h"
#include "third_party/FFmpeg/libavutil/pixdesc.h"
}  // extern "C"

// Renamed from fmv_replacement_delay_ms, which older configs carry with a
// value meant for the old read-based timing; the picture match needs no
// allowance at all.
DEFINE_int32(fmv_replacement_offset_ms, 0,
             "Shifts the replacement against the game's own playback, in "
             "milliseconds. Positive plays it later.",
             "GPU");

DEFINE_int32(fmv_replacement_max_draws, 250,
             "A frame with at most this many guest draws is the game playing "
             "a movie rather than a scene. Lost Odyssey draws about 84 in a "
             "movie and 350-900 in an in-engine cutscene.",
             "GPU");

DEFINE_int32(fmv_replacement_min_frame_ms, 25,
             "A movie is also presented at its own rate (about 33 ms a frame "
             "for 30 fps) rather than the game's, so frames closer together "
             "than this are a scene, not a movie.",
             "GPU");

DEFINE_int32(fmv_replacement_min_contrast, 8,
             "A picture flatter than this (mean difference from its own "
             "average) is a fade or a black screen and cannot be matched "
             "against anything.",
             "GPU");

DEFINE_int32(fmv_replacement_match_threshold, 14,
             "How closely the guest's own picture must match a frame of the "
             "replacement (mean difference per pixel, 0-255) for it to count "
             "as that movie playing.",
             "GPU");

DEFINE_int32(fmv_replacement_arm_seconds, 60,
             "How long after a movie is read from the disc it may still start "
             "playing; after this the replacement is dropped.",
             "GPU");

DEFINE_path(fmv_replacement_dir, "",
            "Folder of replacement videos for game cutscenes (for instance "
            "upscaled copies): <title id>/<media id>/<archive>@<offset>.ivf, "
            "VP9 in IVF. Empty: off.",
            "GPU");

namespace xe {
namespace gpu {

namespace {

// The movie is over (or was skipped) once the guest stops drawing like one.
constexpr uint32_t kVideoGapMs = 300;
// A read of the same movie's header this long after the last one is a replay.
constexpr uint32_t kRestartMs = 3000;

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

// Limited-range YUV 4:2:0 to RGBA at the video's own size; the GPU scales it
// to the front buffer.
void ConvertFrame(const AVFrame* src, std::vector<uint8_t>& rgba) {
  const uint32_t width = uint32_t(src->width), height = uint32_t(src->height);
  rgba.resize(size_t(width) * height * 4);
  const bool bt709 = src->colorspace == AVCOL_SPC_BT709;
  const int32_t cr_r = bt709 ? 459 : 409;
  const int32_t cb_g = bt709 ? 55 : 100;
  const int32_t cr_g = bt709 ? 136 : 208;
  const int32_t cb_b = bt709 ? 541 : 516;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* yrow = src->data[0] + y * src->linesize[0];
    const uint8_t* urow = src->data[1] + (y >> 1) * src->linesize[1];
    const uint8_t* vrow = src->data[2] + (y >> 1) * src->linesize[2];
    uint8_t* out = rgba.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      const int32_t c = (int32_t(yrow[x]) - 16) * 298;
      const int32_t d = int32_t(urow[x >> 1]) - 128;
      const int32_t e = int32_t(vrow[x >> 1]) - 128;
      out[0] = uint8_t(std::clamp((c + cr_r * e + 128) >> 8, 0, 255));
      out[1] =
          uint8_t(std::clamp((c - cb_g * d - cr_g * e + 128) >> 8, 0, 255));
      out[2] = uint8_t(std::clamp((c + cb_b * d + 128) >> 8, 0, 255));
      out[3] = 255;
      out += 4;
    }
  }
}

// The movie's first seconds are decoded when it is read, so that the moment
// the guest puts it on screen can be recognised.
constexpr int32_t kPrefetchMs = 3000;
// How many played frames to keep for checking the guest is still on the movie.
constexpr size_t kRecentThumbs = 24;

FmvReplacement::Thumb ThumbFromYuv(const AVFrame* frame) {
  FmvReplacement::Thumb thumb{};
  const uint32_t w = uint32_t(frame->width), h = uint32_t(frame->height);
  for (uint32_t ty = 0; ty < FmvReplacement::kThumbHeight; ++ty) {
    const uint32_t y0 = ty * h / FmvReplacement::kThumbHeight;
    const uint32_t y1 = std::max((ty + 1) * h / FmvReplacement::kThumbHeight,
                                 y0 + 1);
    for (uint32_t tx = 0; tx < FmvReplacement::kThumbWidth; ++tx) {
      const uint32_t x0 = tx * w / FmvReplacement::kThumbWidth;
      const uint32_t x1 = std::max((tx + 1) * w / FmvReplacement::kThumbWidth,
                                   x0 + 1);
      uint32_t sum = 0, count = 0;
      for (uint32_t y = y0; y < y1; y += 2) {
        const uint8_t* row = frame->data[0] + y * frame->linesize[0];
        for (uint32_t x = x0; x < x1; x += 2) {
          sum += row[x];
          ++count;
        }
      }
      thumb[ty * FmvReplacement::kThumbWidth + tx] =
          uint8_t(count ? sum / count : 0);
    }
  }
  return thumb;
}

FmvReplacement::Thumb ThumbFromRgba(const uint8_t* rgba) {
  FmvReplacement::Thumb thumb{};
  for (size_t i = 0; i < thumb.size(); ++i) {
    const uint8_t* p = rgba + i * 4;
    // The guest's picture is limited-range video too; compare luma only.
    thumb[i] = uint8_t((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
  }
  return thumb;
}

// Spread of a thumbnail's luma. A flat picture (a black screen, a fade)
// matches anything once brightness is taken out, so it is not evidence.
uint32_t ThumbContrast(const FmvReplacement::Thumb& thumb) {
  int32_t sum = 0;
  for (uint8_t v : thumb) {
    sum += v;
  }
  const int32_t mean = sum / int32_t(thumb.size());
  uint32_t spread = 0;
  for (uint8_t v : thumb) {
    spread += uint32_t(std::abs(int32_t(v) - mean));
  }
  return spread / uint32_t(thumb.size());
}

// Mean absolute difference, ignoring an overall brightness shift (the guest
// has not applied its gamma ramp yet at this point, the replacement has not
// been through the same path either).
uint32_t ThumbDifference(const FmvReplacement::Thumb& a,
                         const FmvReplacement::Thumb& b) {
  int32_t sum_a = 0, sum_b = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    sum_a += a[i];
    sum_b += b[i];
  }
  const int32_t bias = (sum_a - sum_b) / int32_t(a.size());
  uint32_t total = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    total += uint32_t(std::abs(int32_t(a[i]) - int32_t(b[i]) - bias));
  }
  return total / uint32_t(a.size());
}

}  // namespace

FmvReplacement& FmvReplacement::Get() {
  static FmvReplacement instance;
  return instance;
}

FmvReplacement::~FmvReplacement() {
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    old = std::move(decoder_);
  }
  wake_.notify_all();
  if (old.joinable()) {
    old.join();
  }
}

void FmvReplacement::SetTitle(uint32_t title_id, uint32_t media_id) {
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    title_id_ = title_id;
    media_id_ = media_id;
    playing_ = nullptr;
    ++generation_;
    frame_.reset();
    old = std::move(decoder_);
    Rescan();
  }
  wake_.notify_all();
  if (old.joinable()) {
    old.join();
  }
}

void FmvReplacement::RescanFolder() {
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    playing_ = nullptr;
    ++generation_;
    frame_.reset();
    old = std::move(decoder_);
    Rescan();
  }
  wake_.notify_all();
  if (old.joinable()) {
    old.join();
  }
}

void FmvReplacement::Rescan() {
  movies_.clear();
  have_movies_ = false;
  if (cvars::fmv_replacement_dir.empty() || !title_id_) {
    return;
  }
  std::filesystem::path dir = cvars::fmv_replacement_dir /
                              fmt::format("{:08X}", title_id_) /
                              fmt::format("{:08X}", media_id_);
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    XELOGI("FMV replacement: no folder {}", dir.string());
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".ivf") {
      continue;
    }
    const std::string stem = entry.path().stem().string();
    const size_t at = stem.rfind('@');
    if (at == std::string::npos || at == 0) {
      continue;
    }
    Movie movie;
    movie.archive = ToLower(std::string_view(stem).substr(0, at));
    movie.offset = std::strtoull(stem.c_str() + at + 1, nullptr, 16);
    movie.path = entry.path().string();
    movies_.push_back(std::move(movie));
  }
  std::sort(movies_.begin(), movies_.end(),
            [](const Movie& a, const Movie& b) {
              return a.archive != b.archive ? a.archive < b.archive
                                            : a.offset < b.offset;
            });
  have_movies_ = !movies_.empty();
  XELOGI("FMV replacement: {} movie(s) in {}", movies_.size(), dir.string());
}

void FmvReplacement::OnDiscRead(std::string_view file_name, uint64_t offset,
                                uint64_t length) {
  (void)length;
  if (!have_movies_) {
    return;
  }
  const std::string name = ToLower(file_name);
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t now = Clock::QueryGuestUptimeMillis();
    if (playing_ && playing_->archive == name && offset >= playing_->offset &&
        (offset != playing_->offset || now - arm_guest_ms_ < kRestartMs)) {
      // Beyond the next known movie of the same archive is not this one.
      const Movie* next = playing_ + 1;
      if (next == movies_.data() + movies_.size() || next->archive != name ||
          offset < next->offset) {
        if (!started_) {
          // Still waiting for the guest to play it; keep the wait alive.
          arm_guest_ms_ = now;
        }
        return;
      }
    }
    const Movie* found = nullptr;
    for (const Movie& movie : movies_) {
      if (movie.archive == name && movie.offset == offset) {
        found = &movie;
        break;
      }
    }
    if (!found) {
      return;
    }
    old = std::move(decoder_);
    playing_ = found;
    ++generation_;
    arm_guest_ms_ = now;
    start_guest_ms_ = now;
    last_video_guest_ms_ = 0;
    started_ = false;
    duration_ms_ = 0;
    decoder_finished_ = false;
    frame_.reset();
    start_thumbs_.clear();
    recent_thumbs_.clear();
    matched_recently_ = false;
    XELOGI("FMV replacement: {} at {:X} read at uptime {}, ready to play {}",
           name, offset, now, found->path);
    decoder_ = std::thread(&FmvReplacement::DecodeThread, this, found->path,
                           generation_);
  }
  wake_.notify_all();
  if (old.joinable()) {
    old.join();
  }
}

bool FmvReplacement::WantsGuestThumbnail() {
  if (!have_movies_ || !thumbnails_ok_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return playing_ != nullptr;
}

void FmvReplacement::SetGuestThumbnailUnavailable() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thumbnails_ok_) {
    thumbnails_ok_ = false;
    XELOGW(
        "FMV replacement: cannot read the guest's picture, falling back to "
        "the draw count");
  }
}

void FmvReplacement::OnGuestThumbnail(const uint8_t* rgba) {
  const Thumb guest = ThumbFromRgba(rgba);
  const uint32_t guest_contrast = ThumbContrast(guest);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  ++thumbnails_seen_;
  const bool too_flat =
      guest_contrast <
      uint32_t(std::max(cvars::fmv_replacement_min_contrast, 0));
  if (too_flat && !started_) {
    // A fade or a black screen matches anything, so it cannot start a movie.
    return;
  }
  if (too_flat) {
    // Mid-movie it is a fade in the movie itself: no evidence either way, so
    // carry on rather than counting it as the guest leaving.
    matched_recently_ = true;
    last_video_guest_ms_ = Clock::QueryGuestUptimeMillis();
    return;
  }
  const uint32_t threshold =
      uint32_t(std::max(cvars::fmv_replacement_match_threshold, 0));
  const uint32_t now = Clock::QueryGuestUptimeMillis();
  if (!started_) {
    // Which frame of the movie's opening is on screen, if any?
    uint32_t best = UINT32_MAX;
    int32_t best_pts = 0;
    uint32_t best_contrast = 0;
    for (const auto& [pts, thumb] : start_thumbs_) {
      const uint32_t difference = ThumbDifference(guest, thumb);
      if (difference < best) {
        best = difference;
        best_pts = pts;
        best_contrast = ThumbContrast(thumb);
      }
    }
    if (best <= threshold && best_contrast >= guest_contrast / 2) {
      started_ = true;
      matched_recently_ = true;
      // Line the clock up with the frame the guest is actually showing.
      start_guest_ms_ = uint32_t(int64_t(now) - best_pts);
      last_video_guest_ms_ = now;
      XELOGI(
          "FMV replacement: {} on screen at uptime {} ({} ms after the read), "
          "the guest is at {} ms, match {}",
          playing_->path, now, now - arm_guest_ms_, best_pts, best);
    }
    return;
  }
  uint32_t best = UINT32_MAX;
  int32_t best_pts = 0;
  for (const auto& [pts, thumb] : recent_thumbs_) {
    const uint32_t difference = ThumbDifference(guest, thumb);
    if (difference < best) {
      best = difference;
      best_pts = pts;
    }
  }
  if (thumbnails_seen_ % 300 == 1) {
    XELOGD(
        "FMV replacement: playing, best match {} at {} ms of {} frames kept, "
        "guest contrast {}",
        best, best_pts, recent_thumbs_.size(), guest_contrast);
  }
  if (best <= threshold) {
    matched_recently_ = true;
    last_video_guest_ms_ = now;
  }
}

std::shared_ptr<const FmvReplacement::Frame> FmvReplacement::GetFrame(
    uint32_t guest_draws) {
  if (!have_movies_) {
    return nullptr;
  }
  std::thread old;
  std::shared_ptr<const Frame> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
      return nullptr;
    }
    const uint32_t now = Clock::QueryGuestUptimeMillis();
    // Smoothed time between swaps: a movie runs at its own frame rate, a game
    // at its own. Together with the draw count this separates a movie from a
    // menu that also draws very little.
    if (last_swap_guest_ms_) {
      const float interval = float(now - last_swap_guest_ms_);
      swap_interval_ms_ = swap_interval_ms_
                              ? swap_interval_ms_ * 0.75f + interval * 0.25f
                              : interval;
    }
    last_swap_guest_ms_ = now;
    const bool draws_like_movie =
        guest_draws <= uint32_t(std::max(cvars::fmv_replacement_max_draws, 0)) &&
        swap_interval_ms_ >= float(cvars::fmv_replacement_min_frame_ms);
    // The guest's own picture matching a frame of this movie is what says it
    // is playing; the draw count is only for hosts where it cannot be read.
    const bool guest_playing_movie =
        thumbnails_ok_ ? matched_recently_ : draws_like_movie;
    const char* ended = nullptr;
    if (!started_) {
      // The movie has been read but the game has not reached it yet: it is
      // still drawing a scene. Wait, but not for ever.
      if (guest_playing_movie && !thumbnails_ok_) {
        started_ = true;
        start_guest_ms_ = now;
        last_video_guest_ms_ = now;
        XELOGI("FMV replacement: {} on screen at uptime {}, {} ms after the "
               "read (by draw count)",
               playing_->path, now, now - arm_guest_ms_);
      } else if (now - arm_guest_ms_ >
                 uint32_t(std::max(cvars::fmv_replacement_arm_seconds, 1)) *
                     1000u) {
        ended = "never played";
      }
    } else {
      if (guest_playing_movie) {
        last_video_guest_ms_ = now;
      }
      matched_recently_ = false;
      const uint32_t elapsed = uint32_t(std::max<int64_t>(
          int64_t(now - start_guest_ms_) - cvars::fmv_replacement_offset_ms, 0));
      if (now - last_video_guest_ms_ > kVideoGapMs) {
        ended = "the guest stopped playing it";
      } else if (decoder_finished_ &&
                 (!duration_ms_ || elapsed > duration_ms_ + 250 || !frame_)) {
        ended = "it ran out";
      }
    }
    if (ended) {
      XELOGI(
          "FMV replacement: {} stopped ({}) at uptime {}, {} ms since a match, "
          "{} guest thumbnails seen",
          playing_->path, ended, now, now - last_video_guest_ms_,
          thumbnails_seen_);
      playing_ = nullptr;
      started_ = false;
      ++generation_;
      frame_.reset();
      old = std::move(decoder_);
    } else if (started_) {
      result = frame_;
    }
  }
  if (old.joinable()) {
    wake_.notify_all();
    old.join();
  }
  return result;
}

void FmvReplacement::DecodeThread(std::string path, uint64_t generation) {
  threading::set_name("FMV Replacement");
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = av_packet_alloc();
  AVFrame* current = av_frame_alloc();
  AVFrame* next = av_frame_alloc();
  bool have_current = false, have_next = false, eof = false, dirty = false;
  Thumb current_thumb{};
  int32_t current_thumb_ms = 0;
  bool have_thumb = false;
  int stream_index = -1;
  double time_base = 0.0;
  uint64_t frame_id = 0;

  auto finish = [&]() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ == generation) {
      decoder_finished_ = true;
    }
  };

  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 ||
      avformat_find_stream_info(format, nullptr) < 0) {
    XELOGE("FMV replacement: cannot open {}", path);
    finish();
  } else {
    stream_index =
        av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* decoder =
        stream_index >= 0
            ? avcodec_find_decoder(format->streams[stream_index]->codecpar
                                       ->codec_id)
            : nullptr;
    if (decoder) {
      codec = avcodec_alloc_context3(decoder);
      avcodec_parameters_to_context(
          codec, format->streams[stream_index]->codecpar);
      codec->thread_count = 8;
      codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (!codec || avcodec_open2(codec, decoder, nullptr) < 0) {
      XELOGE("FMV replacement: no decoder for {}", path);
      finish();
    } else {
      const AVStream* stream = format->streams[stream_index];
      time_base = av_q2d(stream->time_base);
      // IVF written by concatenation carries a frame count where the
      // container duration is expected, so prefer frames over the rate.
      uint64_t duration_ms = 0;
      const double fps = av_q2d(stream->avg_frame_rate);
      if (stream->nb_frames > 0 && fps > 0.0) {
        duration_ms = uint64_t(stream->nb_frames / fps * 1000.0);
      } else if (stream->duration > 0) {
        duration_ms = uint64_t(stream->duration * time_base * 1000.0);
      } else if (format->duration > 0) {
        duration_ms = uint64_t(format->duration / 1000);
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation_ == generation) {
        duration_ms_ = duration_ms;
      }
      XELOGI("FMV replacement: {} {}x{} {} ms", path, codec->width,
             codec->height, duration_ms);
    }
  }

  // Decodes the next frame into `next`; false at the end of the stream.
  auto decode_next = [&]() -> bool {
    while (true) {
      int ret = avcodec_receive_frame(codec, next);
      if (ret == 0) {
        return true;
      }
      if (ret != AVERROR(EAGAIN)) {
        return false;
      }
      ret = av_read_frame(format, packet);
      if (ret < 0) {
        avcodec_send_packet(codec, nullptr);
        continue;
      }
      if (packet->stream_index == stream_index) {
        avcodec_send_packet(codec, packet);
      }
      av_packet_unref(packet);
    }
  };

  // Decode the movie's opening now, while the guest is still showing whatever
  // comes before it, so the frame it starts with can be recognised.
  if (codec) {
    std::vector<std::pair<int32_t, Thumb>> opening;
    while (decode_next()) {
      const int64_t pts = next->best_effort_timestamp;
      const int32_t pts_ms =
          pts == AV_NOPTS_VALUE ? 0 : int32_t(pts * time_base * 1000.0);
      if (next->format == AV_PIX_FMT_YUV420P) {
        opening.emplace_back(pts_ms, ThumbFromYuv(next));
      }
      if (pts_ms >= kPrefetchMs) {
        break;
      }
    }
    av_seek_frame(format, stream_index, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec);
    have_next = false;
    eof = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ == generation) {
      start_thumbs_ = std::move(opening);
    }
  }

  if (codec) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (generation_ == generation) {
      // Until the guest reaches the movie, hold at the first frame: the file
      // is read seconds before it plays.
      const int64_t raw_ms =
          started_ ? int64_t(Clock::QueryGuestUptimeMillis() - start_guest_ms_) -
                         cvars::fmv_replacement_offset_ms
                   : 0;
      const uint32_t target_ms = uint32_t(std::max<int64_t>(raw_ms, 0));
      lock.unlock();
      bool progressed = false;
      if (!have_next && !eof) {
        have_next = decode_next();
        eof = !have_next;
        progressed = true;
      }
      if (have_next) {
        int64_t pts = next->best_effort_timestamp;
        const double pts_ms =
            pts == AV_NOPTS_VALUE ? 0.0 : pts * time_base * 1000.0;
        if (pts_ms <= double(target_ms)) {
          std::swap(current, next);
          have_current = true;
          have_next = false;
          dirty = true;
          progressed = true;
          if (current->format == AV_PIX_FMT_YUV420P) {
            current_thumb = ThumbFromYuv(current);
            current_thumb_ms = int32_t(pts_ms);
            have_thumb = true;
          }
        }
      }
      std::shared_ptr<Frame> converted;
      // Only convert once caught up, so a late start skips frames cheaply.
      if (dirty && have_current && !have_next && !eof) {
        // Still behind: decode more before converting.
      } else if (dirty && have_current &&
                 current->format != AV_PIX_FMT_YUV420P) {
        // Anything else (an RGB VP9 profile 1 file, say) would silently show
        // nothing; say so and stop.
        XELOGE(
            "FMV replacement: {} is {}, not yuv420p - re-encode it with "
            "-pix_fmt yuv420p",
            path, av_get_pix_fmt_name(AVPixelFormat(current->format)));
        dirty = false;
        lock.lock();
        decoder_finished_ = true;
        break;
      } else if (dirty && have_current) {
        converted = std::make_shared<Frame>();
        converted->id = ++frame_id;
        converted->width = uint32_t(current->width);
        converted->height = uint32_t(current->height);
        ConvertFrame(current, converted->rgba);
        dirty = false;
      }
      lock.lock();
      if (generation_ != generation) {
        break;
      }
      if (have_thumb) {
        // What is on screen now, for checking the guest is still on this
        // movie (and has not skipped it).
        recent_thumbs_.emplace_back(current_thumb_ms, current_thumb);
        while (recent_thumbs_.size() > kRecentThumbs) {
          recent_thumbs_.pop_front();
        }
        have_thumb = false;
      }
      if (converted) {
        frame_ = std::move(converted);
        if ((frame_id % 300) == 0) {
          const double shown_ms =
              current->best_effort_timestamp == AV_NOPTS_VALUE
                  ? 0.0
                  : current->best_effort_timestamp * time_base * 1000.0;
          XELOGD("FMV replacement: frame {} shows {:.0f} ms, wanted {} ms",
                 frame_id, shown_ms, target_ms);
        }
      }
      if (eof && !have_next) {
        decoder_finished_ = true;
      }
      if (!progressed || (eof && !dirty)) {
        wake_.wait_for(lock, std::chrono::milliseconds(4));
      }
    }
  }

  av_frame_free(&current);
  av_frame_free(&next);
  av_packet_free(&packet);
  if (codec) {
    avcodec_free_context(&codec);
  }
  if (format) {
    avformat_close_input(&format);
  }
}

}  // namespace gpu
}  // namespace xe
