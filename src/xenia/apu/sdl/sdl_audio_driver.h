/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
#define XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_

#include <mutex>
#include <deque>
#include <memory>
#include <queue>
#include <stack>
#include <vector>

#include "SDL.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace soundtouch {
class SoundTouch;
}

namespace xe {
namespace apu {
namespace sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(xe::threading::Semaphore* semaphore,
                 uint32_t frequency = kFrameFrequencyDefault,
                 uint32_t channels = kFrameChannelsDefault,
                 bool need_format_conversion = true);
  ~SDLAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override { volume_ = volume; };
  void Shutdown() override;

 protected:
  static void SDLCallback(void* userdata, Uint8* stream, int len);

  xe::threading::Semaphore* semaphore_ = nullptr;

  SDL_AudioDeviceID sdl_device_id_ = -1;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;

  float volume_ = 1.0f;

  uint32_t frame_frequency_;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  uint32_t frame_size_;
  bool need_format_conversion_;

  // Guest frames are converted to the device's interleaved format on
  // submit, resampled by the guest time scalar (so fast-forward and
  // slow-motion change pitch and duration like the ALSA driver), and
  // appended to a ring of device samples the callback drains. The number of
  // device frames each guest frame contributed is remembered so the
  // back-pressure semaphore is still released exactly once per guest frame.
  std::unique_ptr<soundtouch::SoundTouch> stretcher_;
  bool stretcher_active_ = false;
  double expected_frames_accumulator_ = 0.0;
  std::vector<float> convert_buffer_;
  std::vector<float> resample_buffer_;
  double resample_position_ = 0.0;
  std::vector<float> ring_;
  size_t ring_read_ = 0;   // in floats
  size_t ring_count_ = 0;  // in floats
  std::deque<size_t> guest_frame_device_frames_;
  size_t front_consumed_device_frames_ = 0;
  std::mutex frames_mutex_ = {};
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
