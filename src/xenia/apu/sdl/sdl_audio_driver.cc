/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_driver.h"

#include "xenia/base/clock.h"

#include "xenia/apu/audio_system.h"
#include "third_party/soundtouch/include/SoundTouch.h"

#include <algorithm>

#include <cstring>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

SDLAudioDriver::SDLAudioDriver(xe::threading::Semaphore* semaphore,
                               uint32_t frequency, uint32_t channels,
                               bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
}

SDLAudioDriver::~SDLAudioDriver() = default;

namespace {
enum class ScaledAudioMode { kResample, kStretch, kMute };
ScaledAudioMode ParseScaledAudioMode(const std::string& value) {
  if (value == "stretch") return ScaledAudioMode::kStretch;
  if (value == "mute") return ScaledAudioMode::kMute;
  return ScaledAudioMode::kResample;
}
}  // namespace

// Slow-motion beyond 4x would need more than 4 device frames per guest frame;
// the ALSA driver stops there too.
static constexpr double kMinAudioScalar = 0.25;
static constexpr double kMaxAudioScalar = 16.0;

bool SDLAudioDriver::Initialize() {
  SDL_version ver = {};
  SDL_GetVersion(&ver);
  if ((ver.major < 2) || (ver.major == 2 && ver.minor == 0 && ver.patch < 8)) {
    XELOGW(
        "SDL library version {}.{}.{} is outdated. "
        "You may experience choppy audio.",
        ver.major, ver.minor, ver.patch);
  }

  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    return false;
  }
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec;
  desired_spec.freq = frame_frequency_;
  desired_spec.format = AUDIO_F32;
  desired_spec.channels = frame_channels_;
  desired_spec.samples = channel_samples_;
  desired_spec.callback = SDLCallback;
  desired_spec.userdata = this;
  // Allow the hardware to decide between 5.1 and stereo,
  // unless the input is stereo
  int allowed_change =
      frame_channels_ != 2 ? SDL_AUDIO_ALLOW_CHANNELS_CHANGE : 0;
  for (int i = 0; i < 2; i++) {
    sdl_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired_spec,
                                         &obtained_spec, allowed_change);
    if (sdl_device_id_ <= 0) {
      XELOGE("SDL_OpenAudioDevice() failed.");
      return false;
    }
    if (obtained_spec.channels == 2 || obtained_spec.channels == 6) {
      break;
    }
    // If the system is 4 or 7.1, let SDL convert
    allowed_change = 0;
    SDL_CloseAudioDevice(sdl_device_id_);
    sdl_device_id_ = -1;
  }
  if (sdl_device_id_ <= 0) {
    XELOGE("Failed to get a compatible SDL Audio Device.");
    return false;
  }
  sdl_device_channels_ = obtained_spec.channels;

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    convert_buffer_.assign(size_t(channel_samples_) * sdl_device_channels_,
                           0.0f);
    resample_buffer_.assign(
        (size_t(channel_samples_) * 4 + 2) * sdl_device_channels_, 0.0f);
    ring_.assign(AudioSystem::kMaximumQueuedFrames * size_t(channel_samples_) *
                     4 * sdl_device_channels_,
                 0.0f);
    ring_read_ = 0;
    ring_count_ = 0;
    guest_frame_device_frames_.clear();
    front_consumed_device_frames_ = 0;
    resample_position_ = 0.0;
    stretcher_ = std::make_unique<soundtouch::SoundTouch>();
    stretcher_->setSampleRate(frame_frequency_);
    stretcher_->setChannels(sdl_device_channels_);
    stretcher_->setSetting(SETTING_USE_QUICKSEEK, 0);
    stretcher_->setSetting(SETTING_USE_AA_FILTER, 1);
    // Shorter grains than the defaults keep transients crisper at 2x.
    stretcher_->setSetting(SETTING_SEQUENCE_MS, 40);
    stretcher_->setSetting(SETTING_SEEKWINDOW_MS, 15);
    stretcher_->setSetting(SETTING_OVERLAP_MS, 8);
    stretcher_active_ = false;
    expected_frames_accumulator_ = 0.0;
  }

  SDL_PauseAudioDevice(sdl_device_id_, 0);

  return true;
}

