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

DEFINE_int32(fmv_replacement_delay_ms, 1900,
             "How long after a game starts reading a movie it shows the first "
             "frame (its buffering), subtracted from the replacement's clock.",
             "GPU");

DEFINE_path(fmv_replacement_dir, "",
            "Folder of replacement videos for game cutscenes (for instance "
            "upscaled copies): <title id>/<media id>/<archive>@<offset>.ivf, "
            "VP9 in IVF. Empty: off.",
            "GPU");

namespace xe {
namespace gpu {

namespace {

// The game reads a movie ahead of playback, so near the end reads stop while
// the picture still runs; only a gap well before the end is a skip.
constexpr uint32_t kReadAheadMs = 10000;
constexpr uint32_t kSkipGapMs = 2500;
// A read of the same movie's header this long after the start is a replay.
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
        (offset != playing_->offset || now - start_guest_ms_ < kRestartMs)) {
      // Beyond the next known movie of the same archive is not this one.
      const Movie* next = playing_ + 1;
      if (next == movies_.data() + movies_.size() || next->archive != name ||
          offset < next->offset) {
        last_read_guest_ms_ = now;
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
    start_guest_ms_ = now;
    last_read_guest_ms_ = now;
    duration_ms_ = 0;
    decoder_finished_ = false;
    frame_.reset();
    XELOGI("FMV replacement: {} at {:X} started at uptime {}, playing {}", name,
           offset, now, found->path);
    decoder_ = std::thread(&FmvReplacement::DecodeThread, this, found->path,
                           generation_);
  }
  wake_.notify_all();
  if (old.joinable()) {
    old.join();
  }
}

std::shared_ptr<const FmvReplacement::Frame> FmvReplacement::GetFrame() {
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
    const uint32_t elapsed = uint32_t(std::max<int64_t>(
        int64_t(now - start_guest_ms_) - cvars::fmv_replacement_delay_ms, 0));
    bool ended = false;
    if (decoder_finished_ &&
        (!duration_ms_ || elapsed > duration_ms_ + 250 || !frame_)) {
      ended = true;
    } else if (duration_ms_ && elapsed + kReadAheadMs < duration_ms_ &&
               now - last_read_guest_ms_ > kSkipGapMs) {
      XELOGI("FMV replacement: reads stopped at {} ms of {}, skipped",
             elapsed, duration_ms_);
      ended = true;
    }
    if (ended) {
      XELOGI("FMV replacement: {} ended at {} ms", playing_->path, elapsed);
      playing_ = nullptr;
      ++generation_;
      frame_.reset();
      old = std::move(decoder_);
    } else {
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

  if (codec) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (generation_ == generation) {
      const int64_t raw_ms =
          int64_t(Clock::QueryGuestUptimeMillis() - start_guest_ms_) -
          cvars::fmv_replacement_delay_ms;
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