void SDLAudioDriver::SubmitFrame(float* frame) {
  const uint32_t channels = sdl_device_channels_;
  float* converted = convert_buffer_.data();
  if (need_format_conversion_) {
    switch (channels) {
      case 2:
        conversion::sequential_6_BE_to_interleaved_2_LE(converted, frame,
                                                        channel_samples_);
        break;
      case 6:
        conversion::sequential_6_BE_to_interleaved_6_LE(converted, frame,
                                                        channel_samples_);
        break;
      default:
        assert_unhandled_case(channels);
        return;
    }
  } else {
    assert_true(channels == frame_channels_);
    std::memcpy(converted, frame, frame_size_);
  }

  // Scale by the guest time scalar. 'resample' keeps the tape-deck pitch
  // shift; 'stretch' runs SoundTouch's WSOLA so pitch is preserved; 'mute'
  // plays silence of the scaled duration. The number of device frames
  // charged to this guest frame is channel_samples_ / scalar either way so
  // the producer's slot is released at the right average rate.
  const double scalar = std::clamp(Clock::guest_time_scalar(), kMinAudioScalar,
                                   kMaxAudioScalar);
  const float* out = converted;
  size_t out_frames = channel_samples_;
  size_t charged_frames = channel_samples_;
  ScaledAudioMode mode = ScaledAudioMode::kResample;
  if (scalar != 1.0) {
    mode = ParseScaledAudioMode(scalar > 1.0 ? cvars::fast_forward_audio
                                             : cvars::slow_motion_audio);
    expected_frames_accumulator_ += double(channel_samples_) / scalar;
    charged_frames = size_t(expected_frames_accumulator_);
    expected_frames_accumulator_ -= double(charged_frames);
  } else {
    expected_frames_accumulator_ = 0.0;
  }
  if (scalar == 1.0 || mode != ScaledAudioMode::kStretch) {
    if (stretcher_active_) {
      stretcher_->clear();
      stretcher_active_ = false;
    }
  }
  float* scratch = resample_buffer_.data();
  const size_t scratch_capacity = resample_buffer_.size() / channels;
  if (scalar != 1.0 && mode == ScaledAudioMode::kStretch) {
    if (!stretcher_active_) {
      stretcher_->clear();
      stretcher_active_ = true;
    }
    stretcher_->setTempo(scalar);
    stretcher_->putSamples(converted, channel_samples_);
    out_frames = stretcher_->receiveSamples(scratch, uint32_t(scratch_capacity));
    out = scratch;
    resample_position_ = 0.0;
  } else if (scalar != 1.0 && mode == ScaledAudioMode::kMute) {
    out_frames = std::min(charged_frames, scratch_capacity);
    std::memset(scratch, 0, out_frames * channels * sizeof(float));
    out = scratch;
    resample_position_ = 0.0;
  } else if (scalar != 1.0) {
    // Resample: step > 1 consumes input faster (fewer, higher-pitched
    // output frames), step < 1 stretches it.
    const double step = scalar;
    double position = resample_position_;
    out_frames = 0;
    while (out_frames < scratch_capacity) {
      size_t index = size_t(position);
      if (index + 1 >= channel_samples_) {
        break;
      }
      float frac = float(position - double(index));
      const float* a = converted + index * channels;
      const float* b = a + channels;
      float* o = scratch + out_frames * channels;
      for (uint32_t ch = 0; ch < channels; ++ch) {
        o[ch] = a[ch] + (b[ch] - a[ch]) * frac;
      }
      position += step;
      ++out_frames;
    }
    resample_position_ = position - double(size_t(position));
    if (resample_position_ >= 1.0) {
      resample_position_ = 0.0;
    }
    out = scratch;
  } else {
    resample_position_ = 0.0;
  }

  std::unique_lock<std::mutex> guard(frames_mutex_);
  const size_t floats = out_frames * channels;
  if (ring_.empty() || ring_count_ + floats > ring_.size()) {
    // Should not happen: the semaphore limits queued guest frames and the
    // ring is sized for the maximum stretch. Count the frame anyway so the
    // producer's slot is released when it would have played.
    guest_frame_device_frames_.push_back(charged_frames);
    return;
  }
  size_t write = (ring_read_ + ring_count_) % ring_.size();
  size_t first = std::min(floats, ring_.size() - write);
  std::memcpy(ring_.data() + write, out, first * sizeof(float));
  if (floats > first) {
    std::memcpy(ring_.data(), out + first, (floats - first) * sizeof(float));
  }
  ring_count_ += floats;
  guest_frame_device_frames_.push_back(charged_frames);
}

void SDLAudioDriver::Pause() { SDL_PauseAudioDevice(sdl_device_id_, 1); }

void SDLAudioDriver::Resume() { SDL_PauseAudioDevice(sdl_device_id_, 0); }

void SDLAudioDriver::Shutdown() {
  if (sdl_device_id_ > 0) {
    SDL_CloseAudioDevice(sdl_device_id_);
    sdl_device_id_ = -1;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  ring_.clear();
  ring_read_ = 0;
  ring_count_ = 0;
  guest_frame_device_frames_.clear();
  front_consumed_device_frames_ = 0;
  stretcher_.reset();
}

void SDLAudioDriver::SDLCallback(void* userdata, Uint8* stream, int len) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    XELOGE("SDLAudioDriver::sdl_callback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const uint32_t channels = driver->sdl_device_channels_;
  float* out = reinterpret_cast<float*>(stream);
  const size_t wanted_floats = size_t(len) / sizeof(float);
  const size_t wanted_frames = wanted_floats / channels;

  std::unique_lock<std::mutex> guard(driver->frames_mutex_);
  size_t available = std::min(wanted_floats, driver->ring_count_);
  if (driver->ring_.empty()) {
    available = 0;
  }
  callback_count_.fetch_add(1, std::memory_order_relaxed);
  if (!available) {
    starved_callback_count_.fetch_add(1, std::memory_order_relaxed);
  }
  if (available) {
    size_t first = std::min(available, driver->ring_.size() - driver->ring_read_);
    std::memcpy(out, driver->ring_.data() + driver->ring_read_,
                first * sizeof(float));
    if (available > first) {
      std::memcpy(out + first, driver->ring_.data(),
                  (available - first) * sizeof(float));
    }
    driver->ring_read_ = (driver->ring_read_ + available) % driver->ring_.size();
    driver->ring_count_ -= available;
  }
  if (available < wanted_floats) {
    std::memset(out + available, 0, (wanted_floats - available) * sizeof(float));
  }

  if (cvars::mute) {
    std::memset(stream, 0, len);
  } else if (driver->volume_ != 1.0f) {
    const float volume = driver->volume_;
    for (size_t i = 0; i < available; ++i) {
      out[i] *= volume;
    }
  }

  // Release the producer's slot for every guest frame fully played.
  size_t consumed_frames = std::min(available / channels, wanted_frames);
  driver->front_consumed_device_frames_ += consumed_frames;
  while (!driver->guest_frame_device_frames_.empty() &&
         driver->front_consumed_device_frames_ >=
             driver->guest_frame_device_frames_.front()) {
    driver->front_consumed_device_frames_ -=
        driver->guest_frame_device_frames_.front();
    driver->guest_frame_device_frames_.pop_front();
    auto ret = driver->semaphore_->Release(1, nullptr);
    assert_true(ret);
  }
};
}  // namespace sdl
}  // namespace apu
}  // namespace xe
