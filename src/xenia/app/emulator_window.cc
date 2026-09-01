/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/qrcodegen/qrcodegen.hpp"
#include "xenia/app/emulator_window.h"

#include "xenia/apu/apu_flags.h"
#include "xenia/config.h"

#include <regex>
#include <thread>
#if XE_PLATFORM_LINUX
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

#include <cctype>
#include <cfloat>
#include <ctime>
#include <map>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <chrono>

#include "third_party/imgui/imgui.h"
#include "third_party/stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "xenia/app/console_settings_dialog.h"
#include "xenia/app/content_list_dialog.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/virtual_key.h"

#include "version.h"

DECLARE_bool(debug);

DECLARE_string(hid);

DECLARE_bool(guide_button);

DECLARE_bool(clear_memory_page_state);

DECLARE_string(readback_resolve);

DECLARE_bool(readback_memexport);

DECLARE_path(content_root);
DEFINE_bool(show_fps, false,
            "Show the frame rate (guest swaps per second) in the top-left "
            "overlay. Emulation > Show FPS toggles it.",
            "UI");

// Advanced GPU options (Display menu). Defined across src/xenia/gpu.
DECLARE_bool(vsync);
DECLARE_uint64(framerate_limit);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_string(render_target_path_vulkan);
DECLARE_bool(vulkan_sparse_shared_memory);
DECLARE_bool(dirty_region_tracking);
DECLARE_bool(promote_vector_context_values);
DECLARE_int32(vulkan_pipeline_creation_threads);
DECLARE_int32(anisotropic_override);
DECLARE_string(occlusion_query);
DECLARE_double(occlusion_query_saturation);
DECLARE_bool(gpu_allow_invalid_fetch_constants);
DECLARE_bool(gpu_allow_invalid_upload_range);
DECLARE_bool(half_pixel_offset);
DECLARE_bool(async_shader_compilation);
DECLARE_bool(force_depth_clamp);
DECLARE_uint32(texture_cache_memory_limit_soft);
DECLARE_uint32(texture_cache_memory_limit_hard);
DECLARE_uint32(texture_cache_memory_limit_soft_lifetime);
DECLARE_double(ui_scale);
DECLARE_bool(apply_patches);

DEFINE_bool(fullscreen, false, "Whether to launch the emulator in fullscreen.",
            "Display");

DEFINE_bool(controller_hotkeys, false, "Hotkeys for Xbox and PS controllers.",
            "General");
DEFINE_string(ui_experiment_dialog, "",
              "Experiment: open a dialog from a timer on the UI thread: "
              "hotkeys, display, console, xmp. For reproducing UI crashes "
              "without a keyboard.",
              "General");
DEFINE_string(support_page_url, "https://peerloomllc.com/about/",
              "Help > Support Development: the page the button opens.", "UI");
DEFINE_string(support_coffee_url, "https://buymeacoffee.com/peerloomllc",
              "Help > Support Development: card tips page for the second "
              "button (empty hides it).",
              "UI");
DEFINE_string(support_btc_address, "bc1q0kksenz3j4u9ppe6f4krclvzwxk7sjy00cc9cf",
              "Help > Support Development: Bitcoin on-chain donation address "
              "(empty hides its QR code).",
              "UI");
DEFINE_string(support_lightning_address, "peerloomllc@strike.me",
              "Help > Support Development: Lightning donation address (empty "
              "hides its QR code).",
              "UI");
DEFINE_int32(screenshot_burst_seconds, 0,
             "Diagnostic: from N seconds after launch, save every new frame's "
             "guest output as a PNG (screenshot_burst_frames of them) into "
             "screenshot_burst_dir, named by swap number. For flicker analysis.",
             "UI");
DEFINE_int32(screenshot_burst_frames, 30,
             "Diagnostic: frames to save for --screenshot_burst_seconds.",
             "UI");
DEFINE_string(screenshot_burst_dir, "",
              "Diagnostic: folder for --screenshot_burst_seconds (default "
              "<exe folder>/screenshots/<title id>/burst).",
              "UI");
DEFINE_int32(ui_experiment_seconds, 30,
             "Experiment: delay before --ui_experiment_dialog opens.",
             "General");
DEFINE_string(pause_hotkey, "Space",
              "Key that pauses/resumes emulation (Emulator::Pause/Resume): "
              "F1-F24, A-Z, 0-9, Space, Delete, Insert, Home, End, PageUp, "
              "PageDown, Tab, or empty to disable.",
              "General");
DEFINE_string(mute_hotkey, "Delete",
              "Key that toggles audio mute. Same key names as pause_hotkey; "
              "empty to disable.",
              "General");
DEFINE_string(save_state_hotkey, "F8",
              "Key that saves a save state into the selected slot (nine per "
              "title, per disc). Same key names as pause_hotkey; empty to "
              "disable.",
              "General");
DEFINE_string(load_state_hotkey, "F10",
              "Key that loads the save state in the selected slot. Same key "
              "names as pause_hotkey; empty to disable.",
              "General");
DEFINE_string(save_state_dir, "",
              "Directory for save states. Empty: <storage root>/savestates.",
              "General");
DEFINE_string(library_view, "list",
              "Game library layout: list or grid.", "General");
DEFINE_string(games_dir, "",
              "Folder holding game files (.iso, .xex, .zar), scanned by "
              "File > Game Library and used as the starting folder of "
              "File > Open. Empty: none.",
              "General");
DEFINE_int32(save_state_slot, 1,
             "Current save state slot (1-9). Files are <title id>_<slot>.sav "
             "in save_state_dir.",
             "General");
DEFINE_string(next_slot_hotkey, "PageDown",
              "Key that selects the next save state slot. Same key names as "
              "pause_hotkey; empty to disable.",
              "General");
DEFINE_string(prev_slot_hotkey, "PageUp",
              "Key that selects the previous save state slot. Same key names "
              "as pause_hotkey; empty to disable.",
              "General");

namespace xe {
namespace app {
namespace {
std::optional<ui::VirtualKey> ParseHotkeyName(const std::string& name);
}  // namespace
}  // namespace app
}  // namespace xe

DEFINE_string(
    postprocess_antialiasing, "",
    "Post-processing anti-aliasing effect to apply to the image output of the "
    "game.\n"
    "Using post-process anti-aliasing is heavily recommended when AMD "
    "FidelityFX Contrast Adaptive Sharpening or Super Resolution 1.0 is "
    "active.\n"
    "Use: [none, fxaa, fxaa_extreme]\n"
    " none (or any value not listed here):\n"
    "  Don't alter the original image.\n"
    " fxaa:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, normal quality preset (12)."
    "\n"
    " fxaa_extreme:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, extreme quality preset "
    "(39).",
    "Display");
DEFINE_string(
    postprocess_scaling_and_sharpening, "",
    "Post-processing effect to use for resampling and/or sharpening of the "
    "final display output.\n"
    "Use: [bilinear, cas, fsr]\n"
    " bilinear (or any value not listed here):\n"
    "  Original image at 1:1, simple bilinear stretching for resampling.\n"
    " cas:\n"
    "  Use AMD FidelityFX Contrast Adaptive Sharpening (CAS) for sharpening "
    "at scaling factors of up to 2x2, with additional bilinear stretching for "
    "larger factors.\n"
    " fsr:\n"
    "  Use AMD FidelityFX Super Resolution 1.0 (FSR) for highest-quality "
    "upscaling, or AMD FidelityFX Contrast Adaptive Sharpening for sharpening "
    "while not scaling or downsampling.\n"
    "  For scaling by factors of more than 2x2, multiple FSR passes are done.",
    "Display");
DEFINE_double(
    postprocess_ffx_cas_additional_sharpness,
    xe::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
    "Additional sharpness for AMD FidelityFX Contrast Adaptive Sharpening "
    "(CAS), from 0 to 1.\n"
    "Higher is sharper.",
    "Display");
DEFINE_uint32(
    postprocess_ffx_fsr_max_upsampling_passes,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
    "Maximum number of upsampling passes performed in AMD FidelityFX Super "
    "Resolution 1.0 (FSR) before falling back to bilinear stretching after the "
    "final pass.\n"
    "Each pass upscales only to up to 2x2 the previous size. If the game "
    "outputs a 1280x720 image, 1 pass will upscale it to up to 2560x1440 "
    "(below 4K), after 2 passes it will be upscaled to a maximum of 5120x2880 "
    "(including 3840x2160 for 4K), and so on.\n"
    "This variable has no effect if the display resolution isn't very high, "
    "but may be reduced on resolutions like 4K or 8K in case the performance "
    "impact of multiple FSR upsampling passes is too high, or if softer edges "
    "are desired.\n"
    "The default value is the maximum internally supported by Xenia.",
    "Display");
DEFINE_double(
    postprocess_ffx_fsr_sharpness_reduction,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
    "Sharpness reduction for AMD FidelityFX Super Resolution 1.0 (FSR), in "
    "stops.\n"
    "Lower is sharper.",
    "Display");
// Dithering to 8bpc is enabled by default since the effect is minor, only
// effects what can't be shown normally by host displays, and nothing is changed
// by it for 8bpc source without resampling.
DEFINE_bool(
    postprocess_dither, true,
    "Dither the final image output from the internal precision to 8 bits per "
    "channel so gradients are smoother.\n"
    "On a 10bpc display, the lower 2 bits will still be kept, but noise will "
    "be added to them - disabling may be recommended for 10bpc, but it "
    "depends on the 10bpc displaying capabilities of the actual display used.",
    "Display");
DEFINE_double(postprocess_brightness,
              xe::ui::Presenter::GuestOutputPaintConfig::kBrightnessDefault,
              "Brightness offset added to the final image, from -1 to 1 "
              "(0 = unchanged).",
              "Display");
DEFINE_double(postprocess_contrast,
              xe::ui::Presenter::GuestOutputPaintConfig::kContrastDefault,
              "Contrast of the final image, from 0 to 2 (1 = unchanged).",
              "Display");
DEFINE_double(postprocess_saturation,
              xe::ui::Presenter::GuestOutputPaintConfig::kSaturationDefault,
              "Colour saturation of the final image, from 0 (grey) to 2 "
              "(1 = unchanged).",
              "Display");
DEFINE_double(postprocess_gamma,
              xe::ui::Presenter::GuestOutputPaintConfig::kGammaDefault,
              "Gamma of the final image, from 0.5 to 2 (1 = unchanged; above "
              "1 brightens the mid-tones).",
              "Display");

DEFINE_int32(recent_titles_entry_amount, 10,
             "Allows user to define how many titles is saved in list of "
             "recently played titles.",
             "General");
DEFINE_bool(disable_doubleclick_fullscreen, false,
            "Allows the user to disable the behavior where a fast double-click "
            "causes Xenia to enter fullscreen mode.",
            "General");

namespace xe {
namespace app {

using xe::ui::FileDropEvent;
using xe::ui::KeyEvent;
using xe::ui::MenuItem;
using xe::ui::UIEvent;

using namespace xe::hid;
using namespace xe::gpu;

constexpr std::string_view kRecentlyPlayedTitlesFilename = "recent.toml";
constexpr std::string_view kBaseTitle = "Xenia-canary";

EmulatorWindow::EmulatorWindow(Emulator* emulator,
                               ui::WindowedAppContext& app_context,
                               uint32_t width, uint32_t height)
    : emulator_(emulator),
      app_context_(app_context),
      window_listener_(*this),
      window_(ui::Window::Create(app_context, kBaseTitle, width, height)),
      imgui_drawer_(
          std::make_unique<ui::ImGuiDrawer>(window_.get(), kZOrderImGui)),
      display_config_game_config_load_callback_(
          new DisplayConfigGameConfigLoadCallback(*emulator, *this)) {
  base_title_ = std::string(kBaseTitle) +
#ifdef DEBUG
#if _NO_DEBUG_HEAP == 1
                " DEBUG"
#else
                " CHECKED"
#endif
#endif
                " ("
#ifdef XE_BUILD_IS_PR
                "PR#" XE_BUILD_PR_NUMBER " - "
#endif
                XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE
                ")";

  LoadRecentlyLaunchedTitles();
}

std::unique_ptr<EmulatorWindow> EmulatorWindow::Create(
    Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
    uint32_t height) {
  assert_true(app_context.IsInUIThread());
  std::unique_ptr<EmulatorWindow> emulator_window(
      new EmulatorWindow(emulator, app_context, width, height));
  if (!emulator_window->Initialize()) {
    return nullptr;
  }
  return emulator_window;
}

EmulatorWindow::~EmulatorWindow() {
#if XE_PLATFORM_LINUX
  AddPlayTime();
  if (!library_titles_.empty()) {
    SaveLibrary();
  }
#endif
  // Notify the ImGui drawer that the immediate drawer is being destroyed.
  ShutdownGraphicsSystemPresenterPainting();
}

ui::Presenter* EmulatorWindow::GetGraphicsSystemPresenter() const {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  return graphics_system ? graphics_system->presenter() : nullptr;
}

void EmulatorWindow::SetupGraphicsSystemPresenterPainting() {
  ShutdownGraphicsSystemPresenterPainting();

  if (!window_) {
    return;
  }

  ui::Presenter* presenter = GetGraphicsSystemPresenter();
  if (!presenter) {
    return;
  }

  ApplyDisplayConfigForCvars();

  window_->SetPresenter(presenter);

  immediate_drawer_ =
      emulator_->graphics_system()->provider()->CreateImmediateDrawer();
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(presenter);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                  immediate_drawer_.get());
    Profiler::SetUserIO(kZOrderProfiler, window_.get(), presenter,
                        immediate_drawer_.get());
  }
}

void EmulatorWindow::ShutdownGraphicsSystemPresenterPainting() {
  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);
  imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
  immediate_drawer_.reset();
  if (window_) {
    window_->SetPresenter(nullptr);
  }
}

void EmulatorWindow::OnEmulatorInitialized() {
  if (!emulator_->kernel_state()
           ->xam_state()
           ->profile_manager()
           ->GetAccountCount()) {
    new NoProfileDialog(imgui_drawer_.get(), this);
    disable_hotkeys_ = true;
  }

  emulator_initialized_ = true;
  window_->SetMainMenuEnabled(true);
  // When the user can see that the emulator isn't initializing anymore (the
  // menu isn't disabled), enter fullscreen if requested.
  if (cvars::fullscreen) {
    SetFullscreen(true);
  }

  if (IsUseNexusForGameBarEnabled()) {
    XELOGE(
        "Xbox Gamebar Enabled, using BACK button instead of GUIDE for "
        "controller hotkeys!!!");
  }

  // Create a thread to listen for controller hotkeys.
  if (cvars::controller_hotkeys) {
    Gamepad_HotKeys_Listener =
        threading::Thread::Create({}, [&] { GamepadHotKeys(); });
    Gamepad_HotKeys_Listener->set_name("Gamepad HotKeys Listener");
  }
}

void EmulatorWindow::EmulatorWindowListener::OnClosing(ui::UIEvent& e) {
#if XE_PLATFORM_LINUX
  // The process exits without destructors ("Cheap-skate exit"): book the
  // session's play time now.
  emulator_window_.AddPlayTime();
  if (!emulator_window_.library_titles_.empty()) {
    emulator_window_.SaveLibrary();
  }
#endif
  emulator_window_.app_context_.QuitFromUIThread();
}

void EmulatorWindow::EmulatorWindowListener::OnFileDrop(ui::FileDropEvent& e) {
  emulator_window_.FileDrop(e.filename());
}

void EmulatorWindow::EmulatorWindowListener::OnKeyChar(ui::KeyEvent& e) {
  emulator_window_.OnKeyChar(e);
}

void EmulatorWindow::EmulatorWindowListener::OnKeyDown(ui::KeyEvent& e) {
  emulator_window_.OnKeyDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseDown(ui::MouseEvent& e) {
  emulator_window_.OnMouseDown(e);
}

void EmulatorWindow::EmulatorWindowListener::OnMouseUp(ui::MouseEvent& e) {
  emulator_window_.OnMouseUp(e);
}

void EmulatorWindow::EmulatorWindowListener::OnUsbDeviceChanged(
    bool is_arrival) {
  if (!emulator_window_.emulator()) {
    return;
  }

  if (!emulator_window_.emulator()->input_system()) {
    return;
  }

  auto* portal = emulator_window_.emulator()->input_system()->GetPortal();
  if (!portal) {
    return;
  }

  if (is_arrival) {
    portal->OnDeviceArrival();
  } else {
    portal->OnDeviceRemoval();
  }
}

void EmulatorWindow::DisplayConfigGameConfigLoadCallback::PostGameConfigLoad() {
  emulator_window_.ApplyDisplayConfigForCvars();
}

void EmulatorWindow::DisplayConfigDialog::OnDraw(ImGuiIO& io) {
  gpu::GraphicsSystem* graphics_system =
      emulator_window_.emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  // In the top-left corner so it's close to the menu bar from where it was
  // opened.
  // Origin Y coordinate 20 was taken from the Dear ImGui demo.
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  // Alpha from Dear ImGui tooltips (0.35 from the overlay provides too low
  // visibility). Translucent so some effect of the changes can still be seen
  // through it.
  ImGui::SetNextWindowBgAlpha(0.6f);
  bool dialog_open = true;
  if (!ImGui::Begin("Post-processing", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    Close();
    return;
  }

  // Even if the close button has been pressed, still paint everything not to
  // have one frame with an empty window.

  // Prevent user confusion which has been reported multiple times.
  ImGui::TextUnformatted("All effects can be used on GPUs of any brand.");
  ImGui::Spacing();

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    if (ImGui::TreeNodeEx(
            "Anti-aliasing",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      gpu::CommandProcessor::SwapPostEffect current_swap_post_effect =
          command_processor->GetDesiredSwapPostEffect();
      int new_swap_post_effect_index = int(current_swap_post_effect);
      ImGui::RadioButton("None", &new_swap_post_effect_index,
                         int(gpu::CommandProcessor::SwapPostEffect::kNone));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Normal Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaa));
      ImGui::RadioButton(
          "NVIDIA Fast Approximate Anti-Aliasing (FXAA) [Extreme Quality]",
          &new_swap_post_effect_index,
          int(gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme));
      gpu::CommandProcessor::SwapPostEffect new_swap_post_effect =
          gpu::CommandProcessor::SwapPostEffect(new_swap_post_effect_index);
      if (current_swap_post_effect != new_swap_post_effect) {
        command_processor->SetDesiredSwapPostEffect(new_swap_post_effect);
      }

      // Override the values in the cvars to save them to the config at exit if
      // the user has set them to anything new.
      if (GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing) !=
          new_swap_post_effect) {
        OVERRIDE_string(postprocess_antialiasing,
                        GetCvarValueForSwapPostEffect(new_swap_post_effect));
      }

      ImGui::TreePop();
    }
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    const ui::Presenter::GuestOutputPaintConfig& current_presenter_config =
        presenter->GetGuestOutputPaintConfigFromUIThread();
    ui::Presenter::GuestOutputPaintConfig new_presenter_config =
        current_presenter_config;

    if (ImGui::TreeNodeEx(
            "Resampling and sharpening",
            ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
      // Filtering effect.
      int new_effect_index = int(new_presenter_config.GetEffect());
      ImGui::RadioButton(
          "None / Bilinear", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear));
      ImGui::RadioButton(
          "AMD FidelityFX Contrast Adaptive Sharpening (CAS)",
          &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kCas));
      ImGui::RadioButton(
          "AMD FidelityFX Super Resolution 1.0 (FSR)", &new_effect_index,
          int(ui::Presenter::GuestOutputPaintConfig::Effect::kFsr));
      new_presenter_config.SetEffect(
          ui::Presenter::GuestOutputPaintConfig::Effect(new_effect_index));

      // effect_description must be one complete, but short enough, sentence per
      // line, as TextWrapped doesn't work correctly in auto-resizing windows
      // (in the initial frames, the window becomes extremely tall, and widgets
      // added after the wrapped text have no effect on the width of the text).
      const char* effect_description = nullptr;
      switch (new_presenter_config.GetEffect()) {
        case ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear:
          effect_description =
              "Simple bilinear filtering is done if resampling is needed.\n"
              "Otherwise, only anti-aliasing is done if enabled, or displaying "
              "as is.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
          effect_description =
              "Sharpening and resampling to up to 2x2 to improve the fidelity "
              "of details.\n"
              "For scaling by more than 2x2, bilinear stretching is done "
              "afterwards.";
          break;
        case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
          effect_description =
              "High-quality edge-preserving upscaling to arbitrary target "
              "resolutions.\n"
              "For scaling by more than 2x2, multiple upsampling passes are "
              "done.\n"
              "If not upscaling, Contrast Adaptive Sharpening (CAS) is used "
              "instead.";
          break;
      }
      if (effect_description) {
        ImGui::TextUnformatted(effect_description);
      }

      if (new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kCas ||
          new_presenter_config.GetEffect() ==
              ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
        if (effect_description) {
          ImGui::Spacing();
        }

        ImGui::TextUnformatted(
            "FXAA is highly recommended when using CAS or FSR.");

        ImGui::Spacing();

        // 2 decimal places is more or less enough precision for the sharpness
        // given the minor visual effect of small changes, the width of the
        // slider, and readability convenience (2 decimal places is like an
        // integer percentage). However, because Dear ImGui parses the string
        // representation of the number and snaps the value to it internally,
        // 2 decimal places actually offer less precision than the slider itself
        // does. This is especially prominent in the low range of the non-linear
        // FSR sharpness reduction slider. 3 decimal places are optimal in this
        // case.

        if (new_presenter_config.GetEffect() ==
            ui::Presenter::GuestOutputPaintConfig::Effect::kFsr) {
          float fsr_sharpness_reduction =
              new_presenter_config.GetFsrSharpnessReduction();
          ImGui::TextUnformatted(
              "FSR sharpness reduction when upscaling (lower is sharper):");
          const auto label = fmt::format(
              "{} %%", static_cast<int>(fsr_sharpness_reduction * 100));
          // Power 2.0 scaling as the reduction is in stops, used in exp2.
          fsr_sharpness_reduction = sqrt(2.f * fsr_sharpness_reduction);
          ImGui::SliderFloat(
              "##FSRSharpnessReduction", &fsr_sharpness_reduction,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
              ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax,
              label.c_str(), ImGuiSliderFlags_NoInput);
          fsr_sharpness_reduction =
              .5f * fsr_sharpness_reduction * fsr_sharpness_reduction;
          ImGui::SameLine();
          if (ImGui::Button("Reset##ResetFSRSharpnessReduction")) {
            fsr_sharpness_reduction = ui::Presenter::GuestOutputPaintConfig ::
                kFsrSharpnessReductionDefault;
          }
          new_presenter_config.SetFsrSharpnessReduction(
              fsr_sharpness_reduction);
        }

        float cas_additional_sharpness =
            new_presenter_config.GetCasAdditionalSharpness();
        ImGui::TextUnformatted(
            new_presenter_config.GetEffect() ==
                    ui::Presenter::GuestOutputPaintConfig::Effect::kFsr
                ? "CAS additional sharpness when not upscaling (higher is "
                  "sharper):"
                : "CAS additional sharpness (higher is sharper):");
        const auto label = fmt::format(
            "{} %%", static_cast<int>(cas_additional_sharpness * 100));
        ImGui::SliderFloat(
            "##CASAdditionalSharpness", &cas_additional_sharpness,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
            ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax,
            label.c_str(), ImGuiSliderFlags_NoInput);
        ImGui::SameLine();
        if (ImGui::Button("Reset##ResetCASAdditionalSharpness")) {
          cas_additional_sharpness = ui::Presenter::GuestOutputPaintConfig ::
              kCasAdditionalSharpnessDefault;
        }
        new_presenter_config.SetCasAdditionalSharpness(
            cas_additional_sharpness);

        // There's no need to expose the setting for the maximum number of FSR
        // EASU passes as it's largely meaningless if the user doesn't have a
        // very high-resolution monitor compared to the original image size as
        // most of the values of the slider will have no effect, and that's just
        // very fine-grained performance control for a fixed-overhead pass only
        // for huge screen resolutions.
      }

      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Dithering", ImGuiTreeNodeFlags_Framed |
                                           ImGuiTreeNodeFlags_DefaultOpen)) {
      bool dither = current_presenter_config.GetDither();
      ImGui::Checkbox(
          "Dither the final output to 8bpc to make gradients smoother",
          &dither);
      new_presenter_config.SetDither(dither);

      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Color", ImGuiTreeNodeFlags_Framed |
                                       ImGuiTreeNodeFlags_DefaultOpen)) {
      using PaintConfig = ui::Presenter::GuestOutputPaintConfig;
      struct ColorSlider {
        const char* label;
        float value, min, max, def;
      };
      ColorSlider sliders[] = {
          {"Brightness", new_presenter_config.GetBrightness(),
           PaintConfig::kBrightnessMin, PaintConfig::kBrightnessMax,
           PaintConfig::kBrightnessDefault},
          {"Contrast", new_presenter_config.GetContrast(),
           PaintConfig::kContrastMin, PaintConfig::kContrastMax,
           PaintConfig::kContrastDefault},
          {"Saturation", new_presenter_config.GetSaturation(),
           PaintConfig::kSaturationMin, PaintConfig::kSaturationMax,
           PaintConfig::kSaturationDefault},
          {"Gamma", new_presenter_config.GetGamma(), PaintConfig::kGammaMin,
           PaintConfig::kGammaMax, PaintConfig::kGammaDefault},
      };
      for (ColorSlider& slider : sliders) {
        ImGui::TextUnformatted(slider.label);
        const auto id = fmt::format("##Color{}", slider.label);
        ImGui::SliderFloat(id.c_str(), &slider.value, slider.min, slider.max,
                           "%.2f", ImGuiSliderFlags_NoInput);
        ImGui::SameLine();
        const auto reset_id = fmt::format("Reset##ResetColor{}", slider.label);
        if (ImGui::Button(reset_id.c_str())) {
          slider.value = slider.def;
        }
      }
      new_presenter_config.SetBrightness(sliders[0].value);
      new_presenter_config.SetContrast(sliders[1].value);
      new_presenter_config.SetSaturation(sliders[2].value);
      new_presenter_config.SetGamma(sliders[3].value);
      ImGui::TreePop();
    }

    presenter->SetGuestOutputPaintConfigFromUIThread(new_presenter_config);

    // Override the values in the cvars to save them to the config at exit if
    // the user has set them to anything new.
    ui::Presenter::GuestOutputPaintConfig cvars_presenter_config =
        GetGuestOutputPaintConfigForCvars();
    if (cvars_presenter_config.GetEffect() !=
        new_presenter_config.GetEffect()) {
      OVERRIDE_string(postprocess_scaling_and_sharpening,
                      GetCvarValueForGuestOutputPaintEffect(
                          new_presenter_config.GetEffect()));
    }
    if (cvars_presenter_config.GetCasAdditionalSharpness() !=
        new_presenter_config.GetCasAdditionalSharpness()) {
      OVERRIDE_double(postprocess_ffx_cas_additional_sharpness,
                      new_presenter_config.GetCasAdditionalSharpness());
    }
    if (cvars_presenter_config.GetFsrSharpnessReduction() !=
        new_presenter_config.GetFsrSharpnessReduction()) {
      OVERRIDE_double(postprocess_ffx_fsr_sharpness_reduction,
                      new_presenter_config.GetFsrSharpnessReduction());
    }
    if (cvars_presenter_config.GetDither() !=
        new_presenter_config.GetDither()) {
      OVERRIDE_bool(postprocess_dither, new_presenter_config.GetDither());
    }
    if (cvars_presenter_config.GetBrightness() !=
        new_presenter_config.GetBrightness()) {
      OVERRIDE_double(postprocess_brightness,
                      new_presenter_config.GetBrightness());
    }
    if (cvars_presenter_config.GetContrast() !=
        new_presenter_config.GetContrast()) {
      OVERRIDE_double(postprocess_contrast, new_presenter_config.GetContrast());
    }
    if (cvars_presenter_config.GetSaturation() !=
        new_presenter_config.GetSaturation()) {
      OVERRIDE_double(postprocess_saturation,
                      new_presenter_config.GetSaturation());
    }
    if (cvars_presenter_config.GetGamma() != new_presenter_config.GetGamma()) {
      OVERRIDE_double(postprocess_gamma, new_presenter_config.GetGamma());
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.ToggleDisplayConfigDialog();
    // `this` might have been destroyed by ToggleDisplayConfigDialog.
    return;
  }
}

void EmulatorWindow::ContentInstallDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin(
          fmt::format("Installation Progress###{}", window_id_).c_str(),
          &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  bool is_everything_installed = true;
  for (const auto& entry : *installation_entries_) {
    ImGui::BeginTable(fmt::format("table_{}", entry.name_).c_str(), 2);
    ImGui::TableNextRow(0);
    ImGui::TableSetColumnIndex(0);
    if (entry.icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon_.get()),
                   ui::default_image_icon_size);
    } else {
      ImGui::Dummy(ui::default_image_icon_size);
    }
    ImGui::TableNextColumn();

    ImGui::Text("Name: %s", entry.name_.c_str());
    ImGui::Text("Installation Path:");
    ImGui::SameLine();
    if (ImGui::TextLink(
            xe::path_to_utf8(entry.data_installation_path_).c_str())) {
      LaunchFileExplorer(emulator_window_.emulator_->content_root() /
                         entry.data_installation_path_);
    }

    if (entry.content_type_ != xe::XContentType::kInvalid) {
      ImGui::Text("Content Type: %s",
                  XContentTypeMap.at(entry.content_type_).c_str());
    }

    std::string result = fmt::format(
        "Status: {}", xe::Emulator::installStateStringName[static_cast<uint8_t>(
                          entry.installation_state_)]);

    if (entry.installation_state_ == xe::Emulator::InstallState::failed) {
      result += fmt::format(" - {} ({:08X})",
                            entry.installation_error_message_.c_str(),
                            entry.installation_result_);
    }

    ImGui::Text("%s", result.c_str());
    ImGui::EndTable();

    if (entry.content_size_ > 0) {
      ImGui::ProgressBar(static_cast<float>(entry.currently_installed_size_) /
                         entry.content_size_);

      if (entry.installation_state_ == Emulator::InstallState::installing ||
          entry.installation_state_ == Emulator::InstallState::pending ||
          entry.installation_state_ == Emulator::InstallState::preparing) {
        is_everything_installed = false;
      }
    } else {
      ImGui::ProgressBar(0.0f);
    }

    if (installation_entries_->size() > 1) {
      ImGui::Separator();
    }
  }
  ImGui::Spacing();

  ImGui::BeginDisabled(!is_everything_installed);
  if (ImGui::Button("Close")) {
    ImGui::EndDisabled();
    Close();
    ImGui::End();
    return;
  }
  ImGui::EndDisabled();

  if (!dialog_open && is_everything_installed) {
    Close();
    ImGui::End();
    return;
  }
  ImGui::End();
}

void EmulatorWindow::XMPConfigDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  bool dialog_open = true;
  if (!ImGui::Begin("Audio Player Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    ImGui::End();
    return;
  }

  auto audio_player = emulator_window_.emulator_->audio_media_player();
  using xmp_state = kernel::xam::apps::XmpApp::State;
  if (audio_player) {
    ImGui::Text("Audio player status:");
    ImGui::SameLine();
    switch (audio_player->GetState()) {
      case xmp_state::kIdle:
        ImGui::Text("Idle");
        break;
      case xmp_state::kPaused:
        ImGui::Text("Paused");
        break;
      case xmp_state::kPlaying:
        ImGui::Text("Playing");
        break;
      default:
        break;
    }

    if (audio_player->IsPlaying()) {
      if (ImGui::Button("Pause")) {
        audio_player->Pause();
      }
    } else if (audio_player->IsPaused()) {
      if (ImGui::Button("Resume")) {
        audio_player->Continue();
      }
    }

    volume_ =
        emulator_window_.emulator_->audio_media_player()->GetVolume()->load();

    if (ImGui::SliderFloat("Audio player volume", &volume_, 0.0f, 1.0f,
                           "%.2f")) {
      audio_player->SetVolume(volume_);
    }
  }

  ImGui::End();

  if (!dialog_open) {
    Close();
    emulator_window_.xmp_config_dialog_.release();
    return;
  }
}

bool EmulatorWindow::Initialize() {
  window_->AddListener(&window_listener_);
  UpdateStatusOverlay(nullptr);
  emulator_->on_pause_state_changed.AddListener([this](bool paused) {
    app_context().CallInUIThread([this, paused]() { SetPausedOverlay(paused); });
  });
  window_->AddInputListener(&window_listener_, kZOrderEmulatorWindowInput);

  // Main menu.
  // FIXME: This code is really messy.
  // Hotkeys shown next to the menu items come from the config.
  action_keys_[int(HotkeyAction::kPauseResume)] =
      ParseHotkeyName(cvars::pause_hotkey);
  action_keys_[int(HotkeyAction::kMute)] = ParseHotkeyName(cvars::mute_hotkey);
  action_keys_[int(HotkeyAction::kSaveState)] =
      ParseHotkeyName(cvars::save_state_hotkey);
  action_keys_[int(HotkeyAction::kLoadState)] =
      ParseHotkeyName(cvars::load_state_hotkey);
  action_keys_[int(HotkeyAction::kNextSlot)] =
      ParseHotkeyName(cvars::next_slot_hotkey);
  action_keys_[int(HotkeyAction::kPrevSlot)] =
      ParseHotkeyName(cvars::prev_slot_hotkey);
  auto hotkey_of = [this](HotkeyAction action, const std::string& name) {
    return action_key(action).has_value() ? name : std::string();
  };

  // Five menus: File, Emulation, Settings, Tools, Help. Everything that is
  // a setting lives in the Settings window (GTK) or, elsewhere, in the
  // ImGui dialogs it wraps.
  auto main_menu = MenuItem::Create(MenuItem::Type::kNormal);

  // File: what to run and what to install.
  auto file_menu = MenuItem::Create(MenuItem::Type::kPopup, "&File");
  auto recent_menu = MenuItem::Create(MenuItem::Type::kPopup, "Open &Recent");
  auto zar_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Zar Package");
  FillRecentlyLaunchedTitlesMenu(recent_menu.get());
  {
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Open...", "Ctrl+O",
                         std::bind(&EmulatorWindow::FileOpen, this)));
    file_menu->AddChild(std::move(recent_menu));
#if XE_PLATFORM_LINUX
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Game &Library", "",
        std::bind(&EmulatorWindow::ToggleDashboard, this)));
#else
    file_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Game &Library...", "",
        std::bind(&EmulatorWindow::ToggleGameLibraryDialog, this)));
#endif
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Reset Game", "",
                         std::bind(&EmulatorWindow::ResetGame, this)));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Close Game", "",
                         std::bind(&EmulatorWindow::CloseGame, this)));
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Install Content...",
                         std::bind(&EmulatorWindow::InstallContent, this)));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Extract Content...",
                         std::bind(&EmulatorWindow::ExtractContent, this, "")));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Create",
                         std::bind(&EmulatorWindow::CreateZarchive, this)));
    zar_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Extract",
                         std::bind(&EmulatorWindow::ExtractZarchive, this)));
    file_menu->AddChild(std::move(zar_menu));
#ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Close",
                         std::bind(&EmulatorWindow::FileClose, this)));
#endif  // #ifdef DEBUG
    file_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    file_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "E&xit", "Alt+F4",
                         [this]() { window_->RequestClose(); }));
  }
  main_menu->AddChild(std::move(file_menu));

  // Emulation: what you do while a title runs.
  auto emulation_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Emulation");
  {
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Pause/Resume",
        hotkey_of(HotkeyAction::kPauseResume, cvars::pause_hotkey),
        std::bind(&EmulatorWindow::TogglePauseEmulation, this)));
    emulation_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Normal Speed", "Numpad *",
        std::bind(&EmulatorWindow::CpuTimeScalarReset, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Fast Forward (speed x2)", "Numpad +",
        std::bind(&EmulatorWindow::CpuTimeScalarSetDouble, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Slow &Motion (speed /2)", "Numpad -",
        std::bind(&EmulatorWindow::CpuTimeScalarSetHalf, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show FPS &overlay", "",
        std::bind(&EmulatorWindow::ToggleFpsOverlay, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Mute",
        hotkey_of(HotkeyAction::kMute, cvars::mute_hotkey),
        std::bind(&EmulatorWindow::ToggleMute, this)));
    emulation_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Save State",
        hotkey_of(HotkeyAction::kSaveState, cvars::save_state_hotkey),
        std::bind(&EmulatorWindow::SaveState, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Load State",
        hotkey_of(HotkeyAction::kLoadState, cvars::load_state_hotkey),
        std::bind(&EmulatorWindow::LoadState, this)));
    emulation_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Save State S&lots...", "",
        std::bind(&EmulatorWindow::ToggleSaveStatesDialog, this)));
    emulation_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    emulation_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "F&ullscreen", "F11",
                         std::bind(&EmulatorWindow::ToggleFullscreen, this)));
    emulation_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Take Screenshot", "F12",
                         std::bind(&EmulatorWindow::TakeScreenshot, this)));
  }
  main_menu->AddChild(std::move(emulation_menu));

  // Settings: one window; post-processing stays direct since it is
  // adjusted while watching the picture.
  auto settings_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Settings");
  {
#if XE_PLATFORM_LINUX
    settings_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Preferences...", "",
        std::bind(&EmulatorWindow::ToggleSettingsWindow, this)));
    // The ImGui panels drawn inside the game view: the same settings as
    // the Preferences window, reachable without leaving the game.
    auto panels = MenuItem::Create(MenuItem::Type::kPopup, "&In-game panels");
#else
    auto& panels = settings_menu;
#endif
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Post-processing", "F6",
        std::bind(&EmulatorWindow::ToggleDisplayConfigDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Advanced &GPU options...", "",
        std::bind(&EmulatorWindow::ToggleGpuOptionsDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Keyboard hotkeys...", "",
        std::bind(&EmulatorWindow::ToggleKeyboardHotkeysDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Content folder...", "",
        std::bind(&EmulatorWindow::ToggleContentFolderDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "P&rofiles...", "",
        std::bind(&EmulatorWindow::ToggleProfilesConfigDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Media player (XMP)...", "",
        std::bind(&EmulatorWindow::ToggleXMPConfigDialog, this)));
    panels->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "C&onsole settings...", "",
        std::bind(&EmulatorWindow::ToggleConsoleSettingsDialog, this)));
    panels->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    auto size_menu = MenuItem::Create(MenuItem::Type::kPopup, "Panel &size");
    for (auto [label, scale] : {std::pair{"Normal", 1.0f},
                                std::pair{"Large (1.25x)", 1.25f},
                                std::pair{"Larger (1.5x)", 1.5f},
                                std::pair{"Largest (2x)", 2.0f}}) {
      size_menu->AddChild(MenuItem::Create(
          MenuItem::Type::kString, label, "",
          std::bind(&EmulatorWindow::SetUIScale, this, scale)));
    }
    panels->AddChild(std::move(size_menu));
#if XE_PLATFORM_LINUX
    settings_menu->AddChild(std::move(panels));
#endif
  }
  main_menu->AddChild(std::move(settings_menu));

  // Tools: content lists and diagnostics.
  auto tools_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Tools");
  {
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Installed Content...", "",
        std::bind(&EmulatorWindow::ToggleContentListDialog, this)));
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Content by &Title...", "",
        std::bind(&EmulatorWindow::ToggleContentFolderDialog, this)));
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Show content &directory...",
        std::bind(&EmulatorWindow::ShowContentDirectory, this)));
    tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    tools_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "GPU Trace &Frame", "F4",
                         std::bind(&EmulatorWindow::GpuTraceFrame, this)));
    tools_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "&Clear Runtime Caches", "F5",
                         std::bind(&EmulatorWindow::GpuClearCaches, this)));
    tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Break and Show Guest Debugger",
        "Pause/Break", std::bind(&EmulatorWindow::CpuBreakIntoDebugger, this)));
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Break into &Host Debugger",
        "Ctrl+Pause/Break",
        std::bind(&EmulatorWindow::CpuBreakIntoHostDebugger, this)));
#if XE_OPTION_PROFILING
  tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
  {
    tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "Toggle Profiler &Display", "F3",
                                        []() { Profiler::ToggleDisplay(); }));
    tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kString,
                                        "&Pause/Resume Profiler", "`",
                                        []() { Profiler::TogglePause(); }));
  }
#endif
    tools_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    tools_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Controller hot&keys (overlay)", "",
        std::bind(&EmulatorWindow::DisplayHotKeysConfig, this)));
  }
  main_menu->AddChild(std::move(tools_menu));

  auto help_menu = MenuItem::Create(MenuItem::Type::kPopup, "&Help");
  {
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "FA&Q...", "F1",
                         std::bind(&EmulatorWindow::ShowFAQ, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(
        MenuItem::Create(MenuItem::Type::kString, "Game &compatibility...",
                         std::bind(&EmulatorWindow::ShowCompatibility, this)));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Build commit on GitHub...", "F2",
        std::bind(&EmulatorWindow::ShowBuildCommit, this)));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "Recent changes on GitHub...", []() {
          // This fork's commits live on the fork, not upstream.
          LaunchWebBrowser(
              "https://github.com/peerloomllc/xenia-canary/"
              "compare/" XE_BUILD_COMMIT "..." XE_BUILD_BRANCH);
        }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&About...",
        []() { LaunchWebBrowser("https://xenia.jp/about/"); }));
    help_menu->AddChild(MenuItem::Create(MenuItem::Type::kSeparator));
    help_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, "&Support development...",
        std::bind(&EmulatorWindow::ToggleSupportDialog, this)));
  }
  main_menu->AddChild(std::move(help_menu));

  window_->SetMainMenu(std::move(main_menu));

  if (cvars::screenshot_burst_seconds > 0) {
    std::thread([this]() {
      xe::threading::set_name("Screenshot Burst");
      std::this_thread::sleep_for(
          std::chrono::seconds(cvars::screenshot_burst_seconds));
      auto* gs = emulator()->graphics_system();
      if (!gs || !gs->command_processor()) {
        XELOGE("SCREENSHOT BURST: no graphics system");
        return;
      }
      std::filesystem::path dir = cvars::screenshot_burst_dir;
      if (dir.empty()) {
        dir = xe::filesystem::GetExecutableFolder() / "screenshots" /
              fmt::format("{:08X}", emulator()->title_id()) / "burst";
      }
      std::filesystem::create_directories(dir);
      uint64_t last = gs->command_processor()->swap_count();
      int saved = 0;
      auto t0 = std::chrono::steady_clock::now();
      // Readback per frame, PNG encoding after the burst: encoding a 2x
      // frame takes longer than a frame, and the frames must be consecutive.
      std::vector<std::pair<uint64_t, xe::ui::RawImage>> captured;
      captured.reserve(cvars::screenshot_burst_frames);
      while (saved < cvars::screenshot_burst_frames) {
        uint64_t now = gs->command_processor()->swap_count();
        if (now == last) {
          std::this_thread::sleep_for(std::chrono::microseconds(500));
          if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) {
            XELOGW("SCREENSHOT BURST: gave up after 30 s, {} frames saved",
                   saved);
            return;
          }
          continue;
        }
        last = now;
        app_context().CallInUIThreadSynchronous([this, &captured, now]() {
          xe::ui::RawImage image;
          auto* presenter = GetGraphicsSystemPresenter();
          if (presenter && presenter->CaptureGuestOutput(image)) {
            captured.emplace_back(now, std::move(image));
          }
        });
        ++saved;
      }
      for (auto& [swap, image] : captured) {
        SaveImage(dir / fmt::format("frame_{:06}.png", swap), image);
      }
      XELOGI("SCREENSHOT BURST: {} frames saved to {} (swaps {}..{})",
             captured.size(), dir.string(),
             captured.empty() ? 0 : captured.front().first,
             captured.empty() ? 0 : captured.back().first);
    }).detach();
  }

  if (!cvars::ui_experiment_dialog.empty()) {
    std::thread([this]() {
      xe::threading::set_name("UI Experiment");
      std::this_thread::sleep_for(
          std::chrono::seconds(cvars::ui_experiment_seconds));
      app_context().CallInUIThread([this]() {
        const std::string& which = cvars::ui_experiment_dialog;
        XELOGI("UI EXPERIMENT: opening '{}'", which);
        if (which == "hotkeys") {
          DisplayHotKeysConfig();
        } else if (which == "display") {
          ToggleDisplayConfigDialog();
        } else if (which == "console") {
          ToggleConsoleSettingsDialog();
        } else if (which == "xmp") {
          ToggleXMPConfigDialog();
        } else if (which == "keyboard") {
          ToggleKeyboardHotkeysDialog();
        } else if (which == "savestates") {
          ToggleSaveStatesDialog();
        } else if (which == "library") {
          ToggleGameLibraryDialog();
        } else if (which == "content") {
          ToggleContentFolderDialog();
        } else if (which == "gpu") {
          ToggleGpuOptionsDialog();
        } else if (which == "settings") {
#if XE_PLATFORM_LINUX
          ToggleSettingsWindow();
#endif
        } else if (which == "large") {
          SetUIScale(1.5f);
          ToggleGpuOptionsDialog();
        } else if (which == "reset") {
          ResetGame();
        } else if (which == "dashboard") {
#if XE_PLATFORM_LINUX
          ToggleDashboard();
#endif
        } else if (which.rfind("open:", 0) == 0) {
          RunTitle(which.substr(5));
        } else if (which == "support") {
          ToggleSupportDialog();
        } else if (which == "keyboard_capture") {
          ToggleKeyboardHotkeysDialog();
          capturing_action_ = int(HotkeyAction::kPauseResume);
        }
        XELOGI("UI EXPERIMENT: '{}' opened", which);
      });
    }).detach();
  }

  window_->SetMainMenuEnabled(false);

  UpdateTitle();

  if (!window_->Open()) {
    XELOGE("Failed to open the platform window");
    return false;
  }
#if XE_PLATFORM_LINUX
  LoadLibrary();
  ScanLibrary();
  BuildDashboard();
  ShowDashboard(!emulator_->is_title_open());
#endif
  // The status overlay (speed, mute, FPS) is an ImGui dialog; one made
  // before a title runs is not attached to the presenter, so (re)create it
  // when a title starts.
  emulator_->on_launch.AddListener([this](uint32_t, const std::string_view) {
    app_context().CallInUIThread([this]() {
      status_overlay_.reset();
      UpdateStatusOverlay(nullptr);
    });
  });

  Profiler::SetUserIO(kZOrderProfiler, window_.get(), nullptr, nullptr);

  return true;
}

const char* EmulatorWindow::GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  switch (effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      return "fxaa";
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      return "fxaa_extreme";
    default:
      return "";
  }
}

gpu::CommandProcessor::SwapPostEffect
EmulatorWindow::GetSwapPostEffectForCvarValue(const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaa)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* EmulatorWindow::GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  switch (effect) {
    case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
      return "cas";
    case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
      return "fsr";
    default:
      return "";
  }
}

ui::Presenter::GuestOutputPaintConfig::Effect
EmulatorWindow::GetGuestOutputPaintEffectForCvarValue(
    const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kCas)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kCas;
  }
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kFsr)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kFsr;
  }
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig
EmulatorWindow::GetGuestOutputPaintConfigForCvars() {
  ui::Presenter::GuestOutputPaintConfig paint_config;
  paint_config.SetAllowOverscanCutoff(true);
  paint_config.SetEffect(GetGuestOutputPaintEffectForCvarValue(
      cvars::postprocess_scaling_and_sharpening));
  paint_config.SetCasAdditionalSharpness(
      float(cvars::postprocess_ffx_cas_additional_sharpness));
  paint_config.SetFsrMaxUpsamplingPasses(
      cvars::postprocess_ffx_fsr_max_upsampling_passes);
  paint_config.SetFsrSharpnessReduction(
      float(cvars::postprocess_ffx_fsr_sharpness_reduction));
  paint_config.SetDither(cvars::postprocess_dither);
  paint_config.SetBrightness(float(cvars::postprocess_brightness));
  paint_config.SetContrast(float(cvars::postprocess_contrast));
  paint_config.SetSaturation(float(cvars::postprocess_saturation));
  paint_config.SetGamma(float(cvars::postprocess_gamma));
  return paint_config;
}

void EmulatorWindow::ApplyDisplayConfigForCvars() {
  gpu::GraphicsSystem* graphics_system = emulator_->graphics_system();
  if (!graphics_system) {
    return;
  }

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    command_processor->SetDesiredSwapPostEffect(
        GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing));
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    presenter->SetGuestOutputPaintConfigFromUIThread(
        GetGuestOutputPaintConfigForCvars());
  }
}

namespace {
std::optional<ui::VirtualKey> ParseHotkeyName(const std::string& name) {
  if (name.empty()) {
    return std::nullopt;
  }
  std::string n = name;
  for (auto& c : n) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (n.size() >= 2 && n[0] == 'F') {
    int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) {
      return ui::VirtualKey(uint16_t(ui::VirtualKey::kF1) + (f - 1));
    }
  }
  if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') {
    return ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + (n[0] - 'A'));
  }
  if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') {
    return ui::VirtualKey(0x30 + (n[0] - '0'));
  }
  static const std::pair<const char*, ui::VirtualKey> kNamed[] = {
      {"PAUSE", ui::VirtualKey::kPause},   {"SPACE", ui::VirtualKey::kSpace},
      {"DELETE", ui::VirtualKey::kDelete}, {"INSERT", ui::VirtualKey::kInsert},
      {"HOME", ui::VirtualKey::kHome},     {"END", ui::VirtualKey::kEnd},
      {"PAGEUP", ui::VirtualKey::kPrior},  {"PAGEDOWN", ui::VirtualKey::kNext},
      {"TAB", ui::VirtualKey::kTab},
  };
  for (const auto& [key_name, key] : kNamed) {
    if (n == key_name) {
      return key;
    }
  }
  XELOGW("Unrecognised hotkey name '{}', hotkey disabled", name);
  return std::nullopt;
}
}  // namespace

namespace {
// Keyboard shortcuts handled in EmulatorWindow::OnKeyDown, for the dialog
// and for conflict checks. Keep in step with OnKeyDown.
struct FixedHotkey {
  ui::VirtualKey key;
  const char* name;
  const char* action;
};
const FixedHotkey kFixedHotkeys[] = {
    {ui::VirtualKey::kO, "Ctrl+O", "Open..."},
    {ui::VirtualKey::kF1, "F1", "Show FAQ"},
    {ui::VirtualKey::kF2, "F2", "Show build commit"},
    {ui::VirtualKey::kF3, "F3", "Toggle profiler display"},
    {ui::VirtualKey::kF4, "F4", "GPU: trace frame"},
    {ui::VirtualKey::kF5, "F5", "GPU: clear runtime caches"},
    {ui::VirtualKey::kF6, "F6", "Post-processing settings"},
    {ui::VirtualKey::kF9, "F9", "Run previously played title"},
    {ui::VirtualKey::kF11, "F11", "Toggle fullscreen"},
    {ui::VirtualKey::kF12, "F12", "Take screenshot"},
    {ui::VirtualKey::kEscape, "Escape", "Leave fullscreen"},
    {ui::VirtualKey::kPause, "Pause/Break", "Break into guest debugger"},
    {ui::VirtualKey::kCancel, "Ctrl+Pause/Break", "Break into host debugger"},
    {ui::VirtualKey::kMultiply, "Numpad *", "Reset time scalar"},
    {ui::VirtualKey::kSubtract, "Numpad -", "Time scalar /= 2"},
    {ui::VirtualKey::kAdd, "Numpad +", "Time scalar *= 2"},
};

// Inverse of ParseHotkeyName for the keys it accepts; empty otherwise.
std::string HotkeyName(ui::VirtualKey key) {
  uint16_t v = uint16_t(key);
  if (v >= uint16_t(ui::VirtualKey::kF1) &&
      v <= uint16_t(ui::VirtualKey::kF24)) {
    return "F" + std::to_string(v - uint16_t(ui::VirtualKey::kF1) + 1);
  }
  if (v >= uint16_t(ui::VirtualKey::kA) && v <= uint16_t(ui::VirtualKey::kZ)) {
    return std::string(1, char('A' + (v - uint16_t(ui::VirtualKey::kA))));
  }
  if (v >= uint16_t(ui::VirtualKey::k0) && v <= uint16_t(ui::VirtualKey::k9)) {
    return std::string(1, char('0' + (v - uint16_t(ui::VirtualKey::k0))));
  }
  switch (key) {
    case ui::VirtualKey::kSpace:
      return "Space";
    case ui::VirtualKey::kDelete:
      return "Delete";
    case ui::VirtualKey::kInsert:
      return "Insert";
    case ui::VirtualKey::kHome:
      return "Home";
    case ui::VirtualKey::kEnd:
      return "End";
    case ui::VirtualKey::kPrior:
      return "PageUp";
    case ui::VirtualKey::kNext:
      return "PageDown";
    case ui::VirtualKey::kTab:
      return "Tab";
    default:
      return "";
  }
}

const FixedHotkey* FindFixedHotkey(ui::VirtualKey key) {
  for (const auto& h : kFixedHotkeys) {
    if (h.key == key) {
      return &h;
    }
  }
  return nullptr;
}

// Keys offered in the dialog's dropdown: everything ParseHotkeyName accepts
// except the fixed hotkeys.
std::vector<std::string> AssignableHotkeyChoices() {
  std::vector<std::string> out;
  auto add = [&](ui::VirtualKey k) {
    if (!FindFixedHotkey(k)) {
      out.push_back(HotkeyName(k));
    }
  };
  for (int i = 0; i < 24; ++i) {
    add(ui::VirtualKey(uint16_t(ui::VirtualKey::kF1) + i));
  }
  for (int i = 0; i < 26; ++i) {
    add(ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + i));
  }
  for (int i = 0; i < 10; ++i) {
    add(ui::VirtualKey(uint16_t(ui::VirtualKey::k0) + i));
  }
  for (ui::VirtualKey k :
       {ui::VirtualKey::kSpace, ui::VirtualKey::kDelete, ui::VirtualKey::kInsert,
        ui::VirtualKey::kHome, ui::VirtualKey::kEnd, ui::VirtualKey::kPrior,
        ui::VirtualKey::kNext, ui::VirtualKey::kTab}) {
    add(k);
  }
  return out;
}

const char* HotkeyActionLabel(EmulatorWindow::HotkeyAction action) {
  switch (action) {
    case EmulatorWindow::HotkeyAction::kPauseResume:
      return "Pause/Resume emulation";
    case EmulatorWindow::HotkeyAction::kMute:
      return "Mute/unmute audio";
    case EmulatorWindow::HotkeyAction::kSaveState:
      return "Save state";
    case EmulatorWindow::HotkeyAction::kLoadState:
      return "Load state";
    case EmulatorWindow::HotkeyAction::kNextSlot:
      return "Next save state slot";
    case EmulatorWindow::HotkeyAction::kPrevSlot:
      return "Previous save state slot";
    default:
      return "?";
  }
}
}  // namespace

bool EmulatorWindow::SetActionHotkey(HotkeyAction action,
                                     ui::VirtualKey key) {
  std::string name = HotkeyName(key);
  if (name.empty()) {
    hotkey_status_ =
        "That key cannot be used; pick F1-F24, A-Z, 0-9, Space, Delete, "
        "Insert, Home, End, PageUp, PageDown or Tab.";
    return false;
  }
  if (const FixedHotkey* fixed = FindFixedHotkey(key)) {
    hotkey_status_ = fmt::format("{} is already used for \"{}\".",
                                 fixed->name, fixed->action);
    return false;
  }
  for (int i = 0; i < int(HotkeyAction::kCount); ++i) {
    if (i != int(action) && action_keys_[i].has_value() &&
        *action_keys_[i] == key) {
      hotkey_status_ = fmt::format("{} is already used for \"{}\".", name,
                                   HotkeyActionLabel(HotkeyAction(i)));
      return false;
    }
  }
  action_keys_[int(action)] = key;
  switch (action) {
    case HotkeyAction::kPauseResume:
      OVERRIDE_string(pause_hotkey, name);
      break;
    case HotkeyAction::kMute:
      OVERRIDE_string(mute_hotkey, name);
      break;
    case HotkeyAction::kSaveState:
      OVERRIDE_string(save_state_hotkey, name);
      break;
    case HotkeyAction::kLoadState:
      OVERRIDE_string(load_state_hotkey, name);
      break;
    case HotkeyAction::kNextSlot:
      OVERRIDE_string(next_slot_hotkey, name);
      break;
    case HotkeyAction::kPrevSlot:
      OVERRIDE_string(prev_slot_hotkey, name);
      break;
    default:
      break;
  }
  config::SaveConfig();
  hotkey_status_ =
      fmt::format("{} is now {}.", HotkeyActionLabel(action), name);
  XELOGI("Hotkey: {} set to {}", HotkeyActionLabel(action), name);
  return true;
}

void EmulatorWindow::ClearActionHotkey(HotkeyAction action) {
  action_keys_[int(action)].reset();
  switch (action) {
    case HotkeyAction::kPauseResume:
      OVERRIDE_string(pause_hotkey, "");
      break;
    case HotkeyAction::kMute:
      OVERRIDE_string(mute_hotkey, "");
      break;
    case HotkeyAction::kSaveState:
      OVERRIDE_string(save_state_hotkey, "");
      break;
    case HotkeyAction::kLoadState:
      OVERRIDE_string(load_state_hotkey, "");
      break;
    case HotkeyAction::kNextSlot:
      OVERRIDE_string(next_slot_hotkey, "");
      break;
    case HotkeyAction::kPrevSlot:
      OVERRIDE_string(prev_slot_hotkey, "");
      break;
    default:
      break;
  }
  config::SaveConfig();
  hotkey_status_ =
      fmt::format("{} hotkey disabled.", HotkeyActionLabel(action));
  XELOGI("Hotkey: {} disabled", HotkeyActionLabel(action));
}

void EmulatorWindow::ToggleKeyboardHotkeysDialog() {
  if (!keyboard_hotkeys_dialog_) {
    hotkey_status_.clear();
    keyboard_hotkeys_dialog_ =
        std::make_unique<KeyboardHotkeysDialog>(imgui_drawer_.get(), *this);
  } else {
    capturing_action_ = -1;
    if (keyboard_hotkeys_dialog_->IsClosing()) {
      keyboard_hotkeys_dialog_.release();
    } else {
      keyboard_hotkeys_dialog_.reset();
    }
  }
}

void EmulatorWindow::KeyboardHotkeysDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  bool dialog_open = true;
  if (!ImGui::Begin(
          "Keyboard hotkeys", &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    // The owner must drop its pointer before Draw() deletes this, or the
    // next toggle uses freed memory (the UI thread then faults forever).
    emulator_window_.ToggleKeyboardHotkeysDialog();
    return;
  }

  EmulatorWindow& w = emulator_window_;
  static std::vector<std::string> choices = AssignableHotkeyChoices();

  ImGui::TextUnformatted("Assignable hotkeys");
  ImGui::Separator();
  for (int a = 0; a < int(HotkeyAction::kCount); ++a) {
    HotkeyAction action = HotkeyAction(a);
    ImGui::PushID(a);
    std::string current = w.action_keys_[a].has_value()
                              ? HotkeyName(*w.action_keys_[a])
                              : "Disabled";
    ImGui::Text("%s", HotkeyActionLabel(action));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::BeginCombo("##key", current.c_str())) {
      for (const auto& choice : choices) {
        bool selected = choice == current;
        if (ImGui::Selectable(choice.c_str(), selected)) {
          w.capturing_action_ = -1;
          w.SetActionHotkey(action, *ParseHotkeyName(choice));
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (w.capturing_action_ == a) {
      if (ImGui::Button("Cancel")) {
        w.capturing_action_ = -1;
        w.hotkey_status_.clear();
      }
      ImGui::SameLine();
      ImGui::TextUnformatted("Press the new key now...");
    } else {
      if (ImGui::Button("Press a key...")) {
        w.capturing_action_ = a;
        w.hotkey_status_.clear();
      }
      ImGui::SameLine();
      if (ImGui::Button("Disable")) {
        w.ClearActionHotkey(action);
      }
    }
    ImGui::PopID();
  }
  if (!w.hotkey_status_.empty()) {
    ImGui::TextUnformatted(w.hotkey_status_.c_str());
  }
  ImGui::TextUnformatted(
      "Changes take effect immediately and are saved to the config file\n"
      "(pause_hotkey, mute_hotkey). Menu labels update on the next launch.");

  ImGui::Spacing();
  ImGui::TextUnformatted("Fixed hotkeys");
  ImGui::Separator();
  if (ImGui::BeginTable("fixed_hotkeys", 2, ImGuiTableFlags_SizingFixedFit)) {
    for (const auto& h : kFixedHotkeys) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(h.name);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(h.action);
    }
    ImGui::EndTable();
  }

  ImGui::End();
  if (!dialog_open) {
    w.capturing_action_ = -1;
    Close();
    emulator_window_.ToggleKeyboardHotkeysDialog();
    // `this` is deleted by Draw() after this returns.
    return;
  }
}

void EmulatorWindow::SetPausedOverlay(bool shown) {
  if (state_op_in_progress_) {
    // A save or load pauses and resumes on its own; "PAUSED" would mislead.
    paused_overlay_.reset();
    return;
  }
  if (shown) {
    if (!paused_overlay_) {
      paused_overlay_ =
          std::make_unique<PausedOverlayDialog>(imgui_drawer_.get(), *this);
    }
  } else {
    paused_overlay_.reset();
  }
}

void EmulatorWindow::SetStateOverlay(const char* title, const char* hint) {
  if (!title) {
    state_overlay_.reset();
    return;
  }
  state_overlay_title_ = title;
  state_overlay_hint_ = hint ? hint : "";
  if (!state_overlay_) {
    state_overlay_ =
        std::make_unique<StateOverlayDialog>(imgui_drawer_.get(), *this);
  }
}

void EmulatorWindow::StateOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::SetNextWindowBgAlpha(0.0f);
  if (!ImGui::Begin("##state_overlay", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                        ImGuiWindowFlags_NoNav)) {
    ImGui::End();
    return;
  }
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                           IM_COL32(0, 0, 0, 120));
  const char* title = emulator_window_.state_overlay_title_.c_str();
  const std::string& hint = emulator_window_.state_overlay_hint_;
  const float title_scale = 3.0f;
  ImGui::SetWindowFontScale(title_scale);
  ImVec2 title_size = ImGui::CalcTextSize(title);
  ImGui::SetWindowFontScale(1.0f);
  ImVec2 hint_size = ImGui::CalcTextSize(hint.c_str());
  float total_height = title_size.y + 8.0f + hint_size.y;
  float y = (io.DisplaySize.y - total_height) * 0.5f;
  ImFont* font = ImGui::GetFont();
  float base_size = ImGui::GetFontSize();
  ImVec2 title_pos((io.DisplaySize.x - title_size.x) * 0.5f, y);
  draw_list->AddText(font, base_size * title_scale,
                     ImVec2(title_pos.x + 2.0f, title_pos.y + 2.0f),
                     IM_COL32(0, 0, 0, 200), title);
  draw_list->AddText(font, base_size * title_scale, title_pos,
                     IM_COL32(255, 255, 255, 255), title);
  ImVec2 hint_pos((io.DisplaySize.x - hint_size.x) * 0.5f,
                  y + title_size.y + 8.0f);
  draw_list->AddText(font, base_size, ImVec2(hint_pos.x + 1.0f, hint_pos.y + 1.0f),
                     IM_COL32(0, 0, 0, 200), hint.c_str());
  draw_list->AddText(font, base_size, hint_pos, IM_COL32(230, 230, 230, 255),
                     hint.c_str());
  ImGui::End();
}

void EmulatorWindow::PausedOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::SetNextWindowBgAlpha(0.0f);
  if (!ImGui::Begin("##paused_overlay", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                        ImGuiWindowFlags_NoNav)) {
    ImGui::End();
    return;
  }
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  draw_list->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                           IM_COL32(0, 0, 0, 120));

  const char* title = "PAUSED";
  std::string hint;
  auto pause_key =
      emulator_window_.action_key(EmulatorWindow::HotkeyAction::kPauseResume);
  if (pause_key.has_value()) {
    hint = "Press " + HotkeyName(*pause_key) +
           " or use CPU > Pause/Resume Emulation to resume";
  } else {
    hint = "Use CPU > Pause/Resume Emulation to resume";
  }

  const float title_scale = 3.0f;
  ImGui::SetWindowFontScale(title_scale);
  ImVec2 title_size = ImGui::CalcTextSize(title);
  ImGui::SetWindowFontScale(1.0f);
  ImVec2 hint_size = ImGui::CalcTextSize(hint.c_str());
  float total_height = title_size.y + 8.0f + hint_size.y;
  float y = (io.DisplaySize.y - total_height) * 0.5f;

  ImFont* font = ImGui::GetFont();
  float base_size = ImGui::GetFontSize();
  ImVec2 title_pos((io.DisplaySize.x - title_size.x) * 0.5f, y);
  draw_list->AddText(font, base_size * title_scale,
                     ImVec2(title_pos.x + 2.0f, title_pos.y + 2.0f),
                     IM_COL32(0, 0, 0, 200), title);
  draw_list->AddText(font, base_size * title_scale, title_pos,
                     IM_COL32(255, 255, 255, 255), title);
  ImVec2 hint_pos((io.DisplaySize.x - hint_size.x) * 0.5f,
                  y + title_size.y + 8.0f);
  draw_list->AddText(font, base_size,
                     ImVec2(hint_pos.x + 1.0f, hint_pos.y + 1.0f),
                     IM_COL32(0, 0, 0, 200), hint.c_str());
  draw_list->AddText(font, base_size, hint_pos, IM_COL32(220, 220, 220, 255),
                     hint.c_str());
  ImGui::End();
}

void EmulatorWindow::TogglePauseEmulation() {
  auto* emu = emulator();
  if (!emu->is_title_open()) {
    return;
  }
  // Pause() waits for the GPU and audio workers to park and suspends every
  // guest thread; do it off the UI thread so the window keeps painting.
  bool pause = !emu->is_paused();
  std::thread([emu, pause]() {
    xe::threading::set_name("Pause Toggle");
    if (pause) {
      emu->Pause();
    } else {
      emu->Resume();
    }
  }).detach();
}

std::filesystem::path EmulatorWindow::SaveStateDir() const {
  return cvars::save_state_dir.empty()
             ? emulator_->storage_root() / "savestates"
             : std::filesystem::path(cvars::save_state_dir);
}

namespace {
// Renames from -> to together with the .png thumbnail next to it.
void RenameSaveStateFiles(const std::filesystem::path& from,
                          const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    XELOGE("Save state: could not rename {} to {}: {}", from.string(),
           to.string(), ec.message());
    return;
  }
  auto from_png = from, to_png = to;
  from_png.replace_extension(".png");
  to_png.replace_extension(".png");
  if (std::filesystem::exists(from_png, ec)) {
    std::filesystem::rename(from_png, to_png, ec);
  }
  XELOGI("Save state: adopted {} as {}", from.string(), to.filename().string());
}
}  // namespace

std::filesystem::path EmulatorWindow::SaveStateSlotPath(int slot) {
  auto dir = SaveStateDir();
  uint32_t title_id = emulator_->title_id();
  auto per_title = dir / fmt::format("{:08X}_{}.sav", title_id, slot);
  std::error_code ec;
  if (slot == 1) {
    // Before slots existed the single state was <title id>.sav.
    auto legacy = dir / fmt::format("{:08X}.sav", title_id);
    if (!std::filesystem::exists(per_title, ec) &&
        std::filesystem::exists(legacy, ec)) {
      RenameSaveStateFiles(legacy, per_title);
    }
  }
  if (!emulator_->is_multi_disc()) {
    return per_title;
  }
  // Multi-disc title: one set of slots per disc. A per-title file is moved
  // to the disc its header names, or to the running disc if it predates
  // disc information (format 1 and 2).
  auto per_disc = dir / fmt::format("{:08X}_disc{}_{}.sav", title_id,
                                    emulator_->disc_number(), slot);
  if (std::filesystem::exists(per_title, ec)) {
    SaveStateFileInfo info;
    uint8_t disc = emulator_->disc_number();
    if (Emulator::ReadSaveStateInfo(per_title, &info) && info.has_disc_info() &&
        info.disc_number) {
      disc = info.disc_number;
    }
    auto target = dir / fmt::format("{:08X}_disc{}_{}.sav", title_id, disc, slot);
    if (!std::filesystem::exists(target, ec)) {
      RenameSaveStateFiles(per_title, target);
    }
  }
  return per_disc;
}

std::filesystem::path EmulatorWindow::SaveStatePath() {
  int slot = std::clamp(cvars::save_state_slot, 1, kSaveStateSlots);
  return SaveStateSlotPath(slot);
}

std::string EmulatorWindow::SaveStateSlotSummary(int slot) {
  auto path = SaveStateSlotPath(slot);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return fmt::format("Slot {}: empty", slot);
  }
  auto mtime = std::filesystem::last_write_time(path, ec);
  auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      mtime - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  std::time_t t = std::chrono::system_clock::to_time_t(system_time);
  char when[32] = {};
  std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&t));
  std::string disc;
  SaveStateFileInfo info;
  if (Emulator::ReadSaveStateInfo(path, &info)) {
    if (info.has_disc_info() && info.disc_count > 1) {
      disc = fmt::format(", disc {}", info.disc_number);
    }
    std::string mismatch = emulator_->SaveStateMismatch(info);
    if (!mismatch.empty()) {
      disc += fmt::format(" - {}", mismatch);
    }
  } else {
    disc = " - not a save state";
  }
  return fmt::format("Slot {}: saved {} ({} MB{})", slot, when,
                     std::filesystem::file_size(path, ec) >> 20, disc);
}

xe::ui::ImmediateTexture* EmulatorWindow::SlotThumbnailTexture(int slot) {
  auto png = SaveStateSlotPath(slot);
  png.replace_extension(".png");
  std::error_code ec;
  if (!std::filesystem::exists(png, ec)) {
    slot_thumbnails_.erase(slot);
    return nullptr;
  }
  auto mtime = std::filesystem::last_write_time(png, ec);
  auto it = slot_thumbnails_.find(slot);
  if (it != slot_thumbnails_.end() && it->second.mtime == mtime) {
    return it->second.texture.get();
  }
  std::ifstream file(png, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
  SlotThumbnail entry;
  entry.mtime = mtime;
  entry.texture = imgui_drawer_->LoadImGuiIcon(bytes);
  auto* texture = entry.texture.get();
  slot_thumbnails_[slot] = std::move(entry);
  return texture;
}

void EmulatorWindow::PickSaveStateDir() {
  auto picker = xe::ui::FilePicker::Create();
  picker->set_mode(ui::FilePicker::Mode::kOpen);
  picker->set_type(ui::FilePicker::Type::kDirectory);
  picker->set_multi_selection(false);
  picker->set_title("Select the save state folder");
  if (!picker->Show(window_.get())) {
    return;
  }
  auto selected = picker->selected_files();
  if (!selected.empty() && !selected[0].empty()) {
    SetSaveStateDir(selected[0]);
  }
}

void EmulatorWindow::SetSaveStateDir(const std::filesystem::path& dir) {
  auto old_dir = SaveStateDir();
  std::error_code ec;
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      XELOGE("Save state folder: cannot create {}: {}", dir.string(),
             ec.message());
      new xe::ui::HostNotificationWindow(
          imgui_drawer(), "Save state folder",
          fmt::format("Cannot create {}: {}", dir.string(), ec.message()), 0);
      return;
    }
  }
  OVERRIDE_string(save_state_dir, dir.string());
  config::SaveConfig();
  auto new_dir = SaveStateDir();
  slot_thumbnails_.clear();
  if (!std::filesystem::equivalent(old_dir, new_dir, ec) &&
      CountSaveStateFiles(old_dir)) {
    previous_save_state_dir_ = old_dir;
  } else {
    previous_save_state_dir_.clear();
  }
  XELOGI("Save state folder: {} (was {})", new_dir.string(),
         old_dir.string());
}

size_t EmulatorWindow::CountSaveStateFiles(const std::filesystem::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    auto ext = entry.path().extension();
    if (entry.is_regular_file(ec) && (ext == ".sav" || ext == ".png")) {
      ++n;
    }
  }
  return n;
}

void EmulatorWindow::MoveSaveStates(const std::filesystem::path& from,
                                    const std::filesystem::path& to) {
  if (state_op_in_progress_.exchange(true)) {
    return;
  }
  std::thread([this, from, to]() {
    xe::threading::set_name("Move Save States");
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    size_t moved = 0, failed = 0;
    for (auto& entry : std::filesystem::directory_iterator(from, ec)) {
      auto ext = entry.path().extension();
      if (!entry.is_regular_file(ec) || (ext != ".sav" && ext != ".png")) {
        continue;
      }
      auto target = to / entry.path().filename();
      if (std::filesystem::exists(target, ec)) {
        XELOGW("Move save states: {} exists, {} left in place",
               target.string(), entry.path().string());
        ++failed;
        continue;
      }
      std::filesystem::rename(entry.path(), target, ec);
      if (ec) {
        // Another filesystem: copy, then remove.
        ec.clear();
        std::filesystem::copy_file(entry.path(), target, ec);
        if (!ec) {
          std::filesystem::remove(entry.path(), ec);
        }
      }
      if (ec) {
        XELOGE("Move save states: {} -> {}: {}", entry.path().string(),
               target.string(), ec.message());
        std::filesystem::remove(target, ec);
        ++failed;
      } else {
        ++moved;
      }
    }
    XELOGI("Move save states: {} moved, {} left in {}", moved, failed,
           from.string());
    std::string text =
        failed ? fmt::format("{} file(s) moved, {} left in {} (see the log)",
                             moved, failed, from.string())
               : fmt::format("{} file(s) moved to {}", moved, to.string());
    state_op_in_progress_ = false;
    app_context().CallInUIThread([this, text]() {
      slot_thumbnails_.clear();
      new xe::ui::HostNotificationWindow(imgui_drawer(), "Save state folder",
                                         text, 0);
    });
  }).detach();
}

void EmulatorWindow::ToggleGameLibraryDialog() {
  if (!game_library_dialog_) {
    ScanGamesDir();
    game_library_dialog_ =
        std::make_unique<GameLibraryDialog>(imgui_drawer_.get(), *this);
  } else {
    if (game_library_dialog_->IsClosing()) {
      game_library_dialog_.release();
    } else {
      game_library_dialog_.reset();
    }
  }
}

void EmulatorWindow::PickGamesDir() {
  auto picker = xe::ui::FilePicker::Create();
  picker->set_mode(ui::FilePicker::Mode::kOpen);
  picker->set_type(ui::FilePicker::Type::kDirectory);
  picker->set_multi_selection(false);
  picker->set_title("Select the games folder");
  if (!cvars::games_dir.empty()) {
    picker->set_default_path(cvars::games_dir);
  }
  if (!picker->Show(window_.get())) {
    return;
  }
  auto selected = picker->selected_files();
  if (!selected.empty() && !selected[0].empty()) {
    SetGamesDir(selected[0]);
  }
}

void EmulatorWindow::SetGamesDir(const std::filesystem::path& dir) {
  OVERRIDE_string(games_dir, dir.string());
  config::SaveConfig();
  XELOGI("Games folder: {}", dir.empty() ? "(none)" : dir.string());
  ScanGamesDir();
}

void EmulatorWindow::ScanGamesDir() {
  library_entries_.clear();
  library_status_.clear();
  if (cvars::games_dir.empty()) {
    library_status_ = "No games folder set.";
    return;
  }
  std::filesystem::path root = cvars::games_dir;
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    library_status_ = fmt::format("{} is not a folder.", root.string());
    return;
  }
  constexpr int kMaxDepth = 3;
  constexpr size_t kMaxEntries = 2000;
  bool truncated = false;
  auto it = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  for (; !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it.depth() >= kMaxDepth) {
      it.disable_recursion_pending();
    }
    const auto& entry = *it;
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    std::string ext = xe::utf8::lower_ascii(entry.path().extension().string());
    if (ext != ".iso" && ext != ".xex" && ext != ".zar") {
      continue;
    }
    if (library_entries_.size() >= kMaxEntries) {
      truncated = true;
      break;
    }
    LibraryEntry e;
    e.path = entry.path();
    e.label = std::filesystem::relative(entry.path(), root, ec).string();
    if (ec || e.label.empty()) {
      ec.clear();
      e.label = entry.path().filename().string();
    }
    e.size = entry.file_size(ec);
    ec.clear();
    for (const auto& recent : recently_launched_titles_) {
      if (recent.path_to_file == entry.path()) {
        e.title_name = recent.title_name;
        break;
      }
    }
    library_entries_.push_back(std::move(e));
  }
  std::sort(library_entries_.begin(), library_entries_.end(),
            [](const LibraryEntry& a, const LibraryEntry& b) {
              return a.label < b.label;
            });
  library_status_ =
      library_entries_.empty()
          ? "No .iso, .xex or .zar files found (3 folder levels scanned)."
          : fmt::format("{} file(s){}", library_entries_.size(),
                        truncated ? ", list cut at 2000" : "");
  XELOGI("Games folder: {} scanned, {} entries", root.string(),
         library_entries_.size());
}

void EmulatorWindow::GameLibraryDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, io.DisplaySize.y - 40.0f));
  bool dialog_open = true;
  if (!ImGui::Begin("Game library", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    emulator_window_.ToggleGameLibraryDialog();
    return;
  }
  EmulatorWindow& w = emulator_window_;
  ImGui::Text("Folder: %s", cvars::games_dir.empty()
                                ? "(none)"
                                : cvars::games_dir.c_str());
  if (ImGui::Button("Change folder...")) {
    w.app_context().CallInUIThread([&w]() { w.PickGamesDir(); });
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(cvars::games_dir.empty());
  if (ImGui::Button("Rescan")) {
    w.ScanGamesDir();
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    w.SetGamesDir("");
  }
  ImGui::EndDisabled();
  ImGui::TextUnformatted(w.library_status_.c_str());
  ImGui::Separator();
  if (!w.library_entries_.empty() &&
      ImGui::BeginTable("games", 4,
                        ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("File");
    ImGui::TableSetupColumn("Title");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    int index = 0;
    for (const auto& entry : w.library_entries_) {
      ImGui::TableNextRow();
      ImGui::PushID(index++);
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(entry.label.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(entry.title_name.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%llu MB", (unsigned long long)(entry.size >> 20));
      ImGui::TableSetColumnIndex(3);
      if (ImGui::Button("Launch")) {
        auto path = entry.path;
        // RunTitle tears the current title down; not from inside a frame.
        w.app_context().CallInUIThread([&w, path]() { w.RunTitle(path); });
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
  if (!dialog_open) {
    Close();
    emulator_window_.ToggleGameLibraryDialog();
    return;
  }
}

// Content folder (Content > Content Folder...).

namespace {
bool IsHexName(const std::filesystem::path& name, size_t digits) {
  std::string s = name.string();
  if (s.size() != digits) {
    return false;
  }
  return std::all_of(s.begin(), s.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string SizeText(uint64_t bytes) {
  if (bytes >= (1ull << 30)) {
    return fmt::format("{:.1f} GiB", double(bytes) / double(1ull << 30));
  }
  if (bytes >= (1ull << 20)) {
    return fmt::format("{:.1f} MiB", double(bytes) / double(1ull << 20));
  }
  return fmt::format("{} KiB", (bytes + 1023) >> 10);
}

// Moves from -> to. A folder that already exists at the target is merged
// one level down; a file that already exists is left in place. Falls back
// to copy + remove across filesystems.
void MoveTree(const std::filesystem::path& from, const std::filesystem::path& to,
              size_t& moved, size_t& left) {
  std::error_code ec;
  if (!std::filesystem::exists(to, ec)) {
    std::filesystem::rename(from, to, ec);
    if (ec) {
      ec.clear();
      std::filesystem::copy(from, to,
                            std::filesystem::copy_options::recursive, ec);
      if (!ec) {
        std::filesystem::remove_all(from, ec);
      }
    }
    if (ec) {
      XELOGE("Move content: {} -> {}: {}", from.string(), to.string(),
             ec.message());
      ++left;
    } else {
      ++moved;
    }
    return;
  }
  if (!std::filesystem::is_directory(from, ec) ||
      !std::filesystem::is_directory(to, ec)) {
    XELOGW("Move content: {} exists, {} left in place", to.string(),
           from.string());
    ++left;
    return;
  }
  for (auto& entry : std::filesystem::directory_iterator(from, ec)) {
    MoveTree(entry.path(), to / entry.path().filename(), moved, left);
  }
  std::filesystem::remove(from, ec);  // only if it is empty now
}
}  // namespace

void EmulatorWindow::ToggleContentFolderDialog() {
  if (!content_folder_dialog_) {
    ApplyPendingContentRoot();
    ScanContentRoot();
    content_folder_dialog_ =
        std::make_unique<ContentFolderDialog>(imgui_drawer_.get(), *this);
  } else {
    if (content_folder_dialog_->IsClosing()) {
      content_folder_dialog_.release();
    } else {
      content_folder_dialog_.reset();
    }
  }
}

std::filesystem::path EmulatorWindow::ContentRootFromConfig() const {
  std::filesystem::path root = cvars::content_root;
  if (root.empty()) {
    root = emulator_->storage_root() / "content";
  } else if (!root.is_absolute()) {
    root = emulator_->storage_root() / root;
  }
  return std::filesystem::absolute(root);
}

void EmulatorWindow::PickContentRoot() {
  auto picker = xe::ui::FilePicker::Create();
  picker->set_mode(ui::FilePicker::Mode::kOpen);
  picker->set_type(ui::FilePicker::Type::kDirectory);
  picker->set_multi_selection(false);
  picker->set_title("Select the content folder");
  picker->set_default_path(emulator_->content_root());
  if (!picker->Show(window_.get())) {
    return;
  }
  auto selected = picker->selected_files();
  if (!selected.empty() && !selected[0].empty()) {
    SetContentRoot(selected[0]);
  }
}

void EmulatorWindow::SetContentRoot(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      XELOGE("Content folder: cannot create {}: {}", dir.string(),
             ec.message());
      new xe::ui::HostNotificationWindow(
          imgui_drawer(), "Content folder",
          fmt::format("Cannot create {}: {}", dir.string(), ec.message()), 0);
      return;
    }
  }
  // content_root is defined in xenia_main.cc; reach it through the registry.
  auto it = cvar::ConfigVars ? cvar::ConfigVars->find("content_root")
                             : std::map<std::string, cvar::IConfigVar*>::iterator();
  if (!cvar::ConfigVars || it == cvar::ConfigVars->end()) {
    XELOGE("Content folder: no content_root config variable");
    return;
  }
  dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(it->second)
      ->OverrideConfigValue(dir);
  config::SaveConfig();
  auto new_root = ContentRootFromConfig();
  if (std::filesystem::equivalent(new_root, emulator_->content_root(), ec)) {
    pending_content_root_.clear();
    XELOGI("Content folder: {} (unchanged)", new_root.string());
    return;
  }
  // Packages the running title mounted stay in the old folder, and a save
  // it writes would land in the new one: wait for the title to close.
  pending_content_root_ = new_root;
  XELOGI("Content folder: {} chosen (current {}{})", new_root.string(),
         emulator_->content_root().string(),
         emulator_->is_title_open() ? ", applied when the title closes" : "");
  ApplyPendingContentRoot();
}

void EmulatorWindow::ApplyPendingContentRoot() {
  if (pending_content_root_.empty() || emulator_->is_title_open()) {
    return;
  }
  auto old_root = emulator_->content_root();
  emulator_->set_content_root(pending_content_root_);
  pending_content_root_.clear();
  std::error_code ec;
  if (CountContentTitles(old_root) &&
      !std::filesystem::equivalent(old_root, emulator_->content_root(), ec)) {
    previous_content_root_ = old_root;
  } else {
    previous_content_root_.clear();
  }
  ScanContentRoot();
}

size_t EmulatorWindow::CountContentTitles(const std::filesystem::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (auto& profile : std::filesystem::directory_iterator(dir, ec)) {
    if (!profile.is_directory(ec) || !IsHexName(profile.path().filename(), 16)) {
      continue;
    }
    for (auto& title : std::filesystem::directory_iterator(profile, ec)) {
      if (title.is_directory(ec) && IsHexName(title.path().filename(), 8)) {
        ++n;
      }
    }
  }
  return n;
}

void EmulatorWindow::MoveContent(const std::filesystem::path& from,
                                 const std::filesystem::path& to) {
  if (state_op_in_progress_.exchange(true)) {
    return;
  }
  content_status_ = fmt::format("Moving {} to {}...", from.string(), to.string());
  std::thread([this, from, to]() {
    xe::threading::set_name("Move Content");
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    size_t moved = 0, left = 0;
    for (auto& profile : std::filesystem::directory_iterator(from, ec)) {
      if (!profile.is_directory(ec) ||
          !IsHexName(profile.path().filename(), 16)) {
        continue;
      }
      MoveTree(profile.path(), to / profile.path().filename(), moved, left);
    }
    XELOGI("Move content: {} moved, {} left in {}", moved, left,
           from.string());
    std::string text =
        left ? fmt::format("{} item(s) moved, {} left in {} (see the log)",
                           moved, left, from.string())
             : fmt::format("{} item(s) moved to {}", moved, to.string());
    state_op_in_progress_ = false;
    app_context().CallInUIThread([this, text]() {
      ScanContentRoot();
      new xe::ui::HostNotificationWindow(imgui_drawer(), "Content folder",
                                         text, 0);
    });
  }).detach();
}

void EmulatorWindow::ScanContentRoot() {
  content_titles_.clear();
  content_status_.clear();
  const auto root = emulator_->content_root();
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    content_status_ = "The folder does not exist yet; nothing is installed.";
    return;
  }
  auto* content_manager = emulator_->kernel_state()
                              ? emulator_->kernel_state()->content_manager()
                              : nullptr;
  if (!content_manager) {
    content_status_ = "The emulator is not set up yet.";
    return;
  }
  constexpr size_t kMaxItems = 2000;
  bool truncated = false;
  size_t items = 0;
  std::map<uint32_t, ContentTitle> titles;
  for (auto& profile : std::filesystem::directory_iterator(root, ec)) {
    if (!profile.is_directory(ec) || !IsHexName(profile.path().filename(), 16)) {
      continue;
    }
    uint64_t xuid = std::strtoull(profile.path().filename().string().c_str(),
                                  nullptr, 16);
    for (auto& title : std::filesystem::directory_iterator(profile, ec)) {
      if (!title.is_directory(ec) || !IsHexName(title.path().filename(), 8)) {
        continue;
      }
      uint32_t title_id = uint32_t(
          std::strtoul(title.path().filename().string().c_str(), nullptr, 16));
      if (title_id == kernel::kDashboardID) {
        continue;  // the profile itself (Profile menu), not game content
      }
      for (auto& type : std::filesystem::directory_iterator(title, ec)) {
        if (!type.is_directory(ec) || !IsHexName(type.path().filename(), 8)) {
          continue;  // "Headers" and anything else
        }
        auto content_type = XContentType(
            std::strtoul(type.path().filename().string().c_str(), nullptr, 16));
        std::string type_name;
        auto it = XContentTypeMap.find(content_type);
        if (it != XContentTypeMap.end()) {
          type_name = it->second;
          if (content_type == XContentType::kMarketplaceContent) {
            type_name = "DLC";
          }
        } else {
          type_name = type.path().filename().string();
        }
        if (xuid != 0) {
          type_name += fmt::format(" (profile {:016X})", xuid);
        }
        for (auto& entry : std::filesystem::directory_iterator(type, ec)) {
          if (items >= kMaxItems) {
            truncated = true;
            break;
          }
          auto package = content_manager->OpenPackage(entry.path());
          if (!package || !package->IsValidPackage()) {
            continue;
          }
          ++items;
          auto& t = titles[title_id];
          t.title_id = title_id;
          if (t.title_name.empty()) {
            t.title_name =
                xe::to_utf8(package->GetContainerMetadata()->title_name());
          }
          ContentItem item;
          item.type = type_name;
          item.name = xe::to_utf8(package->GetContentMetadata().display_name());
          item.file = entry.path().filename().string();
          item.size = package->GetPackageSize();
          item.path = entry.path();
          t.size += item.size;
          t.items.push_back(std::move(item));
        }
      }
    }
  }
  for (auto& [id, title] : titles) {
    std::sort(title.items.begin(), title.items.end(),
              [](const ContentItem& a, const ContentItem& b) {
                return std::tie(a.type, a.name, a.file) <
                       std::tie(b.type, b.name, b.file);
              });
    content_titles_.push_back(std::move(title));
  }
  content_status_ =
      content_titles_.empty()
          ? "Nothing is installed in this folder."
          : fmt::format("{} item(s) for {} title(s){}", items,
                        content_titles_.size(),
                        truncated ? ", list cut at 2000" : "");
  XELOGI("Content folder: {} scanned, {} items, {} titles", root.string(),
         items, content_titles_.size());
}

void EmulatorWindow::ContentFolderDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, io.DisplaySize.y - 40.0f));
  bool dialog_open = true;
  if (!ImGui::Begin("Content folder", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    emulator_window_.ToggleContentFolderDialog();
    return;
  }
  EmulatorWindow& w = emulator_window_;
  w.ApplyPendingContentRoot();
  bool busy = w.state_op_in_progress_.load();
  bool title_open = w.emulator_->is_title_open();
  ImGui::Text("Folder: %s%s", w.emulator_->content_root().string().c_str(),
              cvars::content_root.empty() ? "  (default)" : "");
  ImGui::TextDisabled(
      "DLC, saves and installed titles are stored here as "
      "<profile>/<title id>/<content type>/. Install Content puts new "
      "packages here.");
  if (!w.pending_content_root_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                       "%s takes over when the running title closes.",
                       w.pending_content_root_.string().c_str());
  }
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Change folder...")) {
    // The picker runs its own event loop; not from inside a frame.
    w.app_context().CallInUIThread([&w]() { w.PickContentRoot(); });
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(cvars::content_root.empty());
  if (ImGui::Button("Use default")) {
    w.SetContentRoot("");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Rescan")) {
    w.ScanContentRoot();
  }
  if (!w.previous_content_root_.empty()) {
    size_t n = CountContentTitles(w.previous_content_root_);
    if (n == 0) {
      w.previous_content_root_.clear();
    } else {
      ImGui::Text("%zu title folder(s) are still in %s", n,
                  w.previous_content_root_.string().c_str());
      ImGui::SameLine();
      ImGui::BeginDisabled(title_open);
      if (ImGui::Button("Move them here")) {
        w.MoveContent(w.previous_content_root_, w.emulator_->content_root());
      }
      ImGui::EndDisabled();
      if (title_open) {
        ImGui::SameLine();
        ImGui::TextDisabled("(close the running title first)");
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::TextUnformatted(w.content_status_.c_str());
  ImGui::Separator();
  uint32_t running = title_open ? w.emulator_->title_id() : 0;
  for (const auto& title : w.content_titles_) {
    ImGui::PushID(int(title.title_id));
    std::string header =
        fmt::format("{:08X}  {}  ({} item{}, {})###title", title.title_id,
                    title.title_name.empty() ? "(no name)" : title.title_name,
                    title.items.size(), title.items.size() == 1 ? "" : "s",
                    SizeText(title.size));
    ImGui::SetNextItemOpen(
        title.title_id == running || w.content_titles_.size() <= 3,
        ImGuiCond_Once);
    if (ImGui::CollapsingHeader(header.c_str()) &&
        ImGui::BeginTable("items", 4,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Type");
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("File");
      ImGui::TableSetupColumn("Size");
      ImGui::TableHeadersRow();
      for (const auto& item : title.items) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(item.type.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(item.name.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(item.file.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(SizeText(item.size).c_str());
      }
      ImGui::EndTable();
    }
    ImGui::PopID();
  }
  ImGui::End();
  if (!dialog_open) {
    Close();
    emulator_window_.ToggleContentFolderDialog();
    return;
  }
}

// Advanced GPU options (Display > Advanced GPU options...).

namespace {
// The GPU cvars are defined in other files, so the OVERRIDE_ macros (which
// need the defining file's cv_ object) cannot reach them; the registry can.
template <typename T>
void SetGpuOption(const char* name, const T& value) {
  if (!cvar::ConfigVars) {
    return;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    XELOGE("GPU options: no config variable {}", name);
    return;
  }
  auto* var = dynamic_cast<cvar::ConfigVar<T>*>(it->second);
  if (!var) {
    XELOGE("GPU options: {} has another type", name);
    return;
  }
  var->OverrideConfigValue(value);
  config::SaveConfig();
  XELOGI("GPU options: {} = {}", name, var->config_value());
}

// The cvar's own description as the hover tooltip of the last item.
void GpuOptionTooltip(const char* name) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) ||
      !cvar::ConfigVars) {
    return;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it != cvar::ConfigVars->end()) {
    ImGui::SetTooltip("%s\n(%s)", it->second->description().c_str(), name);
  }
}

void GpuOptionCheckbox(const char* label, const char* name, bool value) {
  if (ImGui::Checkbox(label, &value)) {
    SetGpuOption<bool>(name, value);
  }
  GpuOptionTooltip(name);
}

// Combo over fixed string values; index -1 (not found) shows as the first.
void GpuOptionStringCombo(const char* label, const char* name,
                          const std::string& value,
                          const std::vector<std::pair<const char*, const char*>>&
                              choices /* value, label */) {
  int current = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    if (value == choices[i].first) {
      current = int(i);
      break;
    }
  }
  std::vector<const char*> labels;
  for (const auto& c : choices) {
    labels.push_back(c.second);
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
  if (ImGui::Combo(label, &current, labels.data(), int(labels.size()))) {
    SetGpuOption<std::string>(name, choices[current].first);
  }
  GpuOptionTooltip(name);
}

// Integer input: the - / + buttons and Enter commit (the value is re-read
// from the cvar every frame, so per-keystroke edits would be clamped and
// fought over mid-typing).
template <typename T>
void GpuOptionInt(const char* label, const char* name, T value, int min_value,
                  int max_value) {
  int v = int(value);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
  if (ImGui::InputInt(label, &v, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
    v = std::clamp(v, min_value, max_value);
    if (T(v) != value) {
      SetGpuOption<T>(name, T(v));
    }
  }
  GpuOptionTooltip(name);
}
}  // namespace

void EmulatorWindow::SetUIScale(float scale) {
  SetGpuOption<double>("ui_scale", double(scale));
  if (imgui_drawer_) {
    imgui_drawer_->RequestUIScale(scale);
  }
}

void EmulatorWindow::ToggleGpuOptionsDialog() {
  if (!gpu_options_dialog_) {
    gpu_options_dialog_ =
        std::make_unique<GpuOptionsDialog>(imgui_drawer_.get(), *this);
  } else {
    if (gpu_options_dialog_->IsClosing()) {
      gpu_options_dialog_.release();
    } else {
      gpu_options_dialog_.reset();
    }
  }
}

void EmulatorWindow::GpuOptionsDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, io.DisplaySize.y - 40.0f));
  bool dialog_open = true;
  if (!ImGui::Begin("Advanced GPU options", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    emulator_window_.ToggleGpuOptionsDialog();
    return;
  }
  EmulatorWindow& w = emulator_window_;
  gpu::CommandProcessor* command_processor =
      w.emulator_->graphics_system()
          ? w.emulator_->graphics_system()->command_processor()
          : nullptr;
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 44.0f);
  ImGui::TextDisabled(
      "Written to the config as they change (Enter applies a typed number). "
      "Hover an option for the description.");
  ImGui::PopTextWrapPos();

  if (ImGui::TreeNodeEx("Takes effect now", ImGuiTreeNodeFlags_Framed |
                                                ImGuiTreeNodeFlags_DefaultOpen)) {
    {
      bool fps = cvars::show_fps;
      if (ImGui::Checkbox("Show FPS overlay", &fps)) {
        w.ToggleFpsOverlay();
      }
      GpuOptionTooltip("show_fps");
    }
    GpuOptionCheckbox("VSync", "vsync", cvars::vsync);
    GpuOptionStringCombo(
        "Occlusion queries", "occlusion_query", cvars::occlusion_query,
        {{"fast", "fast: ask the GPU, use the cached answer (default)"},
         {"fast-alt", "fast-alt: fast, keeps zero results"},
         {"fake", "fake: never ask the GPU"},
         {"strict", "strict: wait for the GPU"}});
    {
      float v = float(cvars::occlusion_query_saturation);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
      ImGui::SliderFloat("Occlusion query saturation",
                         &v, 0.0f, 1.0f, "%.2f");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        SetGpuOption<double>("occlusion_query_saturation", double(v));
      }
      GpuOptionTooltip("occlusion_query_saturation");
    }
    GpuOptionStringCombo(
        "Readback resolve", "readback_resolve", cvars::readback_resolve,
        {{"none", "none: no CPU readback"},
         {"fast", "fast: previous frame, no stall"},
         {"full", "full: wait for the GPU"}});
    {
      static const char* kAniso[] = {"No override",  "Off", "1x", "2x",
                                     "4x",           "8x",  "16x"};
      int current = std::clamp(cvars::anisotropic_override + 1, 0, 6);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
      if (ImGui::Combo("Anisotropic filtering", &current, kAniso, 7)) {
        SetGpuOption<int32_t>("anisotropic_override", current - 1);
      }
      GpuOptionTooltip("anisotropic_override");
    }
    GpuOptionCheckbox("Clear memory page state (Team Ninja games)",
                      "clear_memory_page_state",
                      cvars::clear_memory_page_state);
    GpuOptionCheckbox("Allow invalid fetch constants",
                      "gpu_allow_invalid_fetch_constants",
                      cvars::gpu_allow_invalid_fetch_constants);
    GpuOptionCheckbox("Allow reads from no-access pages",
                      "gpu_allow_invalid_upload_range",
                      cvars::gpu_allow_invalid_upload_range);
    GpuOptionCheckbox("Half-pixel offset", "half_pixel_offset",
                      cvars::half_pixel_offset);
    GpuOptionCheckbox("Asynchronous shader compilation (new pipelines)",
                      "async_shader_compilation",
                      cvars::async_shader_compilation);
    GpuOptionCheckbox("Force depth clamp (new pipelines)",
                      "force_depth_clamp", cvars::force_depth_clamp);
    GpuOptionInt<uint32_t>("Texture cache soft limit, MB",
                           "texture_cache_memory_limit_soft",
                           cvars::texture_cache_memory_limit_soft, 16, 65536);
    GpuOptionInt<uint32_t>("Texture cache hard limit, MB",
                           "texture_cache_memory_limit_hard",
                           cvars::texture_cache_memory_limit_hard, 16, 65536);
    GpuOptionInt<uint32_t>("Texture unused for, s (soft limit)",
                           "texture_cache_memory_limit_soft_lifetime",
                           cvars::texture_cache_memory_limit_soft_lifetime, 0,
                           3600);
    ImGui::TreePop();
  }

  if (ImGui::TreeNodeEx("Needs a relaunch", ImGuiTreeNodeFlags_Framed |
                                                ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("Read when the emulator starts; a change waits for "
                        "the next launch.");
    {
      static const char* kPresets[] = {"Custom", "720p, native (1x1)",
                                       "1440p (2x2)", "4K (3x3)"};
      int px = std::clamp(cvars::draw_resolution_scale_x, 1, 7);
      int py = std::clamp(cvars::draw_resolution_scale_y, 1, 7);
      int preset = (px == py && px <= 3) ? px : 0;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
      if (ImGui::Combo("Output preset (720p game)", &preset, kPresets, 4) &&
          preset > 0) {
        SetGpuOption<int32_t>("draw_resolution_scale_x", preset);
        SetGpuOption<int32_t>("draw_resolution_scale_y", preset);
      }
    }
    {
      static const char* kScales[] = {"1x", "2x", "3x", "4x", "5x", "6x", "7x"};
      int x = std::clamp(cvars::draw_resolution_scale_x, 1, 7) - 1;
      int y = std::clamp(cvars::draw_resolution_scale_y, 1, 7) - 1;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
      if (ImGui::Combo("##scale_x", &x, kScales, 7)) {
        SetGpuOption<int32_t>("draw_resolution_scale_x", x + 1);
      }
      GpuOptionTooltip("draw_resolution_scale_x");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
      if (ImGui::Combo("Resolution scale (width, height)", &y, kScales, 7)) {
        SetGpuOption<int32_t>("draw_resolution_scale_y", y + 1);
      }
      GpuOptionTooltip("draw_resolution_scale_y");
      if (command_processor) {
        uint32_t used_x = command_processor->zpd_draw_resolution_scale_x();
        uint32_t used_y = command_processor->zpd_draw_resolution_scale_y();
        bool pending = used_x != uint32_t(x + 1) || used_y != uint32_t(y + 1);
        ImGui::SameLine();
        if (pending) {
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                             "in use: %ux%u, relaunch to apply", used_x,
                             used_y);
        } else {
          ImGui::TextDisabled("in use: %ux%u", used_x, used_y);
        }
      }
      ImGui::TextDisabled(
          "Above 1x needs sparse binding; the device's limit wins.");
    }
    GpuOptionStringCombo(
        "Render target path (Vulkan)", "render_target_path_vulkan",
        cvars::render_target_path_vulkan,
        {{"", "any: pick what suits the GPU"},
         {"fbo", "fbo: host framebuffers, faster, fewer formats"},
         {"fsi", "fsi: fragment shader interlock, most accurate"}});
    GpuOptionCheckbox(
        "Skip copying unchanged screen regions (experimental)",
        "dirty_region_tracking", cvars::dirty_region_tracking);
    ImGui::TextDisabled(
        "Tracks what each draw touches so render target copies move only "
        "what changed. Much faster above 1x in some games; the fbo render "
        "target path only, and not yet verified in every scene.");
    GpuOptionInt<uint64_t>("Frame rate limit, fps (0 = 60 with VSync, else "
                           "unlimited)",
                           "framerate_limit", cvars::framerate_limit, 0, 1000);
    GpuOptionCheckbox("Sparse shared memory (Vulkan)",
                      "vulkan_sparse_shared_memory",
                      cvars::vulkan_sparse_shared_memory);
    GpuOptionInt<int32_t>("Pipeline creation threads (-1 = automatic)",
                          "vulkan_pipeline_creation_threads",
                          cvars::vulkan_pipeline_creation_threads, -1, 64);
    ImGui::TreePop();
  }
  ImGui::End();
  if (!dialog_open) {
    Close();
    emulator_window_.ToggleGpuOptionsDialog();
    return;
  }
}

void EmulatorWindow::ToggleSaveStatesDialog() {
  if (!save_states_dialog_) {
    save_states_dialog_ =
        std::make_unique<SaveStatesDialog>(imgui_drawer_.get(), *this);
  } else {
    if (save_states_dialog_->IsClosing()) {
      save_states_dialog_.release();
    } else {
      save_states_dialog_.reset();
    }
  }
}

void EmulatorWindow::SaveStatesDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  // Auto-size, but never taller than the window: scroll instead.
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, io.DisplaySize.y - 40.0f));
  bool dialog_open = true;
  if (!ImGui::Begin("Save states", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    emulator_window_.ToggleSaveStatesDialog();
    return;
  }
  EmulatorWindow& w = emulator_window_;
  int current = std::clamp(cvars::save_state_slot, 1, kSaveStateSlots);
  bool busy = w.state_op_in_progress_.load();
  bool title_open = w.emulator_->is_title_open();
  auto save_key = w.action_key(HotkeyAction::kSaveState);
  auto load_key = w.action_key(HotkeyAction::kLoadState);
  ImGui::Text("%s saves, %s loads the selected slot; %s / %s change it.",
              save_key ? HotkeyName(*save_key).c_str() : "(no key)",
              load_key ? HotkeyName(*load_key).c_str() : "(no key)",
              cvars::next_slot_hotkey.c_str(), cvars::prev_slot_hotkey.c_str());
  ImGui::Separator();
  // Folder setting.
  auto dir = w.SaveStateDir();
  ImGui::Text("Folder: %s%s", dir.string().c_str(),
              cvars::save_state_dir.empty() ? "  (default)" : "");
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Change folder...")) {
    // The picker runs its own event loop; not from inside a frame.
    w.app_context().CallInUIThread([&w]() { w.PickSaveStateDir(); });
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(cvars::save_state_dir.empty());
  if (ImGui::Button("Use default")) {
    w.SetSaveStateDir("");
  }
  ImGui::EndDisabled();
  if (!w.previous_save_state_dir_.empty()) {
    size_t n = CountSaveStateFiles(w.previous_save_state_dir_);
    if (n == 0) {
      w.previous_save_state_dir_.clear();
    } else {
      ImGui::Text("%zu file(s) are still in %s", n,
                  w.previous_save_state_dir_.string().c_str());
      ImGui::SameLine();
      if (ImGui::Button("Move them here")) {
        w.MoveSaveStates(w.previous_save_state_dir_, dir);
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  if (ImGui::BeginTable("slots", 3, ImGuiTableFlags_SizingFixedFit)) {
    for (int slot = 1; slot <= kSaveStateSlots; ++slot) {
      ImGui::TableNextRow();
      ImGui::PushID(slot);
      ImGui::TableSetColumnIndex(0);
      auto* texture = title_open ? w.SlotThumbnailTexture(slot) : nullptr;
      if (texture) {
        ImGui::Image(reinterpret_cast<ImTextureID>(texture),
                     ImVec2(128.0f, 72.0f));
      } else {
        ImGui::Dummy(ImVec2(128.0f, 72.0f));
      }
      ImGui::TableSetColumnIndex(1);
      std::string summary =
          title_open ? w.SaveStateSlotSummary(slot) : fmt::format("Slot {}", slot);
      if (slot == current) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s  (selected)",
                           summary.c_str());
      } else {
        ImGui::TextUnformatted(summary.c_str());
      }
      ImGui::TableSetColumnIndex(2);
      if (slot != current && ImGui::Button("Select")) {
        w.SelectSaveStateSlot(slot, false);
      }
      if (slot != current) {
        ImGui::SameLine();
      }
      ImGui::BeginDisabled(busy || !title_open);
      if (ImGui::Button("Save")) {
        w.SelectSaveStateSlot(slot, false);
        w.SaveState();
      }
      ImGui::SameLine();
      if (ImGui::Button("Load")) {
        w.SelectSaveStateSlot(slot, false);
        w.LoadState();
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!title_open) {
    ImGui::TextUnformatted("No title is running.");
  }
  ImGui::End();
  if (!dialog_open) {
    Close();
    emulator_window_.ToggleSaveStatesDialog();
    return;
  }
}

void EmulatorWindow::SelectSaveStateSlot(int slot, bool announce) {
  slot = std::clamp(slot, 1, kSaveStateSlots);
  OVERRIDE_int32(save_state_slot, slot);
  config::SaveConfig();
  XELOGI("Save state slot {}", slot);
  if (announce && emulator_->is_title_open()) {
    ShowSlotOverlay();
  }
}

void EmulatorWindow::ShowSlotOverlay() {
  slot_overlay_deadline_ =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  if (!slot_overlay_) {
    slot_overlay_ = std::make_unique<SlotOverlayDialog>(imgui_drawer(), *this);
  }
}

void EmulatorWindow::HideSlotOverlay() {
  if (!slot_overlay_) {
    return;
  }
  // A dialog that has Close() pending deletes itself in the drawer's loop;
  // deleting it here as well would be a double free.
  if (slot_overlay_->IsClosing()) {
    slot_overlay_.release();
  } else {
    slot_overlay_.reset();
  }
}

void EmulatorWindow::SlotOverlayDialog::OnDraw(ImGuiIO& io) {
  EmulatorWindow& w = emulator_window_;
  if (std::chrono::steady_clock::now() >= w.slot_overlay_deadline_ ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
      !w.emulator_->is_title_open()) {
    Close();  // the drawer deletes this after OnDraw returns
    w.slot_overlay_.release();
    return;
  }
  int current = std::clamp(cvars::save_state_slot, 1, kSaveStateSlots);
  // Thumbnails 128x72, smaller when the window is short.
  float thumb_h = std::clamp(
      (io.DisplaySize.y - 120.0f) / float(kSaveStateSlots) - 6.0f, 27.0f,
      72.0f);
  float thumb_w = std::roundf(thumb_h * 16.0f / 9.0f);
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.88f);
  if (ImGui::Begin("##slot_overlay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoNav)) {
    auto save_key = w.action_key(HotkeyAction::kSaveState);
    auto load_key = w.action_key(HotkeyAction::kLoadState);
    ImGui::Text("Save state slots   %s / %s select, %s saves, %s loads, "
                "Esc hides",
                cvars::prev_slot_hotkey.c_str(), cvars::next_slot_hotkey.c_str(),
                save_key ? HotkeyName(*save_key).c_str() : "(no key)",
                load_key ? HotkeyName(*load_key).c_str() : "(no key)");
    ImGui::Separator();
    if (ImGui::BeginTable("slots", 2,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_RowBg)) {
      for (int slot = 1; slot <= kSaveStateSlots; ++slot) {
        ImGui::TableNextRow();
        if (slot == current) {
          ImGui::TableSetBgColor(
              ImGuiTableBgTarget_RowBg0,
              ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.2f, 0.35f)));
        }
        ImGui::TableSetColumnIndex(0);
        auto* texture = w.SlotThumbnailTexture(slot);
        if (texture) {
          ImGui::Image(reinterpret_cast<ImTextureID>(texture),
                       ImVec2(thumb_w, thumb_h));
        } else {
          ImGui::Dummy(ImVec2(thumb_w, thumb_h));
        }
        ImGui::TableSetColumnIndex(1);
        std::string summary = w.SaveStateSlotSummary(slot);
        if (slot == current) {
          ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s  (selected)",
                             summary.c_str());
        } else {
          ImGui::TextUnformatted(summary.c_str());
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

void EmulatorWindow::CycleSaveStateSlot(int delta) {
  int slot = std::clamp(cvars::save_state_slot, 1, kSaveStateSlots) + delta;
  if (slot > kSaveStateSlots) {
    slot = 1;
  } else if (slot < 1) {
    slot = kSaveStateSlots;
  }
  SelectSaveStateSlot(slot);
}

void EmulatorWindow::SaveState() {
  auto* emu = emulator();
  if (!emu->is_title_open()) {
    return;
  }
  if (state_op_in_progress_.exchange(true)) {
    return;
  }
  auto path = SaveStatePath();
  // Thumbnail: the guest output as it is now, scaled down on the save thread.
  auto capture = std::make_shared<xe::ui::RawImage>();
  if (auto* presenter = GetGraphicsSystemPresenter()) {
    if (!presenter->CaptureGuestOutput(*capture)) {
      capture->width = capture->height = 0;
    }
  }
  SetStateOverlay("SAVING STATE...", "");
  std::thread([this, emu, path, capture]() {
    xe::threading::set_name("Save State");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto t0 = std::chrono::steady_clock::now();
    // The overlay covers the pause only; the compressed write happens with
    // the game running and is reported by the notification.
    bool ok = emu->SaveToFile(path, [this]() {
      app_context().CallInUIThread(
          [this]() { SetStateOverlay(nullptr, nullptr); });
    });
    if (ok && capture->width && capture->height) {
      // Box-filter down to 320 px wide, RGBA.
      const uint32_t tw = 320;
      const uint32_t th = std::max<uint32_t>(
          1, capture->height * tw / std::max<uint32_t>(1, capture->width));
      xe::ui::RawImage thumb;
      thumb.width = tw;
      thumb.height = th;
      thumb.stride = tw * 4;
      thumb.data.resize(thumb.stride * th);
      for (uint32_t y = 0; y < th; ++y) {
        uint32_t sy0 = y * capture->height / th;
        uint32_t sy1 = std::max(sy0 + 1, (y + 1) * capture->height / th);
        for (uint32_t x = 0; x < tw; ++x) {
          uint32_t sx0 = x * capture->width / tw;
          uint32_t sx1 = std::max(sx0 + 1, (x + 1) * capture->width / tw);
          uint32_t acc[3] = {0, 0, 0}, n = 0;
          for (uint32_t sy = sy0; sy < sy1; ++sy) {
            const uint8_t* row = capture->data.data() + sy * capture->stride;
            for (uint32_t sx = sx0; sx < sx1; ++sx) {
              acc[0] += row[sx * 4];
              acc[1] += row[sx * 4 + 1];
              acc[2] += row[sx * 4 + 2];
              ++n;
            }
          }
          uint8_t* out = thumb.data.data() + y * thumb.stride + x * 4;
          out[0] = uint8_t(acc[0] / n);
          out[1] = uint8_t(acc[1] / n);
          out[2] = uint8_t(acc[2] / n);
          out[3] = 255;
        }
      }
      auto png = path;
      png.replace_extension(".png");
      SaveImage(png, thumb);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    uint64_t bytes = ok && std::filesystem::exists(path, ec)
                         ? std::filesystem::file_size(path, ec)
                         : 0;
    XELOGI("Save state: {} to {} in {} ms, {} bytes", ok ? "saved" : "FAILED",
           path.string(), ms, bytes);
    std::string text =
        ok ? fmt::format("Slot {}: saved ({} MB, {:.1f} s)",
                         std::clamp(cvars::save_state_slot, 1, kSaveStateSlots),
                         bytes >> 20, ms / 1000.0)
           : emu->last_save_error().empty()
                 ? "Save FAILED (a thread could not be stepped; see the log). "
                   "The previous save state was kept."
                 : fmt::format("Save FAILED: {}.", emu->last_save_error());
    state_op_in_progress_ = false;
    app_context().CallInUIThread([this, text]() {
      SetStateOverlay(nullptr, nullptr);
      new xe::ui::HostNotificationWindow(imgui_drawer(), "Save state", text, 0);
    });
  }).detach();
}

void EmulatorWindow::LoadState() {
  auto* emu = emulator();
  XELOGI("Load state: requested (title open={}, in progress={})",
         emu->is_title_open(), state_op_in_progress_.load());
  if (!emu->is_title_open()) {
    return;
  }
  auto path = SaveStatePath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    new xe::ui::HostNotificationWindow(
        imgui_drawer(), "Load state",
        fmt::format("Slot {} is empty ({})",
                    std::clamp(cvars::save_state_slot, 1, kSaveStateSlots),
                    path.filename().string()),
        0);
    return;
  }
  SaveStateFileInfo info;
  std::string mismatch = Emulator::ReadSaveStateInfo(path, &info)
                             ? emulator()->SaveStateMismatch(info)
                             : "not a save state";
  if (!mismatch.empty()) {
    XELOGE("Load state: {} refused: {}", path.string(), mismatch);
    new xe::ui::HostNotificationWindow(
        imgui_drawer(), "Load state",
        fmt::format("Slot {}: {}",
                    std::clamp(cvars::save_state_slot, 1, kSaveStateSlots),
                    mismatch),
        0);
    return;
  }
  if (state_op_in_progress_.exchange(true)) {
    return;
  }
  SetStateOverlay("LOADING STATE...", "");
  std::thread([this, emu, path]() {
    xe::threading::set_name("Load State");
    auto t0 = std::chrono::steady_clock::now();
    bool ok = emu->RestoreFromFile(path);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    XELOGI("Load state: {} from {} in {} ms", ok ? "loaded" : "FAILED",
           path.string(), ms);
    std::string text = ok ? fmt::format("Slot {}: loaded ({:.1f} s)",
                                        std::clamp(cvars::save_state_slot, 1,
                                                   kSaveStateSlots),
                                        ms / 1000.0)
                          : fmt::format("Load FAILED: {}", path.string());
    for (const auto& warning : emu->restore_warnings()) {
      text += "\n" + warning;
    }
    state_op_in_progress_ = false;
    app_context().CallInUIThread([this, text]() {
      SetStateOverlay(nullptr, nullptr);
      new xe::ui::HostNotificationWindow(imgui_drawer(), "Load state", text, 0);
    });
  }).detach();
}

bool EmulatorWindow::HandleAssignableHotkeys(ui::VirtualKey key,
                                             ui::KeyEvent& e) {
  if (capturing_action_ >= 0) {
    if (key != ui::VirtualKey::kEscape) {
      SetActionHotkey(HotkeyAction(capturing_action_), key);
    } else {
      hotkey_status_ = "Cancelled.";
    }
    capturing_action_ = -1;
    e.set_handled(true);
    return true;
  }
  if (e.is_ctrl_pressed() || e.is_alt_pressed()) {
    return false;
  }
  for (int a = 0; a < int(HotkeyAction::kCount); ++a) {
    if (action_keys_[a].has_value() && *action_keys_[a] == key) {
      switch (HotkeyAction(a)) {
        case HotkeyAction::kPauseResume:
          TogglePauseEmulation();
          break;
        case HotkeyAction::kMute:
          ToggleMute();
          break;
        case HotkeyAction::kSaveState:
          SaveState();
          break;
        case HotkeyAction::kLoadState:
          LoadState();
          break;
        case HotkeyAction::kNextSlot:
          CycleSaveStateSlot(1);
          break;
        case HotkeyAction::kPrevSlot:
          CycleSaveStateSlot(-1);
          break;
        default:
          break;
      }
      e.set_handled(true);
      return true;
    }
  }
  return false;
}

void EmulatorWindow::ToggleMute() {
  xe::apu::SetMuteOverride(!cvars::mute);
  config::SaveConfig();
  XELOGI("Audio {}", cvars::mute ? "muted" : "unmuted");
  UpdateStatusOverlay("Audio");
}

void EmulatorWindow::OnKeyChar(ui::KeyEvent& e) {
  if (!emulator_initialized_ || disable_hotkeys_) {
    return;
  }
  // On GTK the virtual key of a char event is the Unicode code point.
  uint32_t c = uint32_t(e.virtual_key());
  ui::VirtualKey key;
  if (c >= 'a' && c <= 'z') {
    key = ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + (c - 'a'));
  } else if (c >= 'A' && c <= 'Z') {
    key = ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + (c - 'A'));
  } else if (c >= '0' && c <= '9') {
    key = ui::VirtualKey(uint16_t(ui::VirtualKey::k0) + (c - '0'));
  } else if (c == ' ') {
    key = ui::VirtualKey::kSpace;
  } else if (c == '+' || c == '-' || c == '*') {
    // Numpad + - * are printable, so they never reach OnKeyDown on GTK.
    if (c == '+') {
      CpuTimeScalarSetDouble();
    } else if (c == '-') {
      CpuTimeScalarSetHalf();
    } else {
      CpuTimeScalarReset();
    }
    e.set_handled(true);
    return;
  } else {
    return;
  }
  HandleAssignableHotkeys(key, e);
}

void EmulatorWindow::OnKeyDown(ui::KeyEvent& e) {
  XELOGD("OnKeyDown vk={:X} initialized={} capturing={}",
         uint16_t(e.virtual_key()), emulator_initialized_, capturing_action_);
  if (!emulator_initialized_) {
    return;
  }

  if (HandleAssignableHotkeys(e.virtual_key(), e)) {
    return;
  }

  switch (e.virtual_key()) {
    case ui::VirtualKey::kO: {
      if (!e.is_ctrl_pressed()) {
        return;
      }
      FileOpen();
    } break;
    case ui::VirtualKey::kMultiply: {
      CpuTimeScalarReset();
    } break;
    case ui::VirtualKey::kSubtract: {
      CpuTimeScalarSetHalf();
    } break;
    case ui::VirtualKey::kAdd: {
      CpuTimeScalarSetDouble();
    } break;

    case ui::VirtualKey::kF3: {
      Profiler::ToggleDisplay();
    } break;

    case ui::VirtualKey::kF4: {
      GpuTraceFrame();
    } break;
    case ui::VirtualKey::kF5: {
      GpuClearCaches();
    } break;

    case ui::VirtualKey::kF6: {
      ToggleDisplayConfigDialog();
    } break;
    case ui::VirtualKey::kF11: {
      ToggleFullscreen();
    } break;
    case ui::VirtualKey::kF12: {
      TakeScreenshot();
    } break;

    case ui::VirtualKey::kEscape: {
      // The slot table overlay goes first; then fullscreen.
      if (slot_overlay_) {
        HideSlotOverlay();
        return;
      }
      // Allow users to escape fullscreen (but not enter it).
      if (!window_->IsFullscreen()) {
        return;
      }
      SetFullscreen(false);
    } break;

#ifdef DEBUG
    case ui::VirtualKey::kF7: {
      // Save to file
      // TODO: Choose path based on user input, or from options
      // TODO: Spawn a new thread to do this.
      emulator()->SaveToFile("test.sav");
    } break;
    case ui::VirtualKey::kF8: {
      // Restore from file
      // TODO: Choose path from user
      // TODO: Spawn a new thread to do this.
      emulator()->RestoreFromFile("test.sav");
    } break;
#endif  // #ifdef DEBUG

    case ui::VirtualKey::kPause: {
      CpuBreakIntoDebugger();
    } break;
    case ui::VirtualKey::kCancel: {
      CpuBreakIntoHostDebugger();
    } break;

    case ui::VirtualKey::kF1: {
      ShowFAQ();
    } break;

    case ui::VirtualKey::kF2: {
      ShowBuildCommit();
    } break;

    case ui::VirtualKey::kF9: {
      RunPreviouslyPlayedTitle();
    } break;

    default:
      return;
  }

  e.set_handled(true);
}

void EmulatorWindow::OnMouseDown(const ui::MouseEvent& e) {
  if (imgui_drawer_->IsAnyDialogOpen()) {
    return;
  }

  if (e.button() == ui::MouseEvent::Button::kLeft) {
    ToggleFullscreenOnDoubleClick();
  }
}

void EmulatorWindow::OnMouseUp(const ui::MouseEvent& e) {
  last_mouse_up = steady_clock::now();
}

void EmulatorWindow::TakeScreenshot() {
  xe::ui::RawImage image;

  imgui_drawer_->EnableNotifications(false);

  if (!GetGraphicsSystemPresenter()->CaptureGuestOutput(image) ||
      GetGraphicsSystemPresenter() == nullptr) {
    XELOGE("Failed to capture guest output for screenshot");
    return;
  }

  imgui_drawer_->EnableNotifications(true);
  ExportScreenshot(image);
}

void EmulatorWindow::ExportScreenshot(const xe::ui::RawImage& image) {
  auto t = std::time(nullptr);

  // The format is: Year-Month-DayTHours-Minutes-Seconds based off ISO 8601
  std::string datetime =
      fmt::format("{:%Y-%m-%dT%H-%M-%S}", *std::localtime(&t));

  // Get the title id of the game because some titles contain characters that
  // cannot be used as a directory
  std::string title_id;
  if (emulator()->title_id()) {
    title_id = fmt::format("{:08X}", emulator()->title_id());
  } else {
    XELOGE("Failed to get the current title id");
    return;
  }

  // Find where xenia.exe or xenia_canary.exe is located and create a
  // screenshots folder
  auto screenshot_path =
      (xe::filesystem::GetExecutableFolder() / "screenshots" / title_id);

  if (!std::filesystem::exists(screenshot_path)) {
    std::filesystem::create_directories(screenshot_path);
  }

  std::string filename = fmt::format("{} - {}.png", title_id, datetime);
  SaveImage(screenshot_path / filename, image);

  const std::string notification_text =
      fmt::format("Screenshot saved: {}", filename);

  app_context_.CallInUIThread([&, notification_text]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Screenshot Created!",
                                       notification_text, 0);
  });
}

// Converts a RawImage into a PNG file
void EmulatorWindow::SaveImage(const std::filesystem::path& filepath,
                               const xe::ui::RawImage& image) {
  auto file = std::ofstream(filepath, std::ios::binary);
  if (!file.is_open()) {
    XELOGE("Failed to open file for writing: {}", filepath);
    return;
  }

  auto result = stbi_write_png_to_func(
      [](void* context, void* data, int size) {
        auto file = reinterpret_cast<std::ofstream*>(context);
        file->write(reinterpret_cast<const char*>(data), size);
      },
      &file, image.width, image.height, 4, image.data.data(),
      (int)image.stride);
  if (result == 0) {
    XELOGE("Failed to write PNG to file: {}", filepath);
    return;
  }
}

void EmulatorWindow::ToggleFullscreenOnDoubleClick() {
  if (cvars::disable_doubleclick_fullscreen) {
    return;
  }

  // this function tests if user has double clicked.
  // if double click was achieved the fullscreen gets toggled
  const auto now = steady_clock::now();  // current mouse event time
  constexpr int16_t mouse_down_max_threshold = 250;
  constexpr int16_t mouse_up_max_threshold = 250;
  constexpr int16_t mouse_up_down_max_delta = 100;
  // max delta to prevent 'chaining' of double clicks with next mouse events

  const auto last_mouse_down_delta = diff_in_ms(now, last_mouse_down);
  if (last_mouse_down_delta >= mouse_down_max_threshold) {
    last_mouse_down = now;
    return;
  }

  const auto last_mouse_up_delta = diff_in_ms(now, last_mouse_up);
  const auto mouse_event_deltas = diff_in_ms(last_mouse_up, last_mouse_down);
  if (last_mouse_up_delta >= mouse_up_max_threshold) {
    return;
  }

  if (mouse_event_deltas < mouse_up_down_max_delta) {
    ToggleFullscreen();
  }
}

void EmulatorWindow::FileDrop(const std::filesystem::path& path) {
  if (!emulator_initialized_) {
    return;
  }

  RunTitle(path);
}

void EmulatorWindow::FileOpen() {
  std::filesystem::path path;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title("Select Content Package");
  if (!cvars::games_dir.empty()) {
    file_picker->set_default_path(cvars::games_dir);
  }
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.zar;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Disc Archive (*.zar)", "*.zar"},
      {"Xbox Executable (*.xex)", "*.xex"},
      //{"Content Package (*.xcp)", "*.xcp" },
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
    // Only run the title if a file is selected
    RunTitle(path);
  }
}

void EmulatorWindow::FileClose() { emulator_->TerminateTitle(); }

void EmulatorWindow::OnEmulatorReady() {
  emulator_ready_ = true;
  if (pending_launch_path_.empty()) {
    return;
  }
  auto path = pending_launch_path_;
  pending_launch_path_.clear();
  if (emulator_->is_title_open()) {
    return;
  }
  XELOGI("RunTitle: emulator ready, launching the queued {}", path.string());
  RunTitle(path);
}

void EmulatorWindow::ResetGame() {
  auto path = last_launched_path_;
  if (path.empty()) {
    for (const auto& recent : recently_launched_titles_) {
      path = recent.path_to_file;
      break;
    }
  }
  if (path.empty()) {
    XELOGW("Reset Game: nothing was launched yet");
    return;
  }
  XELOGI("Reset Game: {}", path.string());
  RunTitle(path);
}

void EmulatorWindow::CloseGame() {
  if (!emulator_->is_title_open()) {
    return;
  }
  XELOGI("Close Game: {}", emulator_->title_name());
  // Same reason as in RunTitle: a fresh process, with no title.
  RelaunchProcess("");
}

bool EmulatorWindow::RelaunchProcess(const std::filesystem::path& path) {
#if XE_PLATFORM_LINUX
  // The same command line as this process, with the title path replaced
  // (or removed) and a numbered log file so the old log is kept.
  std::vector<std::string> args;
  {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(cmdline)),
                    std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < all.size()) {
      size_t end = all.find('\0', start);
      if (end == std::string::npos) {
        end = all.size();
      }
      args.push_back(all.substr(start, end - start));
      start = end + 1;
    }
  }
  if (args.empty()) {
    XELOGE("Relaunch: cannot read /proc/self/cmdline");
    return false;
  }
  std::vector<std::string> new_args;
  new_args.push_back(args[0]);
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a.rfind("--", 0) != 0) {
      continue;  // the old title path (or any positional argument)
    }
    if (a.rfind("--log_file=", 0) == 0) {
      std::filesystem::path log = a.substr(11);
      std::string stem = log.stem().string();
      int n = 2;
      size_t p = stem.rfind("-relaunch");
      if (p != std::string::npos) {
        n = std::atoi(stem.c_str() + p + 9) + 1;
        stem.resize(p);
      }
      log = log.parent_path() /
            (stem + "-relaunch" + std::to_string(n) + log.extension().string());
      new_args.push_back("--log_file=" + log.string());
      continue;
    }
    if (a.rfind("--ui_experiment", 0) == 0 ||
        a.rfind("--savestate_experiment", 0) == 0) {
      continue;  // timers of this session, not the next one
    }
    new_args.push_back(a);
  }
  if (!path.empty()) {
    new_args.push_back(std::filesystem::absolute(path).string());
  }
  std::vector<char*> argv;
  for (auto& a : new_args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  std::string shown;
  for (auto& a : new_args) {
    shown += a + " ";
  }
  XELOGI("Relaunch: {}", shown);
  xe::FlushLog();
  pid_t child = fork();
  if (child < 0) {
    XELOGE("Relaunch: fork failed: {}", strerror(errno));
    return false;
  }
  if (child == 0) {
    setsid();
    // The binary by its path, so the process keeps its name (/proc/self/exe
    // would make it "exe" and hide it from pgrep -x).
    execv(new_args[0].c_str(), argv.data());
    _exit(127);
  }
  XELOGI("Relaunch: new process {}, closing this one", child);
  AddPlayTime();
  SaveLibrary();
  window_->RequestClose();
  return true;
#else
  XELOGE("Relaunch: not implemented on this platform");
  return false;
#endif
}

void EmulatorWindow::InstallContent() {
  std::vector<std::filesystem::path> paths;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Content Package");
  // Start in the content folder (Settings > Preferences > Folders).
  file_picker->set_default_path(emulator_->content_root());
  file_picker->set_extensions({
      {"All Files (*.*)", "*.*"},
  });
  if (file_picker->Show(window_.get())) {
    paths = file_picker->selected_files();
  }

  if (paths.empty()) {
    return;
  }

  std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
      content_installation_status =
          std::make_shared<std::vector<Emulator::ContentInstallEntry>>();

  for (const auto& path : paths) {
    content_installation_status->push_back({path});
  }

  for (auto& entry : *content_installation_status) {
    emulator_->ProcessContentPackageHeader(entry.path_, entry);
  }

  auto installationThread = std::thread([this, content_installation_status] {
    for (auto& entry : *content_installation_status) {
      emulator_->InstallContentPackage(entry.path_, entry);
    }
  });
  installationThread.detach();

  new ContentInstallDialog(imgui_drawer_.get(), *this,
                           content_installation_status);
}

void EmulatorWindow::ExtractContent(const std::filesystem::path file) {
  std::vector<std::filesystem::path> package_files;
  std::filesystem::path extract_dir;

  if (!file.empty()) {
    package_files.push_back(file);
  } else {
    auto file_picker = xe::ui::FilePicker::Create();
    file_picker->set_mode(ui::FilePicker::Mode::kOpen);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(true);
    file_picker->set_title("Select Content Package");
    // Start in the content folder (Settings > Preferences > Folders).
    file_picker->set_default_path(emulator_->content_root());
    file_picker->set_extensions({
        {"All Files (*.*)", "*.*"},
    });

    if (file_picker->Show(window_.get())) {
      package_files = file_picker->selected_files();
    }

    if (package_files.empty()) {
      return;
    }
  }
  auto save_file_picker = xe::ui::FilePicker::Create();
  save_file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  save_file_picker->set_type(ui::FilePicker::Type::kDirectory);
  save_file_picker->set_title("Select Directory to Extract");

  if (save_file_picker->Show(window_.get())) {
    extract_dir = save_file_picker->selected_files().front();
  }

  if (extract_dir.empty()) {
    return;
  }

  std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
      content_installation_status =
          std::make_shared<std::vector<Emulator::ContentInstallEntry>>();

  for (const auto& path : package_files) {
    content_installation_status->push_back({path});
  }

  for (auto& entry : *content_installation_status) {
    emulator_->ProcessContentPackageHeader(entry.path_, entry);
    entry.data_installation_path_ = extract_dir;
    entry.header_installation_path_ = "";
  }

  auto installationThread = std::thread([this, content_installation_status] {
    for (auto& entry : *content_installation_status) {
      emulator_->ExtractContentPackage(entry.path_, entry);
    }
  });
  installationThread.detach();

  new ContentInstallDialog(imgui_drawer_.get(), *this,
                           content_installation_status);
}

void EmulatorWindow::ExtractZarchive() {
  std::vector<std::filesystem::path> zarchive_files;
  std::filesystem::path extract_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Zar Package");
  file_picker->set_extensions({
      {"Zarchive Files (*.zar)", "*.zar"},
  });

  if (file_picker->Show(window_.get())) {
    zarchive_files = file_picker->selected_files();
  }

  if (zarchive_files.empty()) {
    return;
  }

  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_title("Select Directory to Extract");

  if (file_picker->Show(window_.get())) {
    extract_dir = file_picker->selected_files().front();
  }

  if (extract_dir.empty()) {
    return;
  }

  std::string extract_overview = "";

  for (auto& zarchive_file_path : zarchive_files) {
    extract_overview += "\n" + path_to_utf8(zarchive_file_path);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Extracting...",
                                       string_util::trim(extract_overview), 0);
  });

  auto run = [this, extract_dir, zarchive_files]() -> void {
    std::string summary = "";

    for (auto& zarchive_file_path : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_path = std::filesystem::absolute(zarchive_file_path);
      std::filesystem::path abs_extract_dir;

      if (zarchive_files.size() > 1) {
        abs_extract_dir =
            std::filesystem::absolute((extract_dir / abs_path.stem()));
      } else {
        abs_extract_dir = std::filesystem::absolute(extract_dir);
      }

      XELOGI("Extracting zar package: {}\n",
             zarchive_file_path.filename().string());

      auto result =
          emulator_->ExtractZarchivePackage(abs_path, abs_extract_dir);

      if (result != X_STATUS_SUCCESS) {
        std::error_code ec;

        if (!std::filesystem::is_empty(abs_extract_dir)) {
          std::filesystem::remove(abs_extract_dir, ec);
        }

        summary += fmt::format("\nFailed: {}", zarchive_file_path);

        XELOGE("Failed to extract Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", abs_extract_dir);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Extraction Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::CreateZarchive() {
  std::vector<std::filesystem::path> content_dirs;
  std::filesystem::path zarchive_dir;

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kDirectory);
  file_picker->set_multi_selection(true);
  file_picker->set_title("Select Contents");

  if (file_picker->Show(window_.get())) {
    content_dirs = file_picker->selected_files();
  }

  if (content_dirs.empty()) {
    return;
  }

  if (content_dirs.size() == 1) {
    file_picker->set_mode(ui::FilePicker::Mode::kSave);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(false);
    file_picker->set_file_name(content_dirs.front().filename().string());
    file_picker->set_default_extension("zar");
    file_picker->set_title("Zarchive File");
    file_picker->set_extensions({
        {"Zarchive File (*.zar)", "*.zar"},
    });
  } else {
    file_picker->set_title("Output Directory");
  }

  if (file_picker->Show(window_.get())) {
    zarchive_dir = file_picker->selected_files().front();
  }

  if (zarchive_dir.empty()) {
    return;
  }

  std::string create_overview = "";

  std::map<std::filesystem::path, std::filesystem::path> zarchive_files{};

  for (auto& content_path : content_dirs) {
    // Normalize the path and make absolute.
    auto abs_content_dir = std::filesystem::absolute(content_path);
    std::filesystem::path abs_zarchive_file;

    if (content_dirs.size() > 1) {
      abs_zarchive_file = std::filesystem::absolute(
          (zarchive_dir / abs_content_dir.filename().concat(".zar")));
    } else {
      abs_zarchive_file = std::filesystem::absolute(zarchive_dir);
    }

    zarchive_files[content_path] = abs_zarchive_file;

    create_overview += "\n" + path_to_utf8(abs_zarchive_file);
  }

  app_context_.CallInUIThread([&]() {
    new xe::ui::HostNotificationWindow(imgui_drawer(), "Creating...",
                                       string_util::trim(create_overview), 0);
  });

  auto run = [this, zarchive_files]() -> void {
    std::string summary = "";

    for (auto const& [content_path, zarchive_file] : zarchive_files) {
      // Normalize the path and make absolute.
      auto abs_content_dir = std::filesystem::absolute(content_path);

      XELOGI("Creating zar package: {}\n", zarchive_file.filename().string());

      auto result =
          emulator_->CreateZarchivePackage(abs_content_dir, zarchive_file);

      if (result != X_ERROR_SUCCESS) {
        std::error_code ec;

        // delete incomplete output file
        std::filesystem::remove(zarchive_file, ec);

        summary += fmt::format("\nFailed: {}", abs_content_dir);

        XELOGE("Failed to create Zarchive package.", result);
      } else {
        summary += fmt::format("\nSuccess: {}", zarchive_file);
      }
    }

    new xe::ui::HostNotificationWindow(imgui_drawer(), "Zar Creation Summary",
                                       string_util::trim(summary), 0);
  };

  auto zarThread = std::thread(run);
  zarThread.detach();
}

void EmulatorWindow::ShowContentDirectory() {
  auto content_root = emulator_->content_root();

  if (!std::filesystem::exists(content_root)) {
    std::filesystem::create_directories(content_root);
  }

  LaunchFileExplorer(content_root);
}

void EmulatorWindow::CpuTimeScalarReset() {
  Clock::set_guest_time_scalar(1.0);
  UpdateTitle();
  UpdateStatusOverlay("Speed");
}

void EmulatorWindow::CpuTimeScalarSetHalf() {
  Clock::set_guest_time_scalar(
      std::max(1.0 / 16.0, Clock::guest_time_scalar() / 2.0));
  UpdateTitle();
  UpdateStatusOverlay("Speed");
}

void EmulatorWindow::CpuTimeScalarSetDouble() {
  Clock::set_guest_time_scalar(
      std::min(16.0, Clock::guest_time_scalar() * 2.0));
  UpdateTitle();
  UpdateStatusOverlay("Speed");
}

void EmulatorWindow::UpdateStatusOverlay(const char* notify_title) {
  double scalar = Clock::guest_time_scalar();
  bool normal_speed = std::abs(scalar - 1.0) < 1e-9;
  bool any = !normal_speed || cvars::mute || cvars::show_fps;
  if (!any) {
    status_overlay_.reset();
  } else if (!status_overlay_) {
    status_overlay_ =
        std::make_unique<StatusOverlayDialog>(imgui_drawer_.get(), *this);
  }
  if (notify_title) {
    std::string text;
    if (std::string(notify_title) == "Audio") {
      text = cvars::mute ? "Muted" : "Unmuted";
    } else {
      text = normal_speed ? "Normal speed (1.00x)"
             : scalar > 1.0 ? fmt::format("Fast-forward {:.2f}x", scalar)
                            : fmt::format("Slow-motion {:.2f}x", scalar);
    }
    new xe::ui::HostNotificationWindow(imgui_drawer(), notify_title, text, 0);
  }
}

void EmulatorWindow::ToggleFpsOverlay() {
  SetGpuOption<bool>("show_fps", !cvars::show_fps);
  UpdateStatusOverlay(nullptr);
}

void EmulatorWindow::StatusOverlayDialog::OnDraw(ImGuiIO& io) {
  double scalar = Clock::guest_time_scalar();
  bool normal_speed = std::abs(scalar - 1.0) < 1e-9;
  if (normal_speed && !cvars::mute && !cvars::show_fps) {
    return;
  }
  if (cvars::show_fps) {
    auto* gs = emulator_window_.emulator_->graphics_system();
    auto* cp = gs ? gs->command_processor() : nullptr;
    auto now = std::chrono::steady_clock::now();
    if (fps_last_time_.time_since_epoch().count() == 0) {
      fps_last_time_ = now;
      fps_last_swaps_ = cp ? cp->swap_count() : 0;
    } else {
      double dt = std::chrono::duration<double>(now - fps_last_time_).count();
      if (dt >= 0.5) {
        uint64_t swaps = cp ? cp->swap_count() : 0;
        fps_ = double(swaps - fps_last_swaps_) / dt;
        fps_last_swaps_ = swaps;
        fps_last_time_ = now;
      }
    }
  }
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55f);
  if (ImGui::Begin("##status_overlay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoNav)) {
    ImGui::SetWindowFontScale(1.5f);
    if (cvars::show_fps) {
      ImGui::TextColored(
          emulator_window_.emulator_->is_title_open()
              ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f)
              : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
          "%s",
          emulator_window_.emulator_->is_title_open()
              ? fmt::format("{:.0f} FPS", fps_).c_str()
              : "FPS: no title");
    }
    if (!normal_speed) {
      std::string text =
          scalar > 1.0 ? fmt::format(">> FAST-FORWARD {:.2f}x", scalar)
                       : fmt::format("<< SLOW-MOTION {:.2f}x", scalar);
      ImGui::TextColored(scalar > 1.0 ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                                      : ImVec4(0.5f, 0.8f, 1.0f, 1.0f),
                         "%s", text.c_str());
    }
    if (cvars::mute) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", "MUTED");
    }
    ImGui::SetWindowFontScale(1.0f);
  }
  ImGui::End();
}

void EmulatorWindow::CpuBreakIntoDebugger() {
  if (!cvars::debug) {
    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Xenia Debugger",
                                        "Xenia must be launched with the "
                                        "--debug flag in order to enable "
                                        "debugging.");
    return;
  }
  auto processor = emulator()->processor();
  if (processor->execution_state() == cpu::ExecutionState::kRunning) {
    // Currently running, so interrupt (and show the debugger).
    processor->Pause();
  } else {
    // Not running, so just bring the debugger into focus.
    processor->ShowDebugger();
  }
}

void EmulatorWindow::CpuBreakIntoHostDebugger() { xe::debugging::Break(); }

void EmulatorWindow::GpuTraceFrame() {
  emulator()->graphics_system()->RequestFrameTrace();
}

void EmulatorWindow::GpuClearCaches() {
  emulator()->graphics_system()->ClearCaches();
}

void EmulatorWindow::SetFullscreen(bool fullscreen_) {
  if (window_->IsFullscreen() == fullscreen_) {
    return;
  }

  OVERRIDE_bool(fullscreen, fullscreen_);

  window_->SetFullscreen(fullscreen_);
  window_->SetCursorVisibility(fullscreen_
                                   ? ui::Window::CursorVisibility::kAutoHidden
                                   : ui::Window::CursorVisibility::kVisible);
}

void EmulatorWindow::ToggleFullscreen() {
  SetFullscreen(!window_->IsFullscreen());
}

void EmulatorWindow::ToggleDisplayConfigDialog() {
  if (!display_config_dialog_) {
    display_config_dialog_ =
        std::make_unique<DisplayConfigDialog>(imgui_drawer_.get(), *this);
  } else {
    if (display_config_dialog_->IsClosing()) {
      display_config_dialog_.release();
    } else {
      display_config_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleProfilesConfigDialog() {
  if (!profile_config_dialog_) {
    disable_hotkeys_ = true;

    if (emulator_->kernel_state()->xam_state()->IsUIActive()) {
      return;
    }

    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     true);
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(true);

    profile_config_dialog_ =
        std::make_unique<ProfileConfigDialog>(imgui_drawer_.get(), this);
  } else {
    disable_hotkeys_ = false;
    emulator_->kernel_state()->BroadcastNotification(kXNotificationSystemUI,
                                                     false);
    if (profile_config_dialog_->IsClosing()) {
      profile_config_dialog_.release();
    } else {
      profile_config_dialog_.reset();
    }
    emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
  }
}

namespace {
// One QR code as filled rectangles, with a quiet zone, on a white card,
// centred in the window.
void DrawQrCodeCentered(const std::string& text, float module_px) {
  using qrcodegen::QrCode;
  QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
  const int n = qr.getSize();
  const float quiet = module_px * 4.0f;
  const float size = n * module_px + 2.0f * quiet;
  ImGui::SetCursorPosX(
      std::max(0.0f, (ImGui::GetWindowSize().x - size) * 0.5f));
  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  draw_list->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                           IM_COL32(255, 255, 255, 255));
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      if (qr.getModule(x, y)) {
        const float x0 = p.x + quiet + x * module_px;
        const float y0 = p.y + quiet + y * module_px;
        draw_list->AddRectFilled(ImVec2(x0, y0),
                                 ImVec2(x0 + module_px, y0 + module_px),
                                 IM_COL32(0, 0, 0, 255));
      }
    }
  }
  ImGui::Dummy(ImVec2(size, size));
}

void CenteredText(const char* text) {
  ImGui::SetCursorPosX(std::max(
      0.0f,
      (ImGui::GetWindowSize().x - ImGui::CalcTextSize(text).x) * 0.5f));
  ImGui::TextUnformatted(text);
}

// The string under its QR code, selectable for copying; wide enough for
// the whole text, centred.
void CenteredField(const char* id, const std::string& text) {
  std::string buffer = text;
  const float width = ImGui::CalcTextSize(buffer.c_str()).x +
                      ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f;
  ImGui::SetCursorPosX(
      std::max(0.0f, (ImGui::GetWindowSize().x - width) * 0.5f));
  ImGui::SetNextItemWidth(width);
  ImGui::InputText(id, buffer.data(), buffer.size() + 1,
                   ImGuiInputTextFlags_ReadOnly |
                       ImGuiInputTextFlags_AutoSelectAll);
}

// Label + QR code + copyable string, as one centred block.
void QrSection(const char* label, const char* id, const std::string& qr_text,
               const std::string& shown_text, float module_px) {
  CenteredText(label);
  DrawQrCodeCentered(qr_text, module_px);
  CenteredField(id, shown_text);
}
}  // namespace

void EmulatorWindow::SupportDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
  bool dialog_open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Support Development", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  CenteredText("This build is free software by PeerLoom LLC.");
  CenteredText("If you receive value from it, please consider returning value.");
  ImGui::Spacing();
  if (!cvars::support_page_url.empty()) {
    const char* button_label = "Open the support page in the browser...";
    const float button_width = ImGui::CalcTextSize(button_label).x +
                               ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(
        std::max(0.0f, (ImGui::GetWindowSize().x - button_width) * 0.5f));
    if (ImGui::Button(button_label)) {
      LaunchWebBrowser(cvars::support_page_url);
    }
  }
  const float module_px = std::max(3.0f, 3.0f * io.FontGlobalScale);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  if (!cvars::support_btc_address.empty()) {
    QrSection("Bitcoin (on-chain)", "##support_btc",
              "bitcoin:" + cvars::support_btc_address,
              cvars::support_btc_address, module_px);
    ImGui::Spacing();
    ImGui::Spacing();
  }
  if (!cvars::support_lightning_address.empty()) {
    QrSection("Bitcoin (lightning)", "##support_ln",
              cvars::support_lightning_address,
              cvars::support_lightning_address, module_px);
    ImGui::Spacing();
    ImGui::Spacing();
  }
  if (!cvars::support_coffee_url.empty()) {
    QrSection("Buy Me a Coffee (card)", "##support_coffee",
              cvars::support_coffee_url, cvars::support_coffee_url,
              module_px);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  if (!dialog_open) {
    emulator_window_.ToggleSupportDialog();
    return;
  }
}

void EmulatorWindow::ToggleSupportDialog() {
  if (!support_dialog_) {
    support_dialog_ = std::unique_ptr<SupportDialog>(
        new SupportDialog(imgui_drawer_.get(), *this));
  } else {
    support_dialog_.reset();
  }
}

void EmulatorWindow::ToggleXMPConfigDialog() {
  if (!xmp_config_dialog_) {
    xmp_config_dialog_ = std::unique_ptr<XMPConfigDialog>(
        new XMPConfigDialog(imgui_drawer_.get(), *this));
  } else {
    xmp_config_dialog_.reset();
  }
}

void EmulatorWindow::ToggleConsoleSettingsDialog() {
  if (!console_settings_dialog_) {
    console_settings_dialog_ =
        std::unique_ptr<ConsoleSettingsDialog>(new ConsoleSettingsDialog(
            imgui_drawer_.get(), *this, emulator_->kernel_state()->xconfig()));
  } else {
    if (console_settings_dialog_->IsClosing()) {
      console_settings_dialog_.release();
    } else {
      console_settings_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleContentListDialog() {
  if (!content_list_dialog_) {
    content_list_dialog_ = std::unique_ptr<ContentListDialog>(
        new ContentListDialog(imgui_drawer_.get(), *this,
                              emulator_->kernel_state()->content_manager()));
  } else {
    if (content_list_dialog_->IsClosing()) {
      content_list_dialog_.release();
    } else {
      content_list_dialog_.reset();
    }
  }
}

void EmulatorWindow::ToggleControllerVibration() {
  auto input_sys = emulator()->input_system();
  if (input_sys) {
    auto input_lock = input_sys->lock();

    input_sys->ToggleVibration();

    if (emulator_->kernel_state()) {
      emulator_->kernel_state()->BroadcastNotification(
          kXNotificationSystemProfileSettingChanged,
          static_cast<uint32_t>(input_sys->GetConnectedSlots().count()));
    }
  }
}

void EmulatorWindow::ShowCompatibility() {
  const std::string_view base_url =
      "https://github.com/xenia-canary/game-compatibility/issues";
  std::string url;
  // Avoid searching for a title ID of "00000000".
  uint32_t title_id = emulator_->title_id();
  if (!title_id) {
    url = base_url;
  } else {
    url = fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
  }
  LaunchWebBrowser(url);
}

void EmulatorWindow::ShowFAQ() {
  LaunchWebBrowser("https://github.com/xenia-canary/xenia-canary/wiki/FAQ");
}

void EmulatorWindow::ShowBuildCommit() {
#ifdef XE_BUILD_IS_PR
  LaunchWebBrowser(
      "https://github.com/xenia-canary/xenia-canary/pull/" XE_BUILD_PR_NUMBER);
#else
  // This fork's commits live on the fork, not upstream.
  LaunchWebBrowser(
      "https://github.com/peerloomllc/xenia-canary/commit/" XE_BUILD_COMMIT);
#endif
}

void EmulatorWindow::UpdateTitle() {
  xe::StringBuffer sb;
  sb.Append(base_title_);

  // Title information, if available
  if (emulator()->is_title_open()) {
    sb.AppendFormat(" | [{:08X}", emulator()->title_id());
    auto title_version = emulator()->title_version();
    if (!title_version.empty()) {
      sb.Append(" v");
      sb.Append(title_version);
    }
    sb.Append("]");

    auto title_name = emulator()->title_name();
    if (!title_name.empty()) {
      sb.Append(" ");
      sb.Append(title_name);
    }
  }

  // Graphics system name, if available
  auto graphics_system = emulator()->graphics_system();
  if (graphics_system) {
    auto graphics_name = graphics_system->name();
    if (!graphics_name.empty()) {
      sb.Append(" <");
      sb.Append(graphics_name);
      sb.Append(">");
    }
  }

  if (Clock::guest_time_scalar() != 1.0) {
    sb.AppendFormat(" (@{:.2f}x)", Clock::guest_time_scalar());
  }

  if (initializing_shader_storage_) {
    sb.Append(" (Preloading shaders\u2026)");
  }

  patcher::Patcher* patcher = emulator()->patcher();
  if (patcher && patcher->IsAnyPatchApplied()) {
    sb.Append(" [Patches Applied]");
  }

  patcher::PluginLoader* pluginloader = emulator()->plugin_loader();
  if (pluginloader && pluginloader->IsAnyPluginLoaded()) {
    sb.Append(" [Plugins Loaded]");
  }

  window_->SetTitle(sb.to_string_view());
}

void EmulatorWindow::SetInitializingShaderStorage(bool initializing) {
  if (initializing_shader_storage_ == initializing) {
    return;
  }
  initializing_shader_storage_ = initializing;
  UpdateTitle();
}

// Notes:
// SDL and XInput both support the guide button
//
// Assumes titles do not use the guide button.
// For titles that do such as dashboards these titles could be excluded based on
// their title ID.
//
// Xbox Gamebar:
// If the Xbox Gamebar overlay is enabled Windows will consume the guide
// button's input, this can be seen using hid-demo.
//
// Workaround: Detect if the Xbox Gamebar overlay is enabled then use the BACK
// button instead of the GUIDE button. Therefore BACK and GUIDE are reserved
// buttons for hotkeys.
//
// This is not an issue with DualShock controllers because Windows will not
// open the gamebar overlay using the PlayStation menu button.
//
// Xbox One S Controller:
// The guide button on this controller is very buggy no idea why.
// Using xinput usually registers after a double tap.
// Doesn't work at all using SDL.
// Needs more testing.
//
// Steam:
// If guide button focus is enabled steam will open.
// Steam uses BACK + GUIDE to open an On-Screen keyboard, however this is not a
// problem since both these buttons are reserved.
const std::map<int, EmulatorWindow::ControllerHotKey> controller_hotkey_map = {
    // Must use the Guide Button for all pass through hotkeys
    {X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ReadbackResolve,
         "A + Guide = Toggle Readback Resolve", true)},
    {X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "B + Guide = Toggle between loglevel set in config and the 'Disabled' "
         "loglevel.",
         true, true)},
    {X_INPUT_GAMEPAD_Y | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleFullscreen,
         "Y + Guide = Toggle Fullscreen", true)},
    {X_INPUT_GAMEPAD_X | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearMemoryPageState,
         "X + Guide = Toggle Clear Memory Page State", true)},

    {X_INPUT_GAMEPAD_RIGHT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ClearGPUCache,
         "Right Shoulder + Guide = Clear GPU Cache", true)},
    {X_INPUT_GAMEPAD_LEFT_SHOULDER | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleControllerVibration,
         "Left Shoulder + Guide = Toggle Controller Vibration", true)},

    // CPU Time Scalar with no rumble feedback
    {X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetHalf,
         "D-PAD Down + Guide = Half CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarSetDouble,
         "D-PAD Up + Guide = Double CPU Scalar")},
    {X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_GUIDE,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::CpuTimeScalarReset,
         "D-PAD Right + Guide = Reset CPU Scalar")},

    // non-pass through hotkeys
    {X_INPUT_GAMEPAD_Y, EmulatorWindow::ControllerHotKey(
                            EmulatorWindow::ButtonFunctions::ToggleFullscreen,
                            "Y = Toggle Fullscreen", true, false)},
    {X_INPUT_GAMEPAD_START, EmulatorWindow::ControllerHotKey(
                                EmulatorWindow::ButtonFunctions::RunTitle,
                                "Start = Run Selected Title", false, false)},
    {X_INPUT_GAMEPAD_BACK | X_INPUT_GAMEPAD_START,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::ToggleLogging,
         "Back + Start = Toggle between loglevel set in config and the "
         "'Disabled' loglevel.",
         false, false)},
    {X_INPUT_GAMEPAD_DPAD_DOWN,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::IncTitleSelect,
         "D-PAD Down = Title Selection +1", true, false)},
    {X_INPUT_GAMEPAD_DPAD_UP,
     EmulatorWindow::ControllerHotKey(
         EmulatorWindow::ButtonFunctions::DecTitleSelect,
         "D-PAD Up = Title Selection -1", true, false)}};

EmulatorWindow::ControllerHotKey EmulatorWindow::ProcessControllerHotkey(
    int buttons) {
  // Default return value
  EmulatorWindow::ControllerHotKey Unknown_hotkey = {};

  if (buttons == 0) {
    return Unknown_hotkey;
  }

  if (disable_hotkeys_.load()) {
    return Unknown_hotkey;
  }

  // Hotkey cool-down to prevent toggling too fast
  constexpr std::chrono::milliseconds delay(75);

  // If the Xbox Gamebar is enabled or the Guide button is disabled then
  // replace the Guide button with the Back button without redeclaring the key
  // mappings
  if (IsUseNexusForGameBarEnabled() || !cvars::guide_button) {
    if ((buttons & X_INPUT_GAMEPAD_BACK) == X_INPUT_GAMEPAD_BACK) {
      buttons &= ~X_INPUT_GAMEPAD_BACK;
      buttons |= X_INPUT_GAMEPAD_GUIDE;
    }
  }

  auto it = controller_hotkey_map.find(buttons);
  if (it == controller_hotkey_map.end()) {
    return Unknown_hotkey;
  }

  // Do not activate hotkeys that are not intended for activation during
  // gameplay
  if (emulator_->is_title_open()) {
    // If non-pass through (menu hoykeys) or hotkeys disabled then return
    if (!it->second.title_passthru || !cvars::controller_hotkeys) {
      return Unknown_hotkey;
    }
  }

  std::string notificationTitle = "";
  std::string notificationDesc = "";

  EmulatorWindow::ControllerHotKey button_combination = it->second;

  switch (button_combination.function) {
    case ButtonFunctions::ToggleFullscreen:
      app_context().CallInUIThread([this]() { ToggleFullscreen(); });

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::RunTitle: {
      if (selected_title_index == -1) {
        selected_title_index++;
      }

      if (selected_title_index < recently_launched_titles_.size()) {
        app_context().CallInUIThread([this]() {
          RunTitle(
              recently_launched_titles_[selected_title_index].path_to_file);
        });
      }
    } break;
    case ButtonFunctions::ClearMemoryPageState:
      ToggleGPUSetting(GPUSetting::ClearMemoryPageState);

      // Assume the user wants ClearCaches as well
      if (cvars::clear_memory_page_state) {
        GpuClearCaches();
      }

      notificationTitle = "Toggle Clear Memory Page State";
      notificationDesc =
          cvars::clear_memory_page_state ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::ReadbackResolve:
      CycleReadbackResolve();

      notificationTitle = "Readback Resolve Mode";
      notificationDesc = cvars::readback_resolve;

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::CpuTimeScalarSetHalf:
      CpuTimeScalarSetHalf();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Decreased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarSetDouble:
      CpuTimeScalarSetDouble();

      notificationTitle = "Time Scalar";
      notificationDesc =
          fmt::format("Increased to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::CpuTimeScalarReset:
      CpuTimeScalarReset();

      notificationTitle = "Time Scalar";
      notificationDesc = fmt::format("Reset to {}", Clock::guest_time_scalar());
      break;
    case ButtonFunctions::ClearGPUCache:
      GpuClearCaches();

      notificationTitle = "Clear GPU Cache";
      notificationDesc = "Complete";

      // Extra Sleep
      xe::threading::Sleep(delay);
      break;
    case ButtonFunctions::ToggleControllerVibration: {
      ToggleControllerVibration();

      bool vibration = false;

      auto input_sys = emulator()->input_system();
      if (input_sys) {
        vibration = input_sys->GetVibrationCvar();
      }

      notificationTitle = "Toggle Controller Vibration";
      notificationDesc = vibration ? "Enabled" : "Disabled";

      // Extra Sleep
      xe::threading::Sleep(delay);
    } break;
    case ButtonFunctions::IncTitleSelect:
      selected_title_index++;
      break;
    case ButtonFunctions::DecTitleSelect:
      selected_title_index--;
      break;
    case ButtonFunctions::ToggleLogging: {
      logging::ToggleLogLevel();

      notificationTitle = "Toggle Logging";

      LogLevel level = static_cast<LogLevel>(logging::internal::GetLogLevel());
      notificationDesc = level == LogLevel::Disabled ? "Disabled" : "Enabled";
    } break;
    case ButtonFunctions::Unknown:
    default:
      break;
  }

  if ((button_combination.function == ButtonFunctions::IncTitleSelect ||
       button_combination.function == ButtonFunctions::DecTitleSelect) &&
      recently_launched_titles_.size() > 0) {
    selected_title_index =
        std::clamp(selected_title_index, 0,
                   static_cast<int32_t>(recently_launched_titles_.size() - 1));

    // Must clear dialogs to prevent stacking
    ClearDialogs();

    // Titles may contain Unicode characters such as At World’s End
    // Must use ImGUI font that can render these Unicode characters
    std::string title_name;

    // Use filename if title name is empty
    if (recently_launched_titles_[selected_title_index].title_name.empty()) {
      title_name = recently_launched_titles_[selected_title_index]
                       .path_to_file.filename()
                       .string();
    } else {
      title_name = recently_launched_titles_[selected_title_index].title_name;
    }

    std::string title = fmt::format(
        "{}: {}\n\n{}", selected_title_index + 1, title_name,
        controller_hotkey_map.find(X_INPUT_GAMEPAD_START)->second.pretty);

    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Title Selection",
                                        title);
  }

  if (!notificationTitle.empty()) {
    app_context_.CallInUIThread(
        [imgui_drawer = imgui_drawer(), notificationTitle, notificationDesc]() {
          new xe::ui::HostNotificationWindow(imgui_drawer, notificationTitle,
                                             notificationDesc, 0);
        });
  }

  xe::threading::Sleep(delay);

  return it->second;
}

void EmulatorWindow::VibrateController(xe::hid::InputSystem* input_sys,
                                       uint32_t user_index,
                                       bool toggle_rumble) {
  constexpr std::chrono::milliseconds rumble_duration(100);

  // Hold lock while sleeping this thread for the duration of the rumble,
  // otherwise the rumble may fail.
  auto input_lock = input_sys->lock();

  X_INPUT_VIBRATION vibration = {};

  vibration.left_motor_speed = toggle_rumble ? UINT16_MAX : 0;
  vibration.right_motor_speed = toggle_rumble ? UINT16_MAX : 0;

  input_sys->SetState(user_index, &vibration);

  // Vibration duration
  if (toggle_rumble) {
    xe::threading::Sleep(rumble_duration);
  }
}

void EmulatorWindow::GamepadHotKeys() {
  X_INPUT_STATE state;

  constexpr std::chrono::milliseconds thread_delay(75);

  auto input_sys = emulator_->input_system();

  if (input_sys) {
    while (true) {
      // Collect controller states while holding the lock
      std::array<std::pair<bool, X_INPUT_STATE>, XUserMaxUserCount>
          controller_states;
      {
        auto input_lock = input_sys->lock();
        for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
             ++user_index) {
          X_RESULT result = input_sys->GetState(
              user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD, &state);
          controller_states[user_index] = {result == X_ERROR_SUCCESS, state};
        }
      }  // Lock is released here when input_lock goes out of scope

      // Process hotkeys without holding the lock
      for (uint32_t user_index = 0; user_index < XUserMaxUserCount;
           ++user_index) {
        if (controller_states[user_index].first) {
          if (ProcessControllerHotkey(
                  controller_states[user_index].second.gamepad.buttons)
                  .rumble) {
            // Enable Vibration
            VibrateController(input_sys, user_index, true);

            // Disable Vibration
            VibrateController(input_sys, user_index, false);
          }
        }
      }

      xe::threading::Sleep(thread_delay);
    }
  }
}

void EmulatorWindow::ToggleGPUSetting(gpu::GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      SaveGPUSetting(GPUSetting::ClearMemoryPageState,
                     !cvars::clear_memory_page_state);
      break;
    case GPUSetting::ReadbackMemexport:
      SaveGPUSetting(GPUSetting::ReadbackMemexport, !cvars::readback_memexport);
      break;
  }
}

void EmulatorWindow::CycleReadbackResolve() {
  const std::string& current = cvars::readback_resolve;
  if (current == "fast") {
    gpu::SetReadbackResolveMode("full");
  } else if (current == "full") {
    gpu::SetReadbackResolveMode("none");
  } else {
    gpu::SetReadbackResolveMode("fast");
  }
}

void EmulatorWindow::DisplayHotKeysConfig() {
  std::string msg = "";
  std::string msg_passthru = "";

  bool guide_enabled = !IsUseNexusForGameBarEnabled() && cvars::guide_button;

  for (auto const& [key, val] : controller_hotkey_map) {
    std::string pretty_text = val.pretty;

    if (!guide_enabled) {
      pretty_text = std::regex_replace(
          pretty_text, std::regex("Guide", std::regex_constants::icase),
          "Back");
    }

    if (emulator_->is_title_open() && !val.title_passthru) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru && !cvars::controller_hotkeys) {
      pretty_text += " (Disabled)";
    }

    if (val.title_passthru) {
      msg += pretty_text + "\n";
    } else {
      msg_passthru += pretty_text + "\n";
    }
  }

  // Add Title
  msg.insert(0, "Gameplay Hotkeys\n");

  // Prepend non-passthru hotkeys
  msg_passthru += "\n";
  msg.insert(0, msg_passthru);
  msg += "\n";

  msg += "Readback Resolve: " + cvars::readback_resolve;
  msg += "\n";

  msg += "Clear Memory Page State: " +
         xe::string_util::BoolToString(cvars::clear_memory_page_state);
  msg += "\n";

  msg += "Controller Hotkeys: " +
         xe::string_util::BoolToString(cvars::controller_hotkeys);

  ClearDialogs();
  xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(), "Controller Hotkeys",
                                      msg);
}

std::string EmulatorWindow::CanonicalizeFileExtension(
    const std::filesystem::path& path) {
  return xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
}

xe::X_STATUS EmulatorWindow::RunTitle(
    const std::filesystem::path& path_to_file) {
  std::error_code ec = {};
  bool titleExists = std::filesystem::exists(path_to_file, ec);

  if (path_to_file.empty() || !titleExists) {
    std::string log_msg =
        fmt::format("Failed to launch title path is {}.",
                    path_to_file.empty() ? "empty" : "invalid");

    if (!path_to_file.empty() && !titleExists) {
      log_msg.append(fmt::format("\nProvided Path: {}", path_to_file));
    }

    if (ec) {
      log_msg.append(fmt::format("\nExtended message info: {} ({:08X})",
                                 ec.message(), ec.value()));
    }

    XELOGE("{}", log_msg);

    ClearDialogs();

    xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_.get(),
                                        "Title Launch Failed!", log_msg);

    return X_STATUS_NO_SUCH_FILE;
  }

  if (!emulator_ready_) {
    XELOGI("RunTitle: the emulator is still starting up; {} queued",
           path_to_file.string());
    pending_launch_path_ = path_to_file;
    return X_STATUS_SUCCESS;
  }

  if (emulator_->is_title_open()) {
    // A title cannot be replaced in-process: the old one's guest memory is
    // never reclaimed (the heaps are shared with the kernel's own), so the
    // next title's allocations fail and it freezes within seconds (tried).
    // Start a fresh emulator process with the new path and close this one.
    if (RelaunchProcess(path_to_file)) {
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_UNSUCCESSFUL;
  }

  // Prevent crashing the emulator by not loading a game if a game is already
  // loaded.
  auto abs_path = std::filesystem::absolute(path_to_file);

  auto extension = CanonicalizeFileExtension(abs_path);

  if (extension == ".7z" || extension == ".zip" || extension == ".rar" ||
      extension == ".tar" || extension == ".gz") {
    xe::ShowSimpleMessageBox(
        xe::SimpleMessageBoxType::Error,
        fmt::format(
            "Unsupported format!\n"
            "Xenia does not support running software in an archived format."));

    return X_STATUS_UNSUCCESSFUL;
  }

  auto result = emulator_->LaunchPath(abs_path);

  disable_hotkeys_ = false;

  ClearDialogs();

  if (result) {
    XELOGE("Failed to launch target: {:08X}", result);

    xe::ui::ImGuiDialog::ShowMessageBox(
        imgui_drawer_.get(), "Title Launch Failed!",
        "Failed to launch title.\n\nCheck xenia.log for technical details.");

    emulator_->file_system()->Clear();
  } else {
    AddRecentlyLaunchedTitle(path_to_file, emulator_->title_name());
    last_launched_path_ = path_to_file;
#if XE_PLATFORM_LINUX
    SaveTitleIcon();
    OnDashboardTitleLaunched();
#endif

    auto xam =
        emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
            "xam.xex");

    xam->loader_data().host_path = xe::path_to_utf8(abs_path);
  }

  return result;
}

void EmulatorWindow::RunPreviouslyPlayedTitle() {
  if (recently_launched_titles_.size() >= 1) {
    RunTitle(recently_launched_titles_[0].path_to_file);
  }
}

void EmulatorWindow::FillRecentlyLaunchedTitlesMenu(
    xe::ui::MenuItem* recent_menu) {
  for (int i = 0; i < recently_launched_titles_.size(); ++i) {
    std::string hotkey = (i == 0) ? "F9" : "";

    const RecentTitleEntry& entry = recently_launched_titles_[i];
    const std::string item_text = entry.title_name.empty()
                                      ? entry.path_to_file.string()
                                      : entry.title_name;

    recent_menu->AddChild(MenuItem::Create(
        MenuItem::Type::kString, item_text, hotkey,
        std::bind(&EmulatorWindow::RunTitle, this, entry.path_to_file)));
  }
}

void EmulatorWindow::LoadRecentlyLaunchedTitles() {
  std::ifstream file(emulator()->storage_root() /
                     kRecentlyPlayedTitlesFilename);
  if (!file.is_open()) {
    return;
  }

  toml::parse_result parsed_file;
  try {
    parsed_file = toml::parse(file);
  } catch (toml::parse_error& exception) {
    XELOGE("Cannot parse file: recent.toml. Error: {}", exception.what());
    return;
  }

  if (parsed_file.is_table()) {
    for (const auto& [index, entry] : *parsed_file.as_table()) {
      if (!entry.is_table()) {
        continue;
      }

      const toml::table* entry_table = entry.as_table();

      std::string title_name =
          entry_table->get_as<std::string>("title_name")->get();
      std::string path = entry_table->get_as<std::string>("path")->get();
      std::time_t last_run_time =
          entry_table->get_as<int64_t>("last_run_time")->get();

      std::error_code ec = {};
      if (path.empty() || !std::filesystem::exists(path, ec)) {
        continue;
      }

      recently_launched_titles_.push_back({title_name, path, last_run_time});
    }
  }
}

void EmulatorWindow::AddRecentlyLaunchedTitle(
    std::filesystem::path path_to_file, std::string title_name) {
  if (cvars::recent_titles_entry_amount <= 0) {
    return;
  }

  // Check if game is already on list and pop it to front
  auto entry_index =
      std::ranges::find_if(std::as_const(recently_launched_titles_),
                           [&title_name](const RecentTitleEntry& entry) {
                             return entry.title_name == title_name;
                           });
  if (entry_index != recently_launched_titles_.cend()) {
    recently_launched_titles_.erase(entry_index);
  }

  recently_launched_titles_.insert(recently_launched_titles_.cbegin(),
                                   {title_name, path_to_file, time(nullptr)});
  // Serialize to toml
  auto toml_table = toml::table();

  uint8_t index = 0;
  for (const RecentTitleEntry& entry : recently_launched_titles_) {
    auto entry_table = toml::table();

    // Fill entry under specific index.
    std::string str_path = xe::path_to_utf8(entry.path_to_file);
    entry_table.insert("title_name", entry.title_name);
    entry_table.insert("path", str_path);
    entry_table.insert("last_run_time", entry.last_run_time);

    toml_table.insert(std::to_string(index++), entry_table);

    if (index >= cvars::recent_titles_entry_amount) {
      break;
    }
  }
  // Open and write serialized data.
  std::ofstream file(emulator()->storage_root() / kRecentlyPlayedTitlesFilename,
                     std::ofstream::trunc);
  file << toml_table;
  file.close();
}

void EmulatorWindow::ClearDialogs() {
  if (profile_config_dialog_) {
    profile_config_dialog_.reset();
  }
  slot_overlay_.reset();

  if (display_config_dialog_) {
    display_config_dialog_.reset();
  }

  if (console_settings_dialog_) {
    console_settings_dialog_.reset();
  }

  if (content_list_dialog_) {
    content_list_dialog_.reset();
  }

  if (xmp_config_dialog_) {
    xmp_config_dialog_.reset();
  }

  imgui_drawer_.get()->ClearDialogs();
  // The status overlay (speed, mute, FPS) went with them; put it back if
  // anything still calls for it.
  status_overlay_.reset();
  UpdateStatusOverlay(nullptr);
  emulator_->kernel_state()->xam_state()->is_xam_dialog_present_.store(false);
}

}  // namespace app
}  // namespace xe

#if XE_PLATFORM_LINUX
// Display > Settings window...: GTK, on the UI thread.

#include <gtk/gtk.h>

#include "xenia/ui/window_gtk.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/file.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe {
namespace app {
namespace {

// A widget's action, attached to it and freed with it.
struct SettingsCallback {
  std::function<void(GtkWidget*)> fn;
};

void OnSettingsWidget(GtkWidget* widget, gpointer) {
  auto* cb = static_cast<SettingsCallback*>(
      g_object_get_data(G_OBJECT(widget), "xe-callback"));
  if (cb && cb->fn) {
    cb->fn(widget);
  }
}

void AttachSettingsCallback(GtkWidget* widget, const char* signal,
                            std::function<void(GtkWidget*)> fn) {
  auto* cb = new SettingsCallback{std::move(fn)};
  g_object_set_data_full(G_OBJECT(widget), "xe-callback", cb, [](gpointer p) {
    delete static_cast<SettingsCallback*>(p);
  });
  g_signal_connect(widget, signal, G_CALLBACK(OnSettingsWidget), nullptr);
}

void SetTooltipFromCvar(GtkWidget* widget, const char* name) {
  if (!cvar::ConfigVars) {
    return;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it != cvar::ConfigVars->end()) {
    std::string text = it->second->description() + "\n(" + name + ")";
    gtk_widget_set_tooltip_text(widget, text.c_str());
  }
}

GtkWidget* NewGrid() {
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
  return grid;
}

// A collapsible section: a bold-titled expander holding a fresh grid, packed
// into box. Returns the grid.
GtkWidget* NewSection(GtkWidget* box, const char* title, bool expanded) {
  GtkWidget* expander = gtk_expander_new(nullptr);
  GtkWidget* label = gtk_label_new(nullptr);
  gchar* escaped = g_markup_escape_text(title, -1);
  gtk_label_set_markup(GTK_LABEL(label),
                       (std::string("<b>") + escaped + "</b>").c_str());
  g_free(escaped);
  gtk_expander_set_label_widget(GTK_EXPANDER(expander), label);
  gtk_expander_set_expanded(GTK_EXPANDER(expander), expanded);
  gtk_widget_set_margin_top(expander, 4);
  GtkWidget* grid = NewGrid();
  gtk_container_add(GTK_CONTAINER(expander), grid);
  gtk_box_pack_start(GTK_BOX(box), expander, FALSE, FALSE, 0);
  return grid;
}

GtkWidget* LeftLabel(const char* text) {
  GtkWidget* label = gtk_label_new(text);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  // Wrap rather than demand the width of the longest line: an unwrapped
  // explanatory label sets the page's minimum width, which the notebook
  // passes to the window, and the window can then never be made narrower.
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  return label;
}

// The cvar's description, on a small icon beside the setting's name rather
// than on the control itself: a tooltip that only appears when the pointer
// happens to rest on a combo box is not discoverable, and hovering a control
// to read about it invites changing it by accident.
GtkWidget* HelpIcon(const char* name) {
  GtkWidget* icon =
      gtk_image_new_from_icon_name("help-about-symbolic", GTK_ICON_SIZE_MENU);
  gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
  gtk_widget_set_opacity(icon, 0.55);
  SetTooltipFromCvar(icon, name);
  return icon;
}

// A settings row's first column: the name, then the help icon.
GtkWidget* LabelWithHelp(const char* text, const char* name) {
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(row), LeftLabel(text), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), HelpIcon(name), FALSE, FALSE, 0);
  gtk_widget_set_halign(row, GTK_ALIGN_START);
  return row;
}

// Every settings tab goes through this: a scrolled window has a small
// minimum size of its own, so whatever the page wants, the window stays
// freely resizable in both directions.
GtkWidget* TabScroller(GtkWidget* child) {
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroller), child);
  return scroller;
}

GtkWidget* HeadingLabel(const char* text) {
  GtkWidget* label = gtk_label_new(nullptr);
  std::string markup = std::string("<b>") + text + "</b>";
  gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_margin_top(label, 8);
  return label;
}

void AddCheck(GtkWidget* grid, int& row, const char* label, const char* name,
              bool value) {
  GtkWidget* check = gtk_check_button_new_with_label(label);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), value);
  std::string cvar_name = name;
  AttachSettingsCallback(check, "toggled", [cvar_name](GtkWidget* w) {
    SetGpuOption<bool>(cvar_name.c_str(),
                       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)));
  });
  GtkWidget* check_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(check_row), check, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(check_row), HelpIcon(name), FALSE, FALSE, 0);
  gtk_widget_set_halign(check_row, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), check_row, 0, row++, 2, 1);
}

// Combo over (value, label) pairs; on_change gets the chosen value.
void AddCombo(GtkWidget* grid, int& row, const char* label, const char* name,
              const std::vector<std::pair<std::string, std::string>>& choices,
              const std::string& current,
              std::function<void(const std::string&)> on_change) {
  gtk_grid_attach(GTK_GRID(grid), LabelWithHelp(label, name), 0, row, 1, 1);
  GtkWidget* combo = gtk_combo_box_text_new();
  int active = 0;
  for (size_t i = 0; i < choices.size(); ++i) {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                   choices[i].second.c_str());
    if (choices[i].first == current) {
      active = int(i);
    }
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
  gtk_widget_set_hexpand(combo, TRUE);
  AttachSettingsCallback(combo, "changed", [choices, on_change](GtkWidget* w) {
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    if (index >= 0 && index < int(choices.size())) {
      on_change(choices[index].first);
    }
  });
  gtk_grid_attach(GTK_GRID(grid), combo, 1, row++, 1, 1);
}

void AddSpin(GtkWidget* grid, int& row, const char* label, const char* name,
             double value, double min_value, double max_value,
             std::function<void(double)> on_change) {
  gtk_grid_attach(GTK_GRID(grid), LabelWithHelp(label, name), 0, row, 1, 1);
  GtkWidget* spin = gtk_spin_button_new_with_range(min_value, max_value, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  gtk_widget_set_halign(spin, GTK_ALIGN_START);
  AttachSettingsCallback(spin, "value-changed", [on_change](GtkWidget* w) {
    on_change(gtk_spin_button_get_value(GTK_SPIN_BUTTON(w)));
  });
  gtk_grid_attach(GTK_GRID(grid), spin, 1, row++, 1, 1);
}

// Horizontal slider with the value shown; on_change fires on every step of a
// drag, so callers should use SetGpuOptionDeferred.
void AddScale(GtkWidget* grid, int& row, const char* label, const char* name,
              double value, double min_value, double max_value, double step,
              std::function<void(double)> on_change) {
  gtk_grid_attach(GTK_GRID(grid), LeftLabel(label), 0, row, 1, 1);
  GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                              min_value, max_value, step);
  gtk_scale_set_digits(GTK_SCALE(scale), 2);
  gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
  gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_widget_set_hexpand(scale, TRUE);
  SetTooltipFromCvar(scale, name);
  AttachSettingsCallback(scale, "value-changed", [on_change](GtkWidget* w) {
    on_change(gtk_range_get_value(GTK_RANGE(w)));
  });
  gtk_grid_attach(GTK_GRID(grid), scale, 1, row++, 1, 1);
}

// Override a cvar now and write the config file 500 ms after the last change
// (a slider drag fires dozens of changes a second).
guint deferred_config_save_source = 0;
template <typename T>
void SetGpuOptionDeferred(const char* name, const T& value) {
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return;
  }
  auto* var = dynamic_cast<cvar::ConfigVar<T>*>(it->second);
  if (!var) {
    return;
  }
  var->OverrideConfigValue(value);
  if (deferred_config_save_source) {
    g_source_remove(deferred_config_save_source);
  }
  deferred_config_save_source = g_timeout_add(
      500,
      [](gpointer) -> gboolean {
        deferred_config_save_source = 0;
        config::SaveConfig();
        return G_SOURCE_REMOVE;
      },
      nullptr);
}

// ---- Patches tab helpers ----

// Run a shell command and return its stdout; exit code in *exit_code.
std::string RunCommandCapture(const std::string& command, int* exit_code) {
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    if (exit_code) {
      *exit_code = -1;
    }
    return output;
  }
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output.append(buffer, n);
  }
  int status = pclose(pipe);
  if (exit_code) {
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  return output;
}

std::string ShellQuote(const std::string& text) {
  std::string quoted = "'";
  for (char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

// GitHub's blob id of a local file: sha1 of "blob <size>\0" + contents. Used
// to tell an up-to-date community patch file from an updated one.
std::string GitBlobSha(const std::filesystem::path& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return "";
  }
  int code = 0;
  std::string out = RunCommandCapture(
      fmt::format("(printf 'blob {}\\0'; cat {}) | sha1sum", size,
                  ShellQuote(path.string())),
      &code);
  if (code != 0 || out.size() < 40) {
    return "";
  }
  return out.substr(0, 40);
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Unescape a JSON string body (\" \\ \/ \n \t \uXXXX with surrogate pairs).
std::string JsonUnescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c != '\\' || i + 1 >= in.size()) {
      out += c;
      continue;
    }
    char e = in[++i];
    switch (e) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        if (i + 4 >= in.size()) {
          return out;
        }
        uint32_t cp = uint32_t(strtoul(in.substr(i + 1, 4).c_str(), nullptr, 16));
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < in.size() &&
            in[i + 1] == '\\' && in[i + 2] == 'u') {
          uint32_t low =
              uint32_t(strtoul(in.substr(i + 3, 4).c_str(), nullptr, 16));
          if (low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        AppendUtf8(out, cp);
        break;
      }
      default:
        out += e;  // \" \\ \/ and anything else
        break;
    }
  }
  return out;
}

// Percent-encode a path component for a raw.githubusercontent.com URL.
std::string UrlEncodeComponent(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Flip is_enabled for the [[patch]] entry named patch_name in a patch file,
// keeping every other byte of the file (community files carry comments).
bool SetPatchEnabledInFile(const std::filesystem::path& path,
                           const std::string& patch_name, bool enabled) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();
  auto trimmed = [](const std::string& l) {
    size_t b = l.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : l.substr(b);
  };
  auto quoted_value = [](const std::string& t) {
    size_t q1 = t.find('"');
    size_t q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string() : t.substr(q1 + 1, q2 - q1 - 1);
  };
  bool in_patch = false;
  bool in_target = false;
  size_t name_line = std::string::npos;
  bool done = false;
  for (size_t i = 0; i < lines.size() && !done; ++i) {
    std::string t = trimmed(lines[i]);
    if (t.rfind("[[patch]]", 0) == 0) {
      if (in_target && name_line != std::string::npos) {
        break;  // the target had no is_enabled line: insert after its name
      }
      in_patch = true;
      in_target = false;
      continue;
    }
    if (t.rfind("[[", 0) == 0 || t.rfind("[", 0) == 0) {
      if (in_target && name_line != std::string::npos) {
        break;
      }
      in_patch = false;  // a data table such as [[patch.be32]]
      continue;
    }
    if (!in_patch) {
      continue;
    }
    if (!in_target && t.rfind("name", 0) == 0 &&
        t.find('=') != std::string::npos && quoted_value(t) == patch_name) {
      in_target = true;
      name_line = i;
      continue;
    }
    if (in_target && t.rfind("is_enabled", 0) == 0) {
      size_t indent = lines[i].find_first_not_of(" \t");
      std::string prefix =
          indent == std::string::npos ? "" : lines[i].substr(0, indent);
      size_t hash = lines[i].find('#');
      std::string comment =
          hash == std::string::npos ? "" : " " + lines[i].substr(hash);
      lines[i] = prefix + "is_enabled = " + (enabled ? "true" : "false") +
                 comment;
      done = true;
    }
  }
  if (!done) {
    if (name_line == std::string::npos) {
      return false;
    }
    size_t indent = lines[name_line].find_first_not_of(" \t");
    std::string prefix =
        indent == std::string::npos ? "" : lines[name_line].substr(0, indent);
    lines.insert(lines.begin() + name_line + 1,
                 prefix + "is_enabled = " + (enabled ? "true" : "false"));
  }
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  for (const std::string& l : lines) {
    out << l << '\n';
  }
  return bool(out);
}

// Blob ids of the community files downloaded through the tab, one
// "<sha> <file name>" per line in the storage root, so a file whose
// is_enabled flags were toggled still counts as up to date.
std::filesystem::path CommunityShaFile(const std::filesystem::path& root) {
  return root / "community_patch_shas.txt";
}

std::map<std::string, std::string> LoadCommunityShas(
    const std::filesystem::path& root) {
  std::map<std::string, std::string> shas;
  std::ifstream in(CommunityShaFile(root));
  std::string line;
  while (std::getline(in, line)) {
    size_t space = line.find(' ');
    if (space == 40) {
      shas[line.substr(41)] = line.substr(0, 40);
    }
  }
  return shas;
}

void RecordCommunitySha(const std::filesystem::path& root,
                        const std::string& name, const std::string& sha) {
  auto shas = LoadCommunityShas(root);
  shas[name] = sha;
  std::ofstream out(CommunityShaFile(root), std::ios::trunc);
  for (const auto& [n, s] : shas) {
    out << s << ' ' << n << '\n';
  }
}

// Blob id of the file with every "is_enabled = true" set back to false (the
// repository ships them all off), for copies that were not downloaded here.
std::string NormalisedBlobSha(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  static const std::regex enabled_true("(^|\n)([ \t]*is_enabled[ \t]*=[ \t]*)true");
  content = std::regex_replace(content, enabled_true, "$1$2false");
  std::error_code ec;
  std::filesystem::path temp =
      std::filesystem::temp_directory_path(ec) / "xenia_patch_sha.tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << content;
  }
  std::string sha = GitBlobSha(temp);
  std::filesystem::remove(temp, ec);
  return sha;
}

enum class CommunityFileState { kMissing, kOutdated, kCurrent };
CommunityFileState StateOfCommunityFile(
    const std::filesystem::path& root, const std::string& name,
    const std::string& tree_sha,
    const std::map<std::string, std::string>& recorded) {
  std::filesystem::path local = root / "patches" / name;
  if (!std::filesystem::exists(local)) {
    return CommunityFileState::kMissing;
  }
  auto it = recorded.find(name);
  if (it != recorded.end()) {
    return it->second == tree_sha ? CommunityFileState::kCurrent
                                  : CommunityFileState::kOutdated;
  }
  return NormalisedBlobSha(local) == tree_sha ? CommunityFileState::kCurrent
                                              : CommunityFileState::kOutdated;
}

struct IdleCall {
  std::function<void()> fn;
};
gboolean RunIdleCall(gpointer data) {
  auto* call = static_cast<IdleCall*>(data);
  call->fn();
  delete call;
  return G_SOURCE_REMOVE;
}
void PostToUIThread(std::function<void()> fn) {
  g_idle_add(RunIdleCall, new IdleCall{std::move(fn)});
}

void ClearChildren(GtkWidget* container) {
  GList* children = gtk_container_get_children(GTK_CONTAINER(container));
  for (GList* l = children; l; l = l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(children);
}

const char* kCommunityPatchesTreeUrl =
    "https://api.github.com/repos/xenia-canary/game-patches/git/trees/"
    "main?recursive=1";
const char* kCommunityPatchesRawUrl =
    "https://raw.githubusercontent.com/xenia-canary/game-patches/main/"
    "patches/";

std::optional<ui::VirtualKey> VirtualKeyFromGdk(guint keyval) {
  if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24) {
    return ui::VirtualKey(uint16_t(ui::VirtualKey::kF1) + (keyval - GDK_KEY_F1));
  }
  if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
    return ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + (keyval - GDK_KEY_a));
  }
  if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
    return ui::VirtualKey(uint16_t(ui::VirtualKey::kA) + (keyval - GDK_KEY_A));
  }
  if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
    return ui::VirtualKey(uint16_t(ui::VirtualKey::k0) + (keyval - GDK_KEY_0));
  }
  switch (keyval) {
    case GDK_KEY_space:
      return ui::VirtualKey::kSpace;
    case GDK_KEY_Delete:
      return ui::VirtualKey::kDelete;
    case GDK_KEY_Insert:
      return ui::VirtualKey::kInsert;
    case GDK_KEY_Home:
      return ui::VirtualKey::kHome;
    case GDK_KEY_End:
      return ui::VirtualKey::kEnd;
    case GDK_KEY_Page_Up:
      return ui::VirtualKey::kPrior;
    case GDK_KEY_Page_Down:
      return ui::VirtualKey::kNext;
    case GDK_KEY_Tab:
      return ui::VirtualKey::kTab;
    default:
      return std::nullopt;
  }
}

}  // namespace

namespace {
// Combo boxes as list popups instead of grabbing menus: under XWayland a
// menu-style combo closes on the click's release (the release is taken as
// an activation), so only a press-and-hold could pick a value.
void ApplyComboListStyle() {
  static bool applied = false;
  if (applied) {
    return;
  }
  applied = true;
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      provider, "combobox { -GtkComboBox-appears-as-list: 1; }", -1, nullptr);
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}
}  // namespace

void EmulatorWindow::RefreshSettingsWindow() {
  for (auto& [key, widget] : settings_refresh_labels_) {
    std::string text;
    if (key == "save_state_dir") {
      text = SaveStateDir().string() +
             (cvars::save_state_dir.empty() ? "  (default)" : "");
    } else if (key == "content_root") {
      text = emulator_->content_root().string() +
             (cvars::content_root.empty() ? "  (default)" : "");
      if (!pending_content_root_.empty()) {
        text += "\n" + pending_content_root_.string() +
                " takes over when the running title closes";
      }
    } else if (key == "games_dir") {
      text = cvars::games_dir.empty() ? "(none)" : cvars::games_dir;
    } else if (key.rfind("hotkey:", 0) == 0) {
      int a = std::stoi(key.substr(7));
      if (settings_capture_action_ == a) {
        text = "Press the new key now (Escape cancels)...";
      } else {
        text = action_keys_[a].has_value() ? HotkeyName(*action_keys_[a])
                                           : "Disabled";
      }
    } else if (key == "scale_in_use") {
      auto* cp = emulator_->graphics_system()
                     ? emulator_->graphics_system()->command_processor()
                     : nullptr;
      text = cp ? fmt::format("in use: {}x{}", cp->zpd_draw_resolution_scale_x(),
                              cp->zpd_draw_resolution_scale_y())
                : "";
    }
    gtk_label_set_text(GTK_LABEL(widget), text.c_str());
  }
}

void EmulatorWindow::ToggleSettingsWindow() {
  if (settings_window_) {
    gtk_window_present(GTK_WINDOW(settings_window_));
    return;
  }
  ApplyComboListStyle();
  GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(win), "Xenia preferences");
  gtk_window_set_default_size(GTK_WINDOW(win), 700, 560);
  if (auto* gtk_main = dynamic_cast<ui::GTKWindow*>(window_.get())) {
    gtk_window_set_transient_for(GTK_WINDOW(win),
                                 GTK_WINDOW(gtk_main->window()));
  }
  settings_refresh_labels_.clear();
  settings_capture_action_ = -1;
  auto remember = [this](const std::string& key, GtkWidget* label) {
    settings_refresh_labels_.emplace_back(key, label);
  };

  GtkWidget* notebook = gtk_notebook_new();
  gtk_container_add(GTK_CONTAINER(win), notebook);

  // ---- Graphics ----
  {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget* grid = NewSection(box, "Output", true);
    int row = 0;
    std::vector<std::pair<std::string, std::string>> scales;
    for (int i = 1; i <= 7; ++i) {
      scales.emplace_back(std::to_string(i), std::to_string(i) + "x");
    }
    {
      // Output-size presets over the two scale cvars (a 720p title).
      int cx = std::clamp(cvars::draw_resolution_scale_x, 1, 7);
      int cy = std::clamp(cvars::draw_resolution_scale_y, 1, 7);
      std::string current = cx == cy && cx >= 1 && cx <= 3 ? std::to_string(cx)
                                                            : "custom";
      AddCombo(grid, row, "Output preset (for a 720p game; relaunch)",
               "draw_resolution_scale_x",
               {{"custom", "Custom (set width and height below)"},
                {"1", "720p, native (1x1)"},
                {"2", "1440p (2x2)"},
                {"3", "4K (3x3)"}},
               current, [this](const std::string& v) {
                 if (v == "custom") {
                   return;
                 }
                 int scale = std::stoi(v);
                 SetGpuOption<int32_t>("draw_resolution_scale_x", scale);
                 SetGpuOption<int32_t>("draw_resolution_scale_y", scale);
                 RefreshSettingsWindow();
                 // The width/height combos below show the old values until
                 // the window is reopened.
               });
    }
    AddCombo(grid, row, "Resolution scale, width (relaunch)", "draw_resolution_scale_x",
             scales, std::to_string(std::clamp(cvars::draw_resolution_scale_x, 1, 7)),
             [this](const std::string& v) {
               SetGpuOption<int32_t>("draw_resolution_scale_x", std::stoi(v));
               RefreshSettingsWindow();
             });
    AddCombo(grid, row, "Resolution scale, height (relaunch)", "draw_resolution_scale_y",
             scales, std::to_string(std::clamp(cvars::draw_resolution_scale_y, 1, 7)),
             [this](const std::string& v) {
               SetGpuOption<int32_t>("draw_resolution_scale_y", std::stoi(v));
               RefreshSettingsWindow();
             });
    GtkWidget* in_use = LeftLabel("");
    remember("scale_in_use", in_use);
    gtk_grid_attach(GTK_GRID(grid), in_use, 1, row++, 1, 1);
    AddSpin(grid, row, "Frame rate limit, fps (0 = 60 with VSync, else unlimited; relaunch)",
            "framerate_limit", double(cvars::framerate_limit), 0, 1000,
            [](double v) { SetGpuOption<uint64_t>("framerate_limit", uint64_t(v)); });
    {
      GtkWidget* fps = gtk_check_button_new_with_label("Show FPS overlay");
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fps), cvars::show_fps);
      SetTooltipFromCvar(fps, "show_fps");
      AttachSettingsCallback(fps, "toggled", [this](GtkWidget* w) {
        bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
        if (on != cvars::show_fps) {
          ToggleFpsOverlay();
        }
      });
      gtk_grid_attach(GTK_GRID(grid), fps, 0, row++, 2, 1);
    }
    AddCheck(grid, row, "VSync", "vsync", cvars::vsync);

    grid = NewSection(box, "Filters (take effect now)", true);
    row = 0;
    {
      using PaintConfig = ui::Presenter::GuestOutputPaintConfig;
      AddCombo(grid, row, "Anti-aliasing", "postprocess_antialiasing",
               {{"", "None"},
                {"fxaa", "FXAA (NVIDIA fast approximate, normal quality)"},
                {"fxaa_extreme", "FXAA, extreme quality"}},
               GetCvarValueForSwapPostEffect(GetSwapPostEffectForCvarValue(
                   cvars::postprocess_antialiasing)),
               [this](const std::string& v) {
                 SetGpuOption<std::string>("postprocess_antialiasing", v);
                 ApplyDisplayConfigForCvars();
               });
      AddCombo(grid, row, "Scaling and sharpening",
               "postprocess_scaling_and_sharpening",
               {{"", "Bilinear (plain stretch)"},
                {"cas", "AMD CAS sharpening (up to 2x2 scaling)"},
                {"fsr", "AMD FSR 1.0 upscaling, CAS when not upscaling"}},
               GetCvarValueForGuestOutputPaintEffect(
                   GetGuestOutputPaintEffectForCvarValue(
                       cvars::postprocess_scaling_and_sharpening)),
               [this](const std::string& v) {
                 SetGpuOption<std::string>("postprocess_scaling_and_sharpening",
                                           v);
                 ApplyDisplayConfigForCvars();
               });
      AddScale(grid, row, "CAS additional sharpness",
               "postprocess_ffx_cas_additional_sharpness",
               cvars::postprocess_ffx_cas_additional_sharpness,
               PaintConfig::kCasAdditionalSharpnessMin,
               PaintConfig::kCasAdditionalSharpnessMax, 0.01,
               [this](double v) {
                 SetGpuOptionDeferred<double>(
                     "postprocess_ffx_cas_additional_sharpness", v);
                 ApplyDisplayConfigForCvars();
               });
      AddScale(grid, row, "FSR sharpness reduction (lower is sharper)",
               "postprocess_ffx_fsr_sharpness_reduction",
               cvars::postprocess_ffx_fsr_sharpness_reduction,
               PaintConfig::kFsrSharpnessReductionMin,
               PaintConfig::kFsrSharpnessReductionMax, 0.01,
               [this](double v) {
                 SetGpuOptionDeferred<double>(
                     "postprocess_ffx_fsr_sharpness_reduction", v);
                 ApplyDisplayConfigForCvars();
               });
      {
        GtkWidget* dither =
            gtk_check_button_new_with_label("Dither the output to 8 bits");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dither),
                                     cvars::postprocess_dither);
        SetTooltipFromCvar(dither, "postprocess_dither");
        AttachSettingsCallback(dither, "toggled", [this](GtkWidget* w) {
          SetGpuOption<bool>(
              "postprocess_dither",
              gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)));
          ApplyDisplayConfigForCvars();
        });
        gtk_grid_attach(GTK_GRID(grid), dither, 0, row++, 2, 1);
      }
      struct ColorScale {
        const char* label;
        const char* name;
        double value, min, max;
      };
      const ColorScale color_scales[] = {
          {"Brightness", "postprocess_brightness",
           cvars::postprocess_brightness, PaintConfig::kBrightnessMin,
           PaintConfig::kBrightnessMax},
          {"Contrast", "postprocess_contrast", cvars::postprocess_contrast,
           PaintConfig::kContrastMin, PaintConfig::kContrastMax},
          {"Saturation", "postprocess_saturation",
           cvars::postprocess_saturation, PaintConfig::kSaturationMin,
           PaintConfig::kSaturationMax},
          {"Gamma", "postprocess_gamma", cvars::postprocess_gamma,
           PaintConfig::kGammaMin, PaintConfig::kGammaMax},
      };
      for (const ColorScale& c : color_scales) {
        std::string name = c.name;
        AddScale(grid, row, c.label, c.name, c.value, c.min, c.max, 0.01,
                 [this, name](double v) {
                   SetGpuOptionDeferred<double>(name.c_str(), v);
                   ApplyDisplayConfigForCvars();
                 });
      }
      {
        GtkWidget* reset = gtk_button_new_with_label("Reset colour filters");
        gtk_widget_set_halign(reset, GTK_ALIGN_START);
        AttachSettingsCallback(reset, "clicked", [this](GtkWidget*) {
          SetGpuOptionDeferred<double>("postprocess_brightness",
                                       PaintConfig::kBrightnessDefault);
          SetGpuOptionDeferred<double>("postprocess_contrast",
                                       PaintConfig::kContrastDefault);
          SetGpuOptionDeferred<double>("postprocess_saturation",
                                       PaintConfig::kSaturationDefault);
          SetGpuOptionDeferred<double>("postprocess_gamma",
                                       PaintConfig::kGammaDefault);
          ApplyDisplayConfigForCvars();
          RefreshSettingsWindow();
        });
        gtk_grid_attach(GTK_GRID(grid), reset, 1, row++, 1, 1);
      }
    }

    grid = NewSection(box, "Accuracy and compatibility", false);
    row = 0;
    AddCombo(grid, row, "Occlusion queries", "occlusion_query",
             {{"fast", "fast: ask the GPU, use the cached answer (default)"},
              {"fast-alt", "fast-alt: fast, keeps zero results"},
              {"fake", "fake: never ask the GPU"},
              {"strict", "strict: wait for the GPU"}},
             cvars::occlusion_query, [](const std::string& v) {
               SetGpuOption<std::string>("occlusion_query", v);
             });
    AddSpin(grid, row, "Occlusion query saturation (%)",
            "occlusion_query_saturation",
            cvars::occlusion_query_saturation * 100.0, 0, 100, [](double v) {
              SetGpuOption<double>("occlusion_query_saturation", v / 100.0);
            });
    AddCombo(grid, row, "Readback resolve", "readback_resolve",
             {{"none", "none: no CPU readback"},
              {"fast", "fast: previous frame, no stall"},
              {"full", "full: wait for the GPU"}},
             cvars::readback_resolve, [](const std::string& v) {
               SetGpuOption<std::string>("readback_resolve", v);
             });
    AddCombo(grid, row, "Anisotropic filtering", "anisotropic_override",
             {{"-1", "No override"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"},
              {"3", "4x"}, {"4", "8x"}, {"5", "16x"}},
             std::to_string(cvars::anisotropic_override),
             [](const std::string& v) {
               SetGpuOption<int32_t>("anisotropic_override", std::stoi(v));
             });
    AddCheck(grid, row, "Clear memory page state (Team Ninja games)",
             "clear_memory_page_state", cvars::clear_memory_page_state);
    AddCheck(grid, row, "Allow invalid fetch constants",
             "gpu_allow_invalid_fetch_constants",
             cvars::gpu_allow_invalid_fetch_constants);
    AddCheck(grid, row, "Allow reads from no-access pages",
             "gpu_allow_invalid_upload_range",
             cvars::gpu_allow_invalid_upload_range);
    AddCheck(grid, row, "Half-pixel offset", "half_pixel_offset",
             cvars::half_pixel_offset);
    AddCheck(grid, row, "Force depth clamp (new pipelines)",
             "force_depth_clamp", cvars::force_depth_clamp);

    grid = NewSection(box, "Performance and advanced", false);
    row = 0;
    AddCombo(grid, row, "Render target path (Vulkan; relaunch)",
             "render_target_path_vulkan",
             {{"", "any: pick what suits the GPU"},
              {"fbo", "fbo: host framebuffers, faster, fewer formats"},
              {"fsi", "fsi: fragment shader interlock, most accurate"}},
             cvars::render_target_path_vulkan, [](const std::string& v) {
               SetGpuOption<std::string>("render_target_path_vulkan", v);
             });
    AddCheck(grid, row,
             "Skip copying unchanged screen regions (experimental; relaunch)",
             "dirty_region_tracking", cvars::dirty_region_tracking);
    AddCheck(grid, row, "Sparse shared memory (Vulkan; relaunch)",
             "vulkan_sparse_shared_memory", cvars::vulkan_sparse_shared_memory);
    AddSpin(grid, row, "Pipeline creation threads (-1 = automatic; relaunch)",
            "vulkan_pipeline_creation_threads",
            cvars::vulkan_pipeline_creation_threads, -1, 64, [](double v) {
              SetGpuOption<int32_t>("vulkan_pipeline_creation_threads",
                                    int32_t(v));
            });
    AddCheck(grid, row, "Asynchronous shader compilation (new pipelines)",
             "async_shader_compilation", cvars::async_shader_compilation);
    AddSpin(grid, row, "Texture cache soft limit, MB",
            "texture_cache_memory_limit_soft",
            cvars::texture_cache_memory_limit_soft, 16, 65536, [](double v) {
              SetGpuOption<uint32_t>("texture_cache_memory_limit_soft",
                                     uint32_t(v));
            });
    AddSpin(grid, row, "Texture cache hard limit, MB",
            "texture_cache_memory_limit_hard",
            cvars::texture_cache_memory_limit_hard, 16, 65536, [](double v) {
              SetGpuOption<uint32_t>("texture_cache_memory_limit_hard",
                                     uint32_t(v));
            });
    AddSpin(grid, row, "Texture unused for, s (soft limit)",
            "texture_cache_memory_limit_soft_lifetime",
            cvars::texture_cache_memory_limit_soft_lifetime, 0, 3600,
            [](double v) {
              SetGpuOption<uint32_t>("texture_cache_memory_limit_soft_lifetime",
                                     uint32_t(v));
            });

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(box),
                             gtk_label_new("GPU"));
  }
  // ---- CPU ----
  {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget* grid = NewSection(box, "Code generation (relaunch)", true);
    int row = 0;
    GtkWidget* note = LeftLabel(
        "These change the code the emulator generates for the game, so they "
        "apply to a title launched afterwards.");
    gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 2, 1);
    AddCheck(grid, row,
             "Reuse vector values instead of reloading them (experimental)",
             "promote_vector_context_values",
             cvars::promote_vector_context_values);
    GtkWidget* vecnote = LeftLabel(
        "Worth about +23% in scenes limited by the CPU rather than the "
        "graphics card. Off upstream since January for a reason that was "
        "never written down, so treat it as under test: watch for odd "
        "animation or physics rather than a wrong-looking frame.");
    gtk_grid_attach(GTK_GRID(grid), vecnote, 0, row++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(box),
                             gtk_label_new("CPU"));
  }

  // ---- Audio ----
  {
    GtkWidget* grid = NewGrid();
    int row = 0;
    std::string mute_label = "Mute";
    if (auto mute_key = action_key(HotkeyAction::kMute)) {
      mute_label += " (" + HotkeyName(*mute_key) + ")";
    }
    GtkWidget* mute = gtk_check_button_new_with_label(mute_label.c_str());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mute), cvars::mute);
    SetTooltipFromCvar(mute, "mute");
    AttachSettingsCallback(mute, "toggled", [this](GtkWidget* w) {
      bool active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
      if (active != cvars::mute) {
        ToggleMute();  // keeps the MUTED indicator in step
      }
    });
    gtk_grid_attach(GTK_GRID(grid), mute, 0, row++, 2, 1);
    AddCombo(grid, row, "Fast-forward audio", "fast_forward_audio",
             {{"stretch", "stretch: same pitch (SoundTouch)"},
              {"resample", "resample: higher pitch"},
              {"mute", "mute"}},
             cvars::fast_forward_audio, [](const std::string& v) {
               SetGpuOption<std::string>("fast_forward_audio", v);
             });
    AddCombo(grid, row, "Slow-motion audio", "slow_motion_audio",
             {{"resample", "resample: lower pitch"},
              {"stretch", "stretch: same pitch (SoundTouch)"},
              {"mute", "mute"}},
             cvars::slow_motion_audio, [](const std::string& v) {
               SetGpuOption<std::string>("slow_motion_audio", v);
             });
    gtk_grid_attach(GTK_GRID(grid), HeadingLabel("Media player (XMP)"), 0,
                    row++, 2, 1);
    GtkWidget* xmp = gtk_button_new_with_label("Open the media player panel...");
    AttachSettingsCallback(xmp, "clicked",
                           [this](GtkWidget*) { ToggleXMPConfigDialog(); });
    gtk_widget_set_halign(xmp, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), xmp, 0, row++, 2, 1);
    GtkWidget* note = LeftLabel(
        "The media player panel is still an in-window (ImGui) panel.");
    gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(grid),
                             gtk_label_new("Audio"));

  }

  // ---- Input: controllers and keyboard hotkeys ----
  {
    GtkWidget* grid = NewGrid();
    int row = 0;
    GtkWidget* vibration =
        gtk_check_button_new_with_label("Controller vibration");
    auto* input_system = emulator_->input_system();
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(vibration),
        input_system ? input_system->GetVibrationCvar() : true);
    SetTooltipFromCvar(vibration, "vibration");
    AttachSettingsCallback(vibration, "toggled", [this](GtkWidget* w) {
      bool active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
      auto* input_system = emulator_->input_system();
      if (input_system && active != input_system->GetVibrationCvar()) {
        ToggleControllerVibration();
      }
    });
    gtk_grid_attach(GTK_GRID(grid), vibration, 0, row++, 4, 1);
    AddCheck(grid, row, "Forward the guide button to the game", "guide_button",
             cvars::guide_button);
    GtkWidget* overlay = gtk_button_new_with_label(
        "Show the controller hotkeys overlay");
    AttachSettingsCallback(overlay, "clicked",
                           [this](GtkWidget*) { DisplayHotKeysConfig(); });
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), overlay, 0, row++, 4, 1);
    gtk_grid_attach(GTK_GRID(grid), HeadingLabel("Keyboard hotkeys"), 0, row++,
                    4, 1);
    static std::vector<std::string> choices = AssignableHotkeyChoices();
    std::vector<std::pair<std::string, std::string>> key_choices;
    for (const auto& c : choices) {
      key_choices.emplace_back(c, c);
    }
    for (int a = 0; a < int(HotkeyAction::kCount); ++a) {
      HotkeyAction action = HotkeyAction(a);
      gtk_grid_attach(GTK_GRID(grid), LeftLabel(HotkeyActionLabel(action)), 0,
                      row, 1, 1);
      GtkWidget* current = LeftLabel("");
      remember("hotkey:" + std::to_string(a), current);
      gtk_grid_attach(GTK_GRID(grid), current, 1, row, 1, 1);
      GtkWidget* press = gtk_button_new_with_label("Press a key...");
      AttachSettingsCallback(press, "clicked", [this, a](GtkWidget*) {
        settings_capture_action_ = a;
        RefreshSettingsWindow();
      });
      gtk_grid_attach(GTK_GRID(grid), press, 2, row, 1, 1);
      GtkWidget* combo = gtk_combo_box_text_new();
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Choose...");
      for (const auto& c : choices) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), c.c_str());
      }
      gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
      AttachSettingsCallback(combo, "changed", [this, action](GtkWidget* w) {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
        if (index >= 1 && index <= int(choices.size())) {
          settings_capture_action_ = -1;
          SetActionHotkey(action, *ParseHotkeyName(choices[index - 1]));
          gtk_combo_box_set_active(GTK_COMBO_BOX(w), 0);
          RefreshSettingsWindow();
        }
      });
      gtk_grid_attach(GTK_GRID(grid), combo, 3, row++, 1, 1);
    }
    GtkWidget* note = LeftLabel(
        "Xenia's fixed keys cannot be assigned: F1 FAQ, F2 build commit, F3 "
        "profiler, F4 GPU trace, F5 clear caches, F6 post-processing, F9 run "
        "the previous title, F11 fullscreen, F12 screenshot, Escape, "
        "Pause/Break, Ctrl+O, Numpad + - *.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_grid_attach(GTK_GRID(grid), note, 0, row++, 4, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(grid),
                             gtk_label_new("Input"));
  }

  // ---- Folders ----
  {
    GtkWidget* grid = NewGrid();
    int row = 0;
    struct Folder {
      const char* heading;
      const char* key;
      const char* clear_label;
      std::function<void()> pick;
      std::function<void()> clear;
    };
    Folder folders[] = {
        {"Save states", "save_state_dir", "Use default",
         [this]() { PickSaveStateDir(); }, [this]() { SetSaveStateDir(""); }},
        {"Content (DLC, saves, installed titles)", "content_root",
         "Use default", [this]() { PickContentRoot(); },
         [this]() { SetContentRoot(""); }},
        {"Games", "games_dir", "Use default", [this]() { PickGamesDir(); },
         [this]() { SetGamesDir(""); }},
    };
    for (auto& f : folders) {
      GtkWidget* heading = HeadingLabel(f.heading);
      SetTooltipFromCvar(heading, f.key);
      gtk_grid_attach(GTK_GRID(grid), heading, 0, row++, 3, 1);
      GtkWidget* path = LeftLabel("");
      gtk_label_set_selectable(GTK_LABEL(path), TRUE);
      gtk_label_set_line_wrap(GTK_LABEL(path), TRUE);
      gtk_widget_set_hexpand(path, TRUE);
      remember(f.key, path);
      gtk_grid_attach(GTK_GRID(grid), path, 0, row++, 3, 1);
      GtkWidget* change = gtk_button_new_with_label("Change...");
      auto pick = f.pick;
      AttachSettingsCallback(change, "clicked", [this, pick](GtkWidget*) {
        pick();
        RefreshSettingsWindow();
      });
      gtk_grid_attach(GTK_GRID(grid), change, 0, row, 1, 1);
      GtkWidget* clear = gtk_button_new_with_label(f.clear_label);
      auto clear_fn = f.clear;
      AttachSettingsCallback(clear, "clicked", [this, clear_fn](GtkWidget*) {
        clear_fn();
        RefreshSettingsWindow();
      });
      gtk_grid_attach(GTK_GRID(grid), clear, 1, row++, 1, 1);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(grid),
                             gtk_label_new("Folders"));
  }

  // ---- Profiles and Console ----
  BuildPatchesTab(notebook);
  BuildProfilesTab(notebook);
  BuildConsoleTab(notebook);

  g_signal_connect(
      win, "key-press-event",
      G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer data) -> gboolean {
        auto* self = static_cast<EmulatorWindow*>(data);
        if (self->settings_capture_action_ < 0) {
          return FALSE;
        }
        if (event->keyval == GDK_KEY_Escape) {
          self->settings_capture_action_ = -1;
          self->RefreshSettingsWindow();
          return TRUE;
        }
        auto key = VirtualKeyFromGdk(event->keyval);
        if (!key) {
          return TRUE;  // modifiers and unassignable keys: keep waiting
        }
        int a = self->settings_capture_action_;
        self->settings_capture_action_ = -1;
        self->SetActionHotkey(HotkeyAction(a), *key);
        self->RefreshSettingsWindow();
        return TRUE;
      }),
      this);
  g_signal_connect(win, "destroy",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     auto* self = static_cast<EmulatorWindow*>(data);
                     self->settings_window_ = nullptr;
                     self->patches_status_ = nullptr;
                     self->patches_combo_ = nullptr;
                     self->patches_box_ = nullptr;
                     self->community_status_ = nullptr;
                     self->community_box_ = nullptr;
                     self->community_show_all_ = nullptr;
                     self->community_filter_ = nullptr;
                     self->settings_refresh_labels_.clear();
                     self->settings_capture_action_ = -1;
                     self->profiles_list_ = nullptr;
                     self->profiles_status_ = nullptr;
                     self->console_refreshers_.clear();
                     self->console_status_ = nullptr;
                   }),
                   this);
  settings_window_ = win;
  RefreshSettingsWindow();
  gtk_widget_show_all(win);
  XELOGI("Settings window opened");
}


// ---- Game library dashboard ----

namespace {
constexpr std::string_view kLibraryFilename = "library.toml";

enum DashboardColumn {
  kColType = 0,
  kColTitleId,
  kColTitle,
  kColTimePlayed,
  kColLastPlayed,
  kColSize,
  kColRegion,
  kColDiscs,
  kColRating,
  kColIndex,        // int: index into library_titles_
  kColSeconds,      // int64 sort key
  kColLastPlayedTs, // int64 sort key
  kColSizeBytes,    // int64 sort key
  kColRatingValue,  // int sort key
  kColIcon,         // GdkPixbuf*, 32 px
  kColCount
};

std::string RegionText(uint32_t region) {
  if (region == 0) {
    return "";
  }
  if (region == 0xFFFFFFFFu) {
    return "All";
  }
  std::vector<std::string> parts;
  if (region & 0x000000FF) parts.push_back("NTSC-U");
  if (region & 0x0000FF00) parts.push_back("NTSC-J");
  if (region & 0x00FF0000) parts.push_back("PAL");
  if (region & 0xFF000000) parts.push_back("Other");
  std::string out;
  for (auto& part : parts) {
    out += (out.empty() ? "" : ", ") + part;
  }
  return out;
}

std::string TimePlayedText(int64_t seconds) {
  if (seconds <= 0) {
    return "";
  }
  if (seconds < 3600) {
    int64_t minutes = std::max<int64_t>(1, seconds / 60);
    return fmt::format("{} minute{}", minutes, minutes == 1 ? "" : "s");
  }
  int64_t hours = seconds / 3600;
  return fmt::format("{} hour{}", hours, hours == 1 ? "" : "s");
}

std::string DateText(int64_t ts) {
  if (ts <= 0) {
    return "";
  }
  std::time_t t = std::time_t(ts);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%m/%d/%Y", std::localtime(&t));
  return buf;
}

std::string RatingText(int rating) {
  std::string out;
  for (int i = 1; i <= 5; ++i) {
    out += i <= rating ? "\xE2\x98\x85" : "\xE2\x98\x86";  // filled/empty star
  }
  return rating ? out : "";
}

std::string LowerAscii(std::string s) {
  for (auto& c : s) {
    c = char(std::tolower((unsigned char)c));
  }
  return s;
}
}  // namespace

EmulatorWindow::LibraryTitle* EmulatorWindow::LibraryEntryFor(
    const std::filesystem::path& path) {
  std::error_code ec;
  for (auto& title : library_titles_) {
    if (title.path == path || std::filesystem::equivalent(title.path, path, ec)) {
      return &title;
    }
  }
  return nullptr;
}

void EmulatorWindow::LoadLibrary() {
  library_titles_.clear();
  std::ifstream file(emulator()->storage_root() / kLibraryFilename);
  if (!file.is_open()) {
    return;
  }
  toml::parse_result parsed;
  try {
    parsed = toml::parse(file);
  } catch (toml::parse_error& e) {
    XELOGE("Cannot parse library.toml: {}", e.what());
    return;
  }
  auto* titles = parsed["titles"].as_array();
  if (!titles) {
    return;
  }
  for (auto& node : *titles) {
    auto* t = node.as_table();
    if (!t) {
      continue;
    }
    LibraryTitle title;
    title.path = t->get_as<std::string>("path") ? t->get_as<std::string>("path")->get() : "";
    if (title.path.empty()) {
      continue;
    }
    auto str = [&](const char* key) {
      auto* v = t->get_as<std::string>(key);
      return v ? v->get() : std::string();
    };
    auto num = [&](const char* key) -> int64_t {
      auto* v = t->get_as<int64_t>(key);
      return v ? v->get() : 0;
    };
    title.type = str("type");
    title.title_id = uint32_t(std::strtoul(str("title_id").c_str(), nullptr, 16));
    title.title_name = str("title_name");
    title.disc_number = uint8_t(num("disc_number"));
    title.disc_count = uint8_t(num("disc_count"));
    title.media_id = uint32_t(std::strtoul(str("media_id").c_str(), nullptr, 16));
    title.region = uint32_t(std::strtoul(str("region").c_str(), nullptr, 16));
    title.size = uint64_t(num("size"));
    title.seconds_played = num("seconds_played");
    title.last_played = num("last_played");
    title.rating = int(std::clamp<int64_t>(num("rating"), 0, 5));
    library_titles_.push_back(std::move(title));
  }
  XELOGI("Library: {} title(s) loaded", library_titles_.size());
}

void EmulatorWindow::SaveLibrary() {
  toml::array titles;
  for (const auto& title : library_titles_) {
    toml::table t;
    t.insert("path", xe::path_to_utf8(title.path));
    t.insert("type", title.type);
    t.insert("title_id", fmt::format("{:08X}", title.title_id));
    t.insert("title_name", title.title_name);
    t.insert("disc_number", int64_t(title.disc_number));
    t.insert("disc_count", int64_t(title.disc_count));
    t.insert("media_id", fmt::format("{:08X}", title.media_id));
    t.insert("region", fmt::format("{:08X}", title.region));
    t.insert("size", int64_t(title.size));
    t.insert("seconds_played", title.seconds_played);
    t.insert("last_played", title.last_played);
    t.insert("rating", int64_t(title.rating));
    titles.push_back(std::move(t));
  }
  toml::table root;
  root.insert("titles", std::move(titles));
  std::ofstream file(emulator()->storage_root() / kLibraryFilename,
                     std::ofstream::trunc);
  file << root;
}

// Title id, discs, media id and region from the XEX2 header of the file
// (ISO/ZAR: default.xex at the disc root), without launching. The name
// lives in the compressed part of the XEX and is filled in at first launch.
bool EmulatorWindow::ReadTitleInfo(LibraryTitle& title) {
  std::vector<uint8_t> header;
  std::unique_ptr<vfs::Device> device;
  if (title.type == "XEX") {
    auto* f = xe::filesystem::OpenFile(title.path, "rb");
    if (!f) {
      return false;
    }
    header.resize(64 * 1024);
    size_t n = fread(header.data(), 1, header.size(), f);
    fclose(f);
    header.resize(n);
  } else {
    if (title.type == "ISO") {
      device = std::make_unique<vfs::DiscImageDevice>("\\Device\\LibraryScan",
                                                      title.path);
    } else {
      device = std::make_unique<vfs::DiscZarchiveDevice>("\\Device\\LibraryScan",
                                                         title.path);
    }
    if (!device->Initialize()) {
      return false;
    }
    auto* entry = device->ResolvePath("default.xex");
    if (!entry) {
      return false;
    }
    vfs::File* file = nullptr;
    if (entry->Open(vfs::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
        !file) {
      return false;
    }
    header.resize(std::min<size_t>(entry->size(), 64 * 1024));
    size_t n = 0;
    file->ReadSync(std::span<uint8_t>(header.data(), header.size()), 0, &n);
    file->Destroy();
    header.resize(n);
  }
  if (header.size() < sizeof(xex2_header) ||
      xe::load_and_swap<uint32_t>(header.data()) != 0x58455832) {  // 'XEX2'
    return false;
  }
  auto* xex = reinterpret_cast<const xex2_header*>(header.data());
  uint32_t count = xex->header_count;
  for (uint32_t i = 0; i < count; ++i) {
    size_t at = offsetof(xex2_header, headers) + i * sizeof(xex2_opt_header);
    if (at + sizeof(xex2_opt_header) > header.size()) {
      break;
    }
    auto* opt = reinterpret_cast<const xex2_opt_header*>(header.data() + at);
    if (opt->key == XEX_HEADER_EXECUTION_INFO) {
      uint32_t offset = opt->offset;
      if (offset + sizeof(xex2_opt_execution_info) <= header.size()) {
        auto* info = reinterpret_cast<const xex2_opt_execution_info*>(
            header.data() + offset);
        title.title_id = info->title_id;
        title.media_id = info->media_id;
        title.disc_number = info->disc_number;
        title.disc_count = info->disc_count;
      }
    }
  }
  uint32_t security = xex->security_offset;
  if (security + 0x180 <= header.size()) {
    title.region = xe::load_and_swap<uint32_t>(header.data() + security + 0x178);
  }
  return title.title_id != 0;
}

void EmulatorWindow::ScanLibrary() {
  std::filesystem::path root = cvars::games_dir;
  std::error_code ec;
  if (root.empty() || !std::filesystem::is_directory(root, ec)) {
    return;
  }
  size_t added = 0, unreadable = 0;
  auto it = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  for (; !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it.depth() >= 3) {
      it.disable_recursion_pending();
    }
    const auto& entry = *it;
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    std::string ext = xe::utf8::lower_ascii(entry.path().extension().string());
    std::string type = ext == ".iso" ? "ISO" : ext == ".xex" ? "XEX" : ext == ".zar" ? "ZAR" : "";
    if (type.empty()) {
      continue;
    }
    LibraryTitle* existing = LibraryEntryFor(entry.path());
    if (existing) {
      existing->size = entry.file_size(ec);
      if (!existing->title_id) {
        ReadTitleInfo(*existing);
      }
      continue;
    }
    LibraryTitle title;
    title.path = entry.path();
    title.type = type;
    title.size = entry.file_size(ec);
    if (!ReadTitleInfo(title)) {
      ++unreadable;
    }
    for (const auto& recent : recently_launched_titles_) {
      if (recent.path_to_file == entry.path()) {
        title.title_name = recent.title_name;
        title.last_played = recent.last_run_time;
      }
    }
    library_titles_.push_back(std::move(title));
    ++added;
  }
  // Drop entries whose file is gone.
  std::erase_if(library_titles_, [&](const LibraryTitle& t) {
    return !std::filesystem::exists(t.path, ec);
  });
  XELOGI("Library: {} scanned, {} new, {} without a readable XEX header, {} total",
         root.string(), added, unreadable, library_titles_.size());
  if (added) {
    SaveLibrary();
  }
}

namespace {
// Alternating row backgrounds (near black / grey), set per cell so it does
// not depend on the theme honouring the tree view's rules hint.
void DashboardRowBackground(GtkTreeViewColumn*, GtkCellRenderer* renderer,
                            GtkTreeModel* model, GtkTreeIter* iter, gpointer) {
  GtkTreePath* path = gtk_tree_model_get_path(model, iter);
  bool odd = path && (gtk_tree_path_get_indices(path)[0] & 1);
  if (path) {
    gtk_tree_path_free(path);
  }
  g_object_set(renderer, "cell-background", odd ? "#2b2b2b" : "#101010",
               "cell-background-set", TRUE, nullptr);
  if (GTK_IS_CELL_RENDERER_TEXT(renderer)) {  // the icon column has none
    g_object_set(renderer, "foreground", "#e6e6e6", "foreground-set", TRUE,
                 nullptr);
  }
}

gboolean DashboardVisibleFunc(GtkTreeModel* model, GtkTreeIter* iter,
                              gpointer data) {
  auto* w = static_cast<EmulatorWindow*>(data);
  return w->DashboardRowVisible(model, iter) ? TRUE : FALSE;
}
}  // namespace

bool EmulatorWindow::DashboardRowVisible(void* model_ptr, void* iter_ptr) {
  auto* model = static_cast<GtkTreeModel*>(model_ptr);
  auto* iter = static_cast<GtkTreeIter*>(iter_ptr);
  gchar *type = nullptr, *title = nullptr, *title_id = nullptr, *region = nullptr;
  gtk_tree_model_get(model, iter, kColType, &type, kColTitle, &title, kColTitleId,
                     &title_id, kColRegion, &region, -1);
  bool visible = true;
  if (dashboard_search_) {
    std::string needle =
        LowerAscii(gtk_entry_get_text(GTK_ENTRY(dashboard_search_)));
    if (!needle.empty()) {
      std::string hay = LowerAscii(std::string(title ? title : "") + " " +
                                   (title_id ? title_id : ""));
      visible = hay.find(needle) != std::string::npos;
    }
  }
  if (visible && dashboard_type_) {
    int t = gtk_combo_box_get_active(GTK_COMBO_BOX(dashboard_type_));
    static const char* kTypes[] = {"", "ISO", "XEX", "ZAR"};
    if (t > 0 && t < 4 && type && std::strcmp(type, kTypes[t]) != 0) {
      visible = false;
    }
  }
  if (visible && dashboard_region_) {
    int r = gtk_combo_box_get_active(GTK_COMBO_BOX(dashboard_region_));
    static const char* kRegions[] = {"", "NTSC-U", "NTSC-J", "PAL"};
    if (r > 0 && r < 4 &&
        (!region || !std::strstr(region, kRegions[r])) &&
        !(region && !std::strcmp(region, "All"))) {
      visible = false;
    }
  }
  g_free(type);
  g_free(title);
  g_free(title_id);
  g_free(region);
  return visible;
}

void EmulatorWindow::BuildDashboard() {
  auto* gtk_main = dynamic_cast<ui::GTKWindow*>(window_.get());
  if (!gtk_main || dashboard_widget_) {
    return;
  }
  ApplyComboListStyle();
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Toolbar.
  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
  GtkWidget* type = gtk_combo_box_text_new();
  for (const char* t : {"All types", "ISO", "XEX", "ZAR"}) {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(type), t);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(type), 0);
  GtkWidget* region = gtk_combo_box_text_new();
  for (const char* r : {"All regions", "NTSC-U", "NTSC-J", "PAL"}) {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(region), r);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(region), 0);
  GtkWidget* search = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search...");
  gtk_widget_set_hexpand(search, TRUE);
  GtkWidget* rescan = gtk_button_new_with_label("Rescan");
  GtkWidget* folder = gtk_button_new_with_label("Games folder...");
  GtkWidget* back = gtk_button_new_with_label("Back to game");
  gtk_box_pack_start(GTK_BOX(bar), type, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), region, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), search, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bar), rescan, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), folder, FALSE, FALSE, 0);
  // While a title runs behind the list: a banner saying so, with the
  // Back to game button in it (a small toolbar button was easy to miss).
  GtkWidget* banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(banner), 8);
  GtkWidget* banner_label = gtk_label_new(nullptr);
  gtk_label_set_use_markup(GTK_LABEL(banner_label), TRUE);
  gtk_widget_set_halign(banner_label, GTK_ALIGN_START);
  gtk_widget_set_hexpand(banner_label, TRUE);
  gtk_box_pack_start(GTK_BOX(banner), banner_label, TRUE, TRUE, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(back),
                              "suggested-action");
  gtk_widget_set_size_request(back, 180, 40);
  gtk_box_pack_start(GTK_BOX(banner), back, FALSE, FALSE, 0);
  gtk_widget_set_no_show_all(banner, TRUE);
  gtk_box_pack_start(GTK_BOX(box), banner, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

  // List.
  GtkListStore* store = gtk_list_store_new(
      kColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_INT, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_INT,
      GDK_TYPE_PIXBUF);
  GtkTreeModel* filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), nullptr);
  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter),
                                         DashboardVisibleFunc, this, nullptr);
  GtkTreeModel* sortable = gtk_tree_model_sort_new_with_model(filter);
  GtkWidget* view = gtk_tree_view_new_with_model(sortable);
  gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(view), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
  struct Col {
    const char* title;
    int column;
    int sort_column;
    bool expand;
    float xalign;
  };
  const Col cols[] = {
      {"Type", kColType, kColType, false, 0.0f},
      {"Title ID", kColTitleId, kColTitleId, false, 0.0f},
      {"Title", kColTitle, kColTitle, true, 0.0f},
      {"Time Played", kColTimePlayed, kColSeconds, false, 1.0f},
      {"Last Played", kColLastPlayed, kColLastPlayedTs, false, 1.0f},
      {"Size", kColSize, kColSizeBytes, false, 1.0f},
      {"Region", kColRegion, kColRegion, false, 0.0f},
      {"Rating", kColRating, kColRatingValue, false, 0.0f},
  };
  for (const auto& c : cols) {
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "xalign", c.xalign, "xpad", 8, "ypad", 6, nullptr);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
        c.title, renderer, "text", c.column, nullptr);
    gtk_tree_view_column_set_cell_data_func(column, renderer,
                                            DashboardRowBackground, nullptr,
                                            nullptr);
    gtk_tree_view_column_set_sort_column_id(column, c.sort_column);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, c.expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  }
  {
    // Icon column first.
    GtkCellRenderer* pix = gtk_cell_renderer_pixbuf_new();
    g_object_set(pix, "xpad", 6, "ypad", 2, nullptr);
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
        "", pix, "pixbuf", kColIcon, nullptr);
    gtk_tree_view_column_set_cell_data_func(column, pix,
                                            DashboardRowBackground, nullptr,
                                            nullptr);
    gtk_tree_view_insert_column(GTK_TREE_VIEW(view), column, 0);
  }
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sortable),
                                       kColLastPlayedTs, GTK_SORT_DESCENDING);
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_container_add(GTK_CONTAINER(scroller), view);

  // Grid: the same rows as covers, from the filtered model.
  GtkListStore* grid_store =
      gtk_list_store_new(3, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT);
  GtkWidget* grid = gtk_icon_view_new_with_model(GTK_TREE_MODEL(grid_store));
  gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(grid), 0);
  gtk_icon_view_set_text_column(GTK_ICON_VIEW(grid), 1);
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(grid), 150);
  gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(grid), 12);
  gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(grid), 12);
  gtk_icon_view_set_margin(GTK_ICON_VIEW(grid), 12);
  g_signal_connect(grid, "item-activated",
                   G_CALLBACK(+[](GtkIconView* icon_view, GtkTreePath* path,
                                  gpointer data) {
                     auto* w = static_cast<EmulatorWindow*>(data);
                     GtkTreeModel* model = gtk_icon_view_get_model(icon_view);
                     GtkTreeIter iter;
                     if (!gtk_tree_model_get_iter(model, &iter, path)) {
                       return;
                     }
                     gint index = -1;
                     gtk_tree_model_get(model, &iter, 2, &index, -1);
                     w->LaunchLibraryIndex(index);
                   }),
                   this);
  GtkWidget* grid_scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_container_add(GTK_CONTAINER(grid_scroller), grid);
  GtkWidget* stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(stack), grid_scroller, "grid");
  gtk_stack_set_visible_child_name(
      GTK_STACK(stack), cvars::library_view == "grid" ? "grid" : "list");
  gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);
  dashboard_stack_ = stack;
  dashboard_grid_ = grid;
  dashboard_grid_store_ = grid_store;
  // List / Grid switch on the toolbar.
  GtkWidget* list_button = gtk_radio_button_new_with_label(nullptr, "List");
  GtkWidget* grid_button = gtk_radio_button_new_with_label_from_widget(
      GTK_RADIO_BUTTON(list_button), "Grid");
  gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(list_button), FALSE);
  gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(grid_button), FALSE);
  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(cvars::library_view == "grid" ? grid_button
                                                       : list_button),
      TRUE);
  auto on_view = +[](GtkToggleButton* button, gpointer data) {
    if (!gtk_toggle_button_get_active(button)) {
      return;
    }
    auto* w = static_cast<EmulatorWindow*>(data);
    bool grid = g_object_get_data(G_OBJECT(button), "grid") != nullptr;
    gtk_stack_set_visible_child_name(GTK_STACK(w->dashboard_stack_),
                                     grid ? "grid" : "list");
    OVERRIDE_string(library_view, grid ? "grid" : "list");
    config::SaveConfig();
    if (grid) {
      w->RefreshDashboardGrid();
    }
  };
  g_object_set_data(G_OBJECT(grid_button), "grid", GINT_TO_POINTER(1));
  g_signal_connect(list_button, "toggled", G_CALLBACK(on_view), this);
  g_signal_connect(grid_button, "toggled", G_CALLBACK(on_view), this);
  GtkWidget* view_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(view_box), "linked");
  gtk_box_pack_start(GTK_BOX(view_box), list_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(view_box), grid_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), view_box, FALSE, FALSE, 0);
  GtkWidget* status = gtk_label_new("");
  gtk_widget_set_halign(status, GTK_ALIGN_START);
  gtk_widget_set_margin_start(status, 8);
  gtk_widget_set_margin_top(status, 4);
  gtk_widget_set_margin_bottom(status, 4);
  gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

  dashboard_widget_ = box;
  dashboard_store_ = store;
  dashboard_filter_ = filter;
  dashboard_search_ = search;
  dashboard_type_ = type;
  dashboard_region_ = region;
  dashboard_status_ = status;
  dashboard_back_ = back;
  dashboard_banner_ = banner;
  dashboard_banner_label_ = banner_label;

  auto refilter = [](GtkWidget*, gpointer data) {
    auto* w = static_cast<EmulatorWindow*>(data);
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(w->dashboard_filter_));
    w->RefreshDashboardGrid();
  };
  g_signal_connect(search, "search-changed", G_CALLBACK(+refilter), this);
  g_signal_connect(type, "changed", G_CALLBACK(+refilter), this);
  g_signal_connect(region, "changed", G_CALLBACK(+refilter), this);
  g_signal_connect(rescan, "clicked",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     auto* w = static_cast<EmulatorWindow*>(data);
                     w->ScanLibrary();
                     w->RefreshDashboard();
                   }),
                   this);
  g_signal_connect(folder, "clicked",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     auto* w = static_cast<EmulatorWindow*>(data);
                     w->PickGamesDir();
                     w->ScanLibrary();
                     w->RefreshDashboard();
                   }),
                   this);
  g_signal_connect(back, "clicked",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     static_cast<EmulatorWindow*>(data)->ShowDashboard(false);
                   }),
                   this);
  // Double-click: launch.
  g_signal_connect(
      view, "row-activated",
      G_CALLBACK(+[](GtkTreeView* tree, GtkTreePath* path, GtkTreeViewColumn*,
                     gpointer data) {
        auto* w = static_cast<EmulatorWindow*>(data);
        GtkTreeModel* model = gtk_tree_view_get_model(tree);
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(model, &iter, path)) {
          return;
        }
        gint index = -1;
        gtk_tree_model_get(model, &iter, kColIndex, &index, -1);
        if (index >= 0 && index < int(w->library_titles_.size())) {
          auto title_path = w->library_titles_[index].path;
          w->app_context().CallInUIThread(
              [w, title_path]() { w->RunTitle(title_path); });
        }
      }),
      this);
  // Right-click: rating and folder.
  g_signal_connect(
      view, "button-press-event",
      G_CALLBACK(+[](GtkWidget* widget, GdkEventButton* event,
                     gpointer data) -> gboolean {
        if (event->button != 3) {
          return FALSE;
        }
        auto* w = static_cast<EmulatorWindow*>(data);
        GtkTreePath* path = nullptr;
        if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), int(event->x),
                                           int(event->y), &path, nullptr,
                                           nullptr, nullptr)) {
          return FALSE;
        }
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, nullptr, FALSE);
        GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
        GtkTreeIter iter;
        gint index = -1;
        if (gtk_tree_model_get_iter(model, &iter, path)) {
          gtk_tree_model_get(model, &iter, kColIndex, &index, -1);
        }
        gtk_tree_path_free(path);
        if (index < 0 || index >= int(w->library_titles_.size())) {
          return TRUE;
        }
        w->dashboard_menu_index_ = index;
        GtkWidget* menu = gtk_menu_new();
        auto add = [&](const char* label, int rating) {
          GtkWidget* item = gtk_menu_item_new_with_label(label);
          g_object_set_data(G_OBJECT(item), "rating", GINT_TO_POINTER(rating));
          g_signal_connect(item, "activate",
                           G_CALLBACK(+[](GtkWidget* item, gpointer data) {
                             auto* w = static_cast<EmulatorWindow*>(data);
                             int rating = GPOINTER_TO_INT(
                                 g_object_get_data(G_OBJECT(item), "rating"));
                             int i = w->dashboard_menu_index_;
                             if (i < 0 || i >= int(w->library_titles_.size())) {
                               return;
                             }
                             if (rating == -1) {
                               std::thread(LaunchFileExplorer,
                                           w->library_titles_[i].path.parent_path())
                                   .detach();
                               return;
                             }
                             if (rating == -2) {
                               auto p = w->library_titles_[i].path;
                               w->app_context().CallInUIThread(
                                   [w, p]() { w->RunTitle(p); });
                               return;
                             }
                             w->library_titles_[i].rating = rating;
                             w->SaveLibrary();
                             w->RefreshDashboard();
                           }),
                           w);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        };
        add("Launch", -2);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        for (int r = 5; r >= 1; --r) {
          add((RatingText(r) + fmt::format("  {} star{}", r, r == 1 ? "" : "s")).c_str(), r);
        }
        add("No rating", 0);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        add("Open folder", -1);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(event));
        return TRUE;
      }),
      this);

  gtk_main->SetIdleWidget(box);
  RefreshDashboard();
}

void EmulatorWindow::RefreshDashboard() {
  if (!dashboard_store_) {
    return;
  }
  auto* store = static_cast<GtkListStore*>(dashboard_store_);
  gtk_list_store_clear(store);
  uint64_t total_seconds = 0;
  for (size_t i = 0; i < library_titles_.size(); ++i) {
    const auto& t = library_titles_[i];
    total_seconds += t.seconds_played;
    std::string name = t.title_name;
    if (name.empty() && t.title_id) {
      // Another disc of the same title may have been played.
      for (const auto& other : library_titles_) {
        if (other.title_id == t.title_id && !other.title_name.empty()) {
          name = other.title_name;
          break;
        }
      }
    }
    if (name.empty()) {
      name = t.title_id ? "(not played yet)" : "(unreadable)";
    }
    std::string discs =
        t.disc_count > 1 ? fmt::format("{} of {}", t.disc_number, t.disc_count)
                         : "";
    if (t.disc_count > 1) {
      // In the title, where the eye is: "Lost Odyssey [Disc 1 of 4]".
      name += fmt::format(" [Disc {} of {}]", t.disc_number, t.disc_count);
    }
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(
        store, &iter, kColType, t.type.c_str(), kColTitleId,
        t.title_id ? fmt::format("{:08X}", t.title_id).c_str() : "", kColTitle,
        name.c_str(), kColTimePlayed, TimePlayedText(t.seconds_played).c_str(),
        kColLastPlayed, DateText(t.last_played).c_str(), kColSize,
        fmt::format("{:.2f} MB", double(t.size) / (1024.0 * 1024.0)).c_str(),
        kColRegion, RegionText(t.region).c_str(), kColDiscs, discs.c_str(),
        kColRating, RatingText(t.rating).c_str(), kColIndex, int(i), kColSeconds,
        gint64(t.seconds_played), kColLastPlayedTs, gint64(t.last_played),
        kColSizeBytes, gint64(t.size), kColRatingValue, t.rating, kColIcon,
        TitleIconPixbuf(t.title_id, 32), -1);
  }
  std::string status =
      cvars::games_dir.empty()
          ? "No games folder set: Games folder... above, or Settings > "
            "Preferences > Folders."
          : fmt::format("{} title(s) in {}   |   {} played in total   |   "
                        "double-click launches, right-click rates",
                        library_titles_.size(), cvars::games_dir,
                        TimePlayedText(int64_t(total_seconds)).empty()
                            ? "nothing"
                            : TimePlayedText(int64_t(total_seconds)));
  gtk_label_set_text(GTK_LABEL(dashboard_status_), status.c_str());
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(dashboard_filter_));
  RefreshDashboardGrid();
}

std::filesystem::path EmulatorWindow::TitleIconPath(uint32_t title_id) const {
  return emulator_->storage_root() / "library" / "icons" /
         fmt::format("{:08X}.png", title_id);
}

void EmulatorWindow::SaveTitleIcon() {
  auto* db = emulator_->game_info_database();
  if (!db || !emulator_->is_title_open() || !emulator_->title_id()) {
    return;
  }
  auto icon = db->GetIcon();
  if (icon.empty()) {
    return;
  }
  auto path = TitleIconPath(emulator_->title_id());
  std::error_code ec;
  if (std::filesystem::exists(path, ec) &&
      std::filesystem::file_size(path, ec) == icon.size()) {
    return;
  }
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(icon.data()), icon.size());
  XELOGI("Library: icon of {:08X} written ({} bytes)", emulator_->title_id(),
         icon.size());
  for (auto it = dashboard_icons_.begin(); it != dashboard_icons_.end();) {
    if ((it->first >> 8) == emulator_->title_id()) {
      g_object_unref(G_OBJECT(it->second));
      it = dashboard_icons_.erase(it);
    } else {
      ++it;
    }
  }
}

void* EmulatorWindow::TitleIconPixbuf(uint32_t title_id, int size) {
  uint64_t key = (uint64_t(title_id) << 8) | uint64_t(size & 0xFF);
  auto it = dashboard_icons_.find(key);
  if (it != dashboard_icons_.end()) {
    return it->second;
  }
  GdkPixbuf* pixbuf = nullptr;
  if (title_id) {
    auto path = TitleIconPath(title_id);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      pixbuf = gdk_pixbuf_new_from_file_at_scale(path.string().c_str(), size,
                                                 size, TRUE, nullptr);
    }
  }
  if (!pixbuf) {
    // Not launched yet (the icon lives inside the XEX, read at launch): a
    // grey tile.
    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
    gdk_pixbuf_fill(pixbuf, 0x3a3a3aff);
  }
  dashboard_icons_[key] = pixbuf;
  return pixbuf;
}

void EmulatorWindow::RefreshDashboardGrid() {
  if (!dashboard_grid_store_ || !dashboard_stack_) {
    return;
  }
  const char* visible =
      gtk_stack_get_visible_child_name(GTK_STACK(dashboard_stack_));
  if (!visible || std::strcmp(visible, "grid") != 0) {
    return;
  }
  auto* grid_store = static_cast<GtkListStore*>(dashboard_grid_store_);
  gtk_list_store_clear(grid_store);
  auto* model = static_cast<GtkTreeModel*>(dashboard_filter_);
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter_first(model, &iter)) {
    return;
  }
  do {
    gint index = -1;
    gchar* title = nullptr;
    gtk_tree_model_get(model, &iter, kColIndex, &index, kColTitle, &title, -1);
    if (index >= 0 && index < int(library_titles_.size())) {
      const auto& t = library_titles_[index];
      // The title text already carries "[Disc N of M]".
      std::string label = title && *title ? title : t.path.stem().string();
      GtkTreeIter row;
      gtk_list_store_append(grid_store, &row);
      gtk_list_store_set(grid_store, &row, 0,
                         TitleIconPixbuf(t.title_id, 96), 1, label.c_str(),
                         2, index, -1);
    }
    g_free(title);
  } while (gtk_tree_model_iter_next(model, &iter));
}

void EmulatorWindow::LaunchLibraryIndex(int index) {
  if (index < 0 || index >= int(library_titles_.size())) {
    return;
  }
  auto path = library_titles_[index].path;
  app_context().CallInUIThread([this, path]() { RunTitle(path); });
}

void EmulatorWindow::ShowDashboard(bool show) {
  auto* gtk_main = dynamic_cast<ui::GTKWindow*>(window_.get());
  if (!gtk_main || !dashboard_widget_) {
    return;
  }
  if (show) {
    RefreshDashboard();
  }
  gtk_main->ShowIdleWidget(show);
  if (dashboard_banner_) {
    bool running = show && emulator_->is_title_open();
    if (running) {
      std::string name = emulator_->title_name().empty()
                             ? fmt::format("{:08X}", emulator_->title_id())
                             : emulator_->title_name();
      gtk_label_set_markup(
          GTK_LABEL(dashboard_banner_label_),
          fmt::format("<span size=\"large\"><b>{} is still running</b></span>\n"
                      "The game keeps going behind this list; go back to it "
                      "or launch another title (that restarts the emulator).",
                      g_markup_escape_text(name.c_str(), -1))
              .c_str());
      // show_all skips a no-show-all widget's children; show them by hand.
      gtk_widget_show(GTK_WIDGET(dashboard_banner_label_));
      gtk_widget_show(GTK_WIDGET(dashboard_back_));
    }
    gtk_widget_set_visible(GTK_WIDGET(dashboard_banner_), running);
  }
}

void EmulatorWindow::ToggleDashboard() {
  auto* gtk_main = dynamic_cast<ui::GTKWindow*>(window_.get());
  if (!gtk_main) {
    return;
  }
  ShowDashboard(!gtk_main->idle_widget_shown());
}

void EmulatorWindow::OnDashboardTitleLaunched() {
  AddPlayTime();
  session_running_ = true;
  session_start_ = std::chrono::steady_clock::now();
  session_path_ = last_launched_path_;
  LibraryTitle* title = LibraryEntryFor(last_launched_path_);
  if (!title) {
    LibraryTitle fresh;
    fresh.path = last_launched_path_;
    std::string ext = xe::utf8::lower_ascii(last_launched_path_.extension().string());
    fresh.type = ext == ".iso" ? "ISO" : ext == ".xex" ? "XEX" : ext == ".zar" ? "ZAR" : "";
    std::error_code ec;
    fresh.size = std::filesystem::file_size(last_launched_path_, ec);
    ReadTitleInfo(fresh);
    library_titles_.push_back(std::move(fresh));
    title = &library_titles_.back();
  }
  if (emulator_->is_title_open()) {
    title->title_id = emulator_->title_id();
    if (!emulator_->title_name().empty()) {
      title->title_name = emulator_->title_name();
    }
  }
  title->last_played = int64_t(time(nullptr));
  SaveLibrary();
  ShowDashboard(false);
}

void EmulatorWindow::AddPlayTime() {
  if (!session_running_) {
    return;
  }
  session_running_ = false;
  int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - session_start_)
                        .count();
  if (LibraryTitle* title = LibraryEntryFor(session_path_)) {
    title->seconds_played += seconds;
    XELOGI("Library: {} played {} s this session, {} s in total",
           title->title_name, seconds, title->seconds_played);
  }
}


// ---- Preferences: Profiles tab ----

// ---- Patches tab ----

std::map<uint32_t, std::string> EmulatorWindow::PatchTitles() {
  std::map<uint32_t, std::string> titles;
  auto add = [&titles](uint32_t id, const std::string& name) {
    if (!id) {
      return;
    }
    auto it = titles.find(id);
    if (it == titles.end()) {
      titles[id] = name;
    } else if (it->second.empty() && !name.empty()) {
      it->second = name;
    }
  };
  if (emulator_->is_title_open()) {
    add(emulator_->title_id(), emulator_->title_name());
  }
  for (const LibraryTitle& title : library_titles_) {
    add(title.title_id, title.title_name.empty()
                            ? title.path.stem().string()
                            : title.title_name);
  }
  if (auto* patcher = emulator_->patcher()) {
    for (const auto& file : patcher->patch_db()->GetAllPatches()) {
      add(file.title_id, file.title_name);
    }
  }
  return titles;
}

void EmulatorWindow::BuildPatchesTab(void* notebook_ptr) {
  auto* notebook = static_cast<GtkWidget*>(notebook_ptr);
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 12);

  std::filesystem::path folder = emulator_->storage_root() / "patches";
  GtkWidget* intro = LeftLabel(
      fmt::format("Patch files (<tt>&lt;title id&gt; - &lt;name&gt;.patch.toml</tt>) "
                  "in {}. A change here applies when the game next starts.",
                  folder.string())
          .c_str());
  gtk_label_set_use_markup(GTK_LABEL(intro), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
  gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

  GtkWidget* enable =
      gtk_check_button_new_with_label("Apply enabled patches when a game starts");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable), cvars::apply_patches);
  SetTooltipFromCvar(enable, "apply_patches");
  AttachSettingsCallback(enable, "toggled", [this](GtkWidget* w) {
    SetGpuOption<bool>("apply_patches",
                       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)));
    RefreshPatchesTab();
  });
  gtk_box_pack_start(GTK_BOX(box), enable, FALSE, FALSE, 0);

  GtkWidget* title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(title_row), LeftLabel("Game"), FALSE, FALSE, 0);
  GtkWidget* combo = gtk_combo_box_text_new();
  gtk_widget_set_hexpand(combo, TRUE);
  AttachSettingsCallback(combo, "changed", [this](GtkWidget* w) {
    if (patches_refreshing_) {
      return;
    }
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    if (index >= 0 && index < int(patches_combo_title_ids_.size())) {
      patches_selected_title_ = patches_combo_title_ids_[index];
      RefreshPatchesTab();
    }
  });
  gtk_box_pack_start(GTK_BOX(title_row), combo, TRUE, TRUE, 0);
  GtkWidget* reload = gtk_button_new_with_label("Reload files");
  AttachSettingsCallback(reload, "clicked", [this](GtkWidget*) {
    if (auto* patcher = emulator_->patcher()) {
      patcher->patch_db()->Reload(true);
    }
    RefreshPatchesTab();
  });
  gtk_box_pack_start(GTK_BOX(title_row), reload, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), title_row, FALSE, FALSE, 0);

  GtkWidget* status = LeftLabel("");
  gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
  gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(box), list, FALSE, FALSE, 0);

  // Community repository.
  gtk_box_pack_start(GTK_BOX(box),
                     HeadingLabel("Community patches (xenia-canary/game-patches "
                                  "on GitHub)"),
                     FALSE, FALSE, 0);
  GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* lookup = gtk_button_new_with_label("Look up");
  gtk_widget_set_tooltip_text(
      lookup, "Fetch the list of patch files in the repository (needs curl "
              "and a network connection) and match it against your games.");
  AttachSettingsCallback(lookup, "clicked",
                         [this](GtkWidget*) { LookupCommunityPatches(); });
  gtk_box_pack_start(GTK_BOX(actions), lookup, FALSE, FALSE, 0);
  GtkWidget* get_all = gtk_button_new_with_label("Download all for my games");
  AttachSettingsCallback(get_all, "clicked", [this](GtkWidget*) {
    std::map<uint32_t, std::string> titles = PatchTitles();
    auto recorded = LoadCommunityShas(emulator_->storage_root());
    for (const CommunityPatchFile& file : community_patch_files_) {
      if (!titles.count(file.title_id)) {
        continue;
      }
      if (StateOfCommunityFile(emulator_->storage_root(), file.name, file.sha,
                               recorded) == CommunityFileState::kCurrent) {
        continue;
      }
      DownloadCommunityPatch(file.name);
    }
  });
  gtk_box_pack_start(GTK_BOX(actions), get_all, FALSE, FALSE, 0);
  GtkWidget* show_all = gtk_check_button_new_with_label("All titles");
  AttachSettingsCallback(show_all, "toggled",
                         [this](GtkWidget*) { RefreshCommunityPatchList(); });
  gtk_box_pack_start(GTK_BOX(actions), show_all, FALSE, FALSE, 0);
  GtkWidget* filter = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(filter), "Filter by name or title id");
  gtk_widget_set_hexpand(filter, TRUE);
  AttachSettingsCallback(filter, "changed",
                         [this](GtkWidget*) { RefreshCommunityPatchList(); });
  gtk_box_pack_start(GTK_BOX(actions), filter, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
  GtkWidget* community_status = LeftLabel("Not looked up yet.");
  gtk_label_set_line_wrap(GTK_LABEL(community_status), TRUE);
  gtk_box_pack_start(GTK_BOX(box), community_status, FALSE, FALSE, 0);
  GtkWidget* community_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(box), community_list, FALSE, FALSE, 0);

  patches_status_ = status;
  patches_combo_ = combo;
  patches_box_ = list;
  community_status_ = community_status;
  community_box_ = community_list;
  community_show_all_ = show_all;
  community_filter_ = filter;

  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroller), box);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroller,
                           gtk_label_new("Patches"));
  RefreshPatchesTab();
}

void EmulatorWindow::RefreshPatchesTab() {
  if (!patches_box_) {
    return;
  }
  patches_refreshing_ = true;
  auto* patcher = emulator_->patcher();
  std::map<uint32_t, std::string> titles = PatchTitles();
  uint32_t running = emulator_->is_title_open() ? emulator_->title_id() : 0;

  // The game combo: the running title first, then the rest by name.
  auto* combo = GTK_COMBO_BOX_TEXT(patches_combo_);
  gtk_combo_box_text_remove_all(combo);
  patches_combo_title_ids_.clear();
  std::vector<std::pair<std::string, uint32_t>> ordered;
  for (const auto& [id, name] : titles) {
    if (id != running) {
      ordered.emplace_back(name, id);
    }
  }
  std::sort(ordered.begin(), ordered.end());
  if (running) {
    ordered.insert(ordered.begin(), {titles[running], running});
  }
  if (!patches_selected_title_ || !titles.count(patches_selected_title_)) {
    patches_selected_title_ = ordered.empty() ? 0 : ordered.front().second;
  }
  int active = 0;
  for (const auto& [name, id] : ordered) {
    std::string label = fmt::format("{} ({:08X}){}", name.empty() ? "?" : name,
                                    id, id == running ? "  [running]" : "");
    gtk_combo_box_text_append_text(combo, label.c_str());
    if (id == patches_selected_title_) {
      active = int(patches_combo_title_ids_.size());
    }
    patches_combo_title_ids_.push_back(id);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);

  // The title's patch files.
  ClearChildren(GTK_WIDGET(patches_box_));
  std::filesystem::path folder = emulator_->storage_root() / "patches";
  std::optional<uint64_t> running_hash;
  if (running && emulator_->kernel_state()) {
    auto module = emulator_->kernel_state()->GetExecutableModule();
    if (module) {
      running_hash = module->hash();
    }
  }
  size_t file_count = 0;
  if (patcher && patches_selected_title_) {
    for (const auto& file : patcher->patch_db()->GetAllPatches()) {
      if (file.title_id != patches_selected_title_) {
        continue;
      }
      ++file_count;
      std::string heading = file.file_path.filename().string();
      // A file for another version of the running game (its hashes do not
      // include the running module's) is folded away: it will not apply.
      bool other_version = false;
      std::string note_text;
      if (running_hash && file.title_id == running) {
        bool match = std::find(file.hashes.begin(), file.hashes.end(),
                               *running_hash) != file.hashes.end();
        std::string hashes;
        for (uint64_t h : file.hashes) {
          hashes += fmt::format("{}{:016X}", hashes.empty() ? "" : ", ", h);
        }
        other_version = !match;
        note_text = match ? "Matches the running copy of the game."
                          : fmt::format("For a different version of the game "
                                        "(file: {}; running: {:016X}); it "
                                        "will not apply.",
                                        hashes, *running_hash);
      }
      GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      if (other_version) {
        GtkWidget* expander = gtk_expander_new(nullptr);
        GtkWidget* title = gtk_label_new(nullptr);
        gtk_label_set_markup(
            GTK_LABEL(title),
            fmt::format("{}   <i>another version of the game, not applied</i>",
                        g_markup_escape_text(heading.c_str(), -1))
                .c_str());
        gtk_expander_set_label_widget(GTK_EXPANDER(expander), title);
        gtk_expander_set_expanded(GTK_EXPANDER(expander), FALSE);
        gtk_widget_set_margin_top(expander, 8);
        gtk_container_add(GTK_CONTAINER(expander), section);
        gtk_widget_set_margin_start(section, 16);
        gtk_box_pack_start(GTK_BOX(patches_box_), expander, FALSE, FALSE, 0);
      } else {
        GtkWidget* head = HeadingLabel(heading.c_str());
        gtk_box_pack_start(GTK_BOX(patches_box_), head, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(patches_box_), section, FALSE, FALSE, 0);
      }
      if (!note_text.empty()) {
        GtkWidget* note = LeftLabel(note_text.c_str());
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_box_pack_start(GTK_BOX(section), note, FALSE, FALSE, 0);
      }
      for (const auto& patch : file.patch_info) {
        GtkWidget* check =
            gtk_check_button_new_with_label(patch.patch_name.c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
                                     patch.is_enabled);
        std::string tip = patch.patch_desc;
        if (!patch.patch_author.empty()) {
          tip += (tip.empty() ? "" : "\n") + std::string("Author: ") +
                 patch.patch_author;
        }
        if (!tip.empty()) {
          gtk_widget_set_tooltip_text(check, tip.c_str());
        }
        std::filesystem::path path = file.file_path;
        std::string name = patch.patch_name;
        AttachSettingsCallback(
            check, "toggled", [this, path, name](GtkWidget* w) {
              bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
              bool ok = SetPatchEnabledInFile(path, name, on);
              XELOGI("Patches: {} '{}' in {} {}", on ? "enable" : "disable",
                     name, path.filename().string(), ok ? "ok" : "FAILED");
              if (auto* patcher = emulator_->patcher()) {
                patcher->patch_db()->Reload(true);
              }
              gtk_label_set_text(
                  GTK_LABEL(patches_status_),
                  ok ? (emulator_->is_title_open()
                            ? "Saved. Restart the game to apply the change."
                            : "Saved. Applies when the game starts.")
                     : "Could not write the patch file.");
            });
        gtk_box_pack_start(GTK_BOX(section), check, FALSE, FALSE, 0);
        if (!patch.patch_desc.empty()) {
          GtkWidget* desc = LeftLabel(patch.patch_desc.c_str());
          gtk_label_set_line_wrap(GTK_LABEL(desc), TRUE);
          gtk_widget_set_margin_start(desc, 28);
          gtk_widget_set_sensitive(desc, FALSE);
          gtk_box_pack_start(GTK_BOX(section), desc, FALSE, FALSE, 0);
        }
      }
    }
  }
  if (running && file_count) {
    GtkWidget* restart = gtk_button_new_with_label("Restart the game");
    gtk_widget_set_halign(restart, GTK_ALIGN_START);
    gtk_widget_set_margin_top(restart, 6);
    AttachSettingsCallback(restart, "clicked",
                           [this](GtkWidget*) { ResetGame(); });
    gtk_box_pack_start(GTK_BOX(patches_box_), restart, FALSE, FALSE, 0);
  }
  std::string status;
  if (!patcher) {
    status = "No patcher.";
  } else if (titles.empty()) {
    status = "No games known yet: run one, or set the games folder.";
  } else if (!file_count) {
    status = fmt::format("No patch file for this game in {}. Look it up in the "
                         "community list below.",
                         folder.string());
  } else if (!cvars::apply_patches) {
    status = "Patching is off: enable it above for these to apply.";
  }
  gtk_label_set_text(GTK_LABEL(patches_status_), status.c_str());
  gtk_widget_show_all(GTK_WIDGET(patches_box_));
  patches_refreshing_ = false;
  RefreshCommunityPatchList();
}

void EmulatorWindow::RefreshCommunityPatchList() {
  if (!community_box_) {
    return;
  }
  ClearChildren(GTK_WIDGET(community_box_));
  if (!community_looked_up_) {
    return;
  }
  std::map<uint32_t, std::string> titles = PatchTitles();
  bool show_all =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(community_show_all_));
  std::string filter = gtk_entry_get_text(GTK_ENTRY(community_filter_));
  std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
  auto recorded = LoadCommunityShas(emulator_->storage_root());
  const size_t kMaxRows = 200;
  size_t shown = 0, matched = 0;
  for (const CommunityPatchFile& file : community_patch_files_) {
    bool mine = titles.count(file.title_id) != 0;
    if (!show_all && !mine) {
      continue;
    }
    if (!filter.empty()) {
      std::string lower = file.name;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (lower.find(filter) == std::string::npos) {
        continue;
      }
    }
    ++matched;
    if (shown >= kMaxRows) {
      continue;
    }
    ++shown;
    CommunityFileState state = StateOfCommunityFile(
        emulator_->storage_root(), file.name, file.sha, recorded);
    bool installed = state != CommunityFileState::kMissing;
    bool current = state == CommunityFileState::kCurrent;
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    std::string text = fmt::format(
        "{}{}   <i>{}</i>", mine ? "" : "",
        g_markup_escape_text(file.name.c_str(), -1),
        current ? "installed, up to date"
                : installed ? "installed, update available" : "not installed");
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), text.c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    if (!current) {
      GtkWidget* get =
          gtk_button_new_with_label(installed ? "Update" : "Download");
      std::string name = file.name;
      AttachSettingsCallback(get, "clicked", [this, name](GtkWidget*) {
        DownloadCommunityPatch(name);
      });
      gtk_box_pack_start(GTK_BOX(row), get, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(community_box_), row, FALSE, FALSE, 0);
  }
  std::string status = fmt::format(
      "{} files in the repository; {} {}{}.", community_patch_files_.size(),
      matched, show_all ? "listed" : "for your games",
      matched > shown ? fmt::format(" (showing {}, narrow the filter)", shown)
                      : "");
  if (community_downloads_running_) {
    status += fmt::format(" Downloading {}...", community_downloads_running_);
  }
  gtk_label_set_text(GTK_LABEL(community_status_), status.c_str());
  gtk_widget_show_all(GTK_WIDGET(community_box_));
}

void EmulatorWindow::LookupCommunityPatches() {
  if (community_lookup_running_) {
    return;
  }
  community_lookup_running_ = true;
  gtk_label_set_text(GTK_LABEL(community_status_), "Looking up...");
  std::thread([this]() {
    xe::threading::set_name("Patch lookup");
    int code = 0;
    std::string json = RunCommandCapture(
        fmt::format("curl -sSfL --max-time 60 -H 'User-Agent: xenia-canary' "
                    "-H 'Accept: application/vnd.github+json' {} 2>&1",
                    ShellQuote(kCommunityPatchesTreeUrl)),
        &code);
    std::vector<CommunityPatchFile> files;
    std::string error;
    if (code != 0) {
      error = fmt::format("curl failed ({}): {}", code,
                          json.substr(0, 200));
    } else {
      size_t pos = 0;
      const std::string key = "\"path\":\"";
      while ((pos = json.find(key, pos)) != std::string::npos) {
        pos += key.size();
        std::string raw;
        size_t end = pos;
        while (end < json.size() && json[end] != '"') {
          if (json[end] == '\\' && end + 1 < json.size()) {
            raw += json[end];
            raw += json[end + 1];
            end += 2;
            continue;
          }
          raw += json[end++];
        }
        std::string path = JsonUnescape(raw);
        size_t sha_pos = json.find("\"sha\":\"", end);
        std::string sha = sha_pos == std::string::npos
                              ? ""
                              : json.substr(sha_pos + 7, 40);
        pos = end;
        const std::string prefix = "patches/";
        const std::string suffix = ".patch.toml";
        if (path.rfind(prefix, 0) != 0 || path.size() < suffix.size() ||
            path.compare(path.size() - suffix.size(), suffix.size(),
                         suffix) != 0) {
          continue;
        }
        std::string name = path.substr(prefix.size());
        if (name.size() < 8) {
          continue;
        }
        CommunityPatchFile file;
        file.name = name;
        file.sha = sha;
        file.title_id = uint32_t(strtoul(name.substr(0, 8).c_str(), nullptr, 16));
        files.push_back(std::move(file));
      }
      if (files.empty()) {
        error = "No patch files in the reply: " + json.substr(0, 200);
      }
    }
    PostToUIThread([this, files, error]() {
      community_lookup_running_ = false;
      if (!error.empty()) {
        XELOGE("Patches: community lookup failed: {}", error);
        if (community_status_) {
          gtk_label_set_text(GTK_LABEL(community_status_), error.c_str());
        }
        return;
      }
      community_patch_files_ = files;
      std::sort(community_patch_files_.begin(), community_patch_files_.end(),
                [](const CommunityPatchFile& a, const CommunityPatchFile& b) {
                  return a.name < b.name;
                });
      community_looked_up_ = true;
      XELOGI("Patches: community list has {} files", files.size());
      RefreshCommunityPatchList();
    });
  }).detach();
}

void EmulatorWindow::DownloadCommunityPatch(const std::string& name) {
  std::filesystem::path folder = emulator_->storage_root() / "patches";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::filesystem::path target = folder / name;
  std::filesystem::path temp = folder / (name + ".download");
  std::string url = kCommunityPatchesRawUrl + UrlEncodeComponent(name);
  ++community_downloads_running_;
  RefreshCommunityPatchList();
  std::thread([this, name, url, target, temp]() {
    xe::threading::set_name("Patch download");
    int code = 0;
    std::string out = RunCommandCapture(
        fmt::format("curl -sSfL --max-time 60 -H 'User-Agent: xenia-canary' "
                    "-o {} {} 2>&1",
                    ShellQuote(temp.string()), ShellQuote(url)),
        &code);
    PostToUIThread([this, name, target, temp, code, out]() {
      --community_downloads_running_;
      std::error_code ec;
      if (code != 0) {
        XELOGE("Patches: download of {} failed ({}): {}", name, code, out);
        std::filesystem::remove(temp, ec);
        if (community_status_) {
          gtk_label_set_text(
              GTK_LABEL(community_status_),
              fmt::format("Download of {} failed: {}", name, out).c_str());
        }
        return;
      }
      // Keep what was enabled in the old copy.
      std::vector<std::string> enabled;
      auto* patcher = emulator_->patcher();
      if (patcher && std::filesystem::exists(target)) {
        auto old = patcher->patch_db()->ReadPatchFile(target);
        for (const auto& patch : old.patch_info) {
          if (patch.is_enabled) {
            enabled.push_back(patch.patch_name);
          }
        }
      }
      std::filesystem::rename(temp, target, ec);
      if (ec) {
        XELOGE("Patches: cannot move {} into place: {}", name, ec.message());
        return;
      }
      for (const std::string& patch_name : enabled) {
        SetPatchEnabledInFile(target, patch_name, true);
      }
      for (const CommunityPatchFile& file : community_patch_files_) {
        if (file.name == name) {
          RecordCommunitySha(emulator_->storage_root(), name, file.sha);
          break;
        }
      }
      XELOGI("Patches: downloaded {}{}", name,
             enabled.empty()
                 ? ""
                 : fmt::format(" ({} previously enabled kept)", enabled.size()));
      if (patcher) {
        patcher->patch_db()->Reload(true);
      }
      RefreshPatchesTab();
    });
  }).detach();
}

void EmulatorWindow::BuildProfilesTab(void* notebook_ptr) {
  auto* notebook = static_cast<GtkWidget*>(notebook_ptr);
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 12);
  GtkWidget* status = LeftLabel("");
  gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
  gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_container_add(GTK_CONTAINER(scroller), list);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

  // Create a profile.
  gtk_box_pack_start(GTK_BOX(box), HeadingLabel("New profile"), FALSE, FALSE, 0);
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* name = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Gamertag (up to 15 characters)");
  gtk_entry_set_max_length(GTK_ENTRY(name), 15);
  gtk_widget_set_hexpand(name, TRUE);
  GtkWidget* autologin = gtk_check_button_new_with_label("Sign in at start-up");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(autologin), TRUE);
  GtkWidget* create = gtk_button_new_with_label("Create");
  gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(row), autologin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), create, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  AttachSettingsCallback(create, "clicked", [this, name, autologin](GtkWidget*) {
    std::string gamertag = gtk_entry_get_text(GTK_ENTRY(name));
    auto* pm = emulator_->kernel_state()->xam_state()->profile_manager();
    if (!pm || !kernel::xam::ProfileManager::IsGamertagValid(gamertag)) {
      new xe::ui::HostNotificationWindow(
          imgui_drawer(), "Profiles",
          "Gamertag: 1 to 15 letters, digits and spaces, not starting with "
          "a digit or a space.",
          0);
      return;
    }
    bool ok = pm->CreateProfile(
        gamertag, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(autologin)));
    XELOGI("Profiles: create '{}' {}", gamertag, ok ? "ok" : "FAILED");
    gtk_entry_set_text(GTK_ENTRY(name), "");
    RefreshProfilesTab();
  });
  GtkWidget* note = LeftLabel(
      "Migrate data from an older content layout and edit gamercards with the "
      "in-window panel:");
  gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
  gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
  GtkWidget* panel = gtk_button_new_with_label("Open the profile panel...");
  gtk_widget_set_halign(panel, GTK_ALIGN_START);
  AttachSettingsCallback(panel, "clicked",
                         [this](GtkWidget*) { ToggleProfilesConfigDialog(); });
  gtk_box_pack_start(GTK_BOX(box), panel, FALSE, FALSE, 0);

  profiles_list_ = list;
  profiles_status_ = status;
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), TabScroller(box),
                           gtk_label_new("Profiles"));
  RefreshProfilesTab();
}

void EmulatorWindow::RefreshProfilesTab() {
  if (!profiles_list_) {
    return;
  }
  auto* list = GTK_CONTAINER(profiles_list_);
  GList* children = gtk_container_get_children(list);
  for (GList* l = children; l; l = l->next) {
    gtk_widget_destroy(GTK_WIDGET(l->data));
  }
  g_list_free(children);
  auto* pm = emulator_->kernel_state() && emulator_->kernel_state()->xam_state()
                 ? emulator_->kernel_state()->xam_state()->profile_manager()
                 : nullptr;
  if (!pm) {
    gtk_label_set_text(GTK_LABEL(profiles_status_), "No profile manager.");
    return;
  }
  const auto* accounts = pm->GetAccounts();
  size_t signed_in = 0;
  for (const auto& [xuid, account] : *accounts) {
    uint8_t slot = pm->GetUserIndexAssignedToProfile(xuid);
    bool is_signed_in = slot != XUserIndexAny;
    signed_in += is_signed_in;
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row), 4);
    std::string text = fmt::format(
        "<b>{}</b>   {:016X}   {}", account.GetGamertagString(), xuid,
        is_signed_in ? fmt::format("signed in, slot {}", slot + 1)
                     : "not signed in");
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), text.c_str());
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    auto add_button = [&](const char* caption, std::function<void()> fn,
                          bool enabled = true) {
      GtkWidget* b = gtk_button_new_with_label(caption);
      gtk_widget_set_sensitive(b, enabled);
      AttachSettingsCallback(b, "clicked", [fn](GtkWidget*) { fn(); });
      gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    };
    uint64_t id = xuid;
    if (is_signed_in) {
      add_button("Sign out", [this, pm, slot]() {
        pm->Logout(slot);
        RefreshProfilesTab();
      });
    } else {
      add_button("Sign in", [this, pm, id]() {
        pm->Login(id);
        RefreshProfilesTab();
      }, pm->IsAnyProfileSlotFree());
      GtkWidget* slot_combo = gtk_combo_box_text_new();
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot_combo), "Slot...");
      for (int i = 1; i <= XUserMaxUserCount; ++i) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot_combo),
                                       fmt::format("Slot {}", i).c_str());
      }
      gtk_combo_box_set_active(GTK_COMBO_BOX(slot_combo), 0);
      AttachSettingsCallback(slot_combo, "changed", [this, pm, id](GtkWidget* w) {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
        if (index >= 1) {
          uint8_t target = uint8_t(index - 1);
          if (auto* current = pm->GetProfile(target)) {
            pm->Logout(target);
          }
          pm->Login(id, target);
          RefreshProfilesTab();
        }
      });
      gtk_box_pack_start(GTK_BOX(row), slot_combo, FALSE, FALSE, 0);
    }
    add_button("Modify...", [this, id]() {
      new kernel::xam::ui::GamercardUI(window_.get(), imgui_drawer(),
                                       emulator_->kernel_state(), id);
    });
    add_button("Content folder", [pm, id]() {
      std::thread(LaunchFileExplorer, pm->GetProfileContentPath(id)).detach();
    });
    add_button("Delete...", [this, pm, id, account]() {
      GtkWidget* dialog = gtk_message_dialog_new(
          GTK_WINDOW(settings_window_), GTK_DIALOG_MODAL,
          GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
          "Delete profile %s (%016lX) and everything saved under it?",
          account.GetGamertagString().c_str(), (unsigned long)id);
      int answer = gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
      if (answer == GTK_RESPONSE_YES) {
        pm->DeleteProfile(id);
        RefreshProfilesTab();
      }
    }, !emulator_->is_title_open());
    gtk_container_add(list, row);
  }
  gtk_widget_show_all(GTK_WIDGET(list));
  std::string status =
      accounts->empty()
          ? "No profiles yet. Create one below; games need a signed-in profile "
            "for saves."
          : fmt::format("{} profile(s), {} signed in.{}", accounts->size(),
                        signed_in,
                        emulator_->is_title_open()
                            ? " Deleting is disabled while a title runs."
                            : "");
  gtk_label_set_text(GTK_LABEL(profiles_status_), status.c_str());
}

// ---- Preferences: Console tab (xconfig) ----

namespace {
// A combo over (value, label) pairs bound to a numeric field of the edited
// copy; refreshers re-read the copy after Reset.
template <typename T>
void AddConsoleCombo(GtkWidget* grid, int& row, const char* label,
                     const std::map<T, std::string>& options, xe::be<T>& field,
                     std::vector<std::function<void()>>& refreshers,
                     std::function<void(T)> on_change = nullptr) {
  gtk_grid_attach(GTK_GRID(grid), LeftLabel(label), 0, row, 1, 1);
  GtkWidget* combo = gtk_combo_box_text_new();
  std::vector<T> keys;
  for (const auto& [value, name] : options) {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), name.c_str());
    keys.push_back(value);
  }
  auto select = [combo, keys, &field]() {
    for (size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == field.get()) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), int(i));
        return;
      }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), -1);
  };
  select();
  refreshers.push_back(select);
  gtk_widget_set_hexpand(combo, TRUE);
  AttachSettingsCallback(combo, "changed", [keys, &field, on_change](GtkWidget* w) {
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    if (index >= 0 && index < int(keys.size())) {
      field = keys[index];
      if (on_change) {
        on_change(keys[index]);
      }
    }
  });
  gtk_grid_attach(GTK_GRID(grid), combo, 1, row++, 1, 1);
}

template <typename T>
void AddConsoleFlag(GtkWidget* grid, int& row, const char* label,
                    xe::be<T>& field, T bit,
                    std::vector<std::function<void()>>& refreshers) {
  GtkWidget* check = gtk_check_button_new_with_label(label);
  auto sync = [check, &field, bit]() {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
                                 (field.get() & bit) != 0);
  };
  sync();
  refreshers.push_back(sync);
  AttachSettingsCallback(check, "toggled", [&field, bit](GtkWidget* w) {
    bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    field = on ? T(field.get() | bit) : T(field.get() & ~bit);
  });
  gtk_grid_attach(GTK_GRID(grid), check, 0, row++, 2, 1);
}
}  // namespace

void EmulatorWindow::BuildConsoleTab(void* notebook_ptr) {
  auto* notebook = static_cast<GtkWidget*>(notebook_ptr);
  auto* xconfig = emulator_->kernel_state() ? emulator_->kernel_state()->xconfig()
                                            : nullptr;
  GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
  if (!xconfig) {
    gtk_box_pack_start(GTK_BOX(outer), LeftLabel("No console configuration."),
                       FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), outer,
                             gtk_label_new("Console"));
    return;
  }
  console_data_ = std::make_unique<kernel::XConfigData>(*xconfig->GetXConfig());
  console_refreshers_.clear();
  auto& data = *console_data_;
  auto& refreshers = console_refreshers_;

  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroller, TRUE);
  GtkWidget* sections = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_container_add(GTK_CONTAINER(scroller), sections);
  GtkWidget* grid = nullptr;
  gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);
  int row = 0;

  grid = NewSection(sections, "User", true);
  row = 0;
  AddConsoleFlag<uint32_t>(grid, row, "Disable daylight-saving time",
                           data.user.retail_flags,
                           uint32_t(kernel::X_RETAIL_FLAGS::DSTOff), refreshers);
  AddConsoleFlag<uint32_t>(
      grid, row, "24-hour clock", data.user.retail_flags,
      uint32_t(kernel::X_RETAIL_FLAGS::TwentyFourHourClock), refreshers);
  static const std::map<uint32_t, std::string> kLanguages = {
      {1, "English"},    {2, "Japanese"},  {3, "German"},
      {4, "French"},     {5, "Spanish"},   {6, "Italian"},
      {7, "Korean"},     {8, "Traditional Chinese"},
      {9, "Portuguese"}, {11, "Polish"},   {12, "Russian"},
      {13, "Swedish"},   {14, "Turkish"},  {15, "Norwegian"},
      {16, "Dutch"},     {17, "Simplified Chinese"}};
  AddConsoleCombo<uint32_t>(grid, row, "Language", kLanguages,
                            data.user.language, refreshers);
  static const std::map<uint8_t, std::string> kCountries = {
      {1, "AE"},  {2, "AL"},  {3, "AM"},  {4, "AR"},  {5, "AT"},  {6, "AU"},
      {7, "AZ"},  {8, "BE"},  {9, "BG"},  {10, "BH"}, {11, "BN"}, {12, "BO"},
      {13, "BR"}, {14, "BY"}, {15, "BZ"}, {16, "CA"}, {18, "CH"}, {19, "CL"},
      {20, "CN"}, {21, "CO"}, {22, "CR"}, {23, "CZ"}, {24, "DE"}, {25, "DK"},
      {26, "DO"}, {27, "DZ"}, {28, "EC"}, {29, "EE"}, {30, "EG"}, {31, "ES"},
      {32, "FI"}, {33, "FO"}, {34, "FR"}, {35, "GB"}, {36, "GE"}, {37, "GR"},
      {38, "GT"}, {39, "HK"}, {40, "HN"}, {41, "HR"}, {42, "HU"}, {43, "ID"},
      {44, "IE"}, {45, "IL"}, {46, "IN"}, {47, "IQ"}, {48, "IR"}, {49, "IS"},
      {50, "IT"}, {51, "JM"}, {52, "JO"}, {53, "JP"}, {54, "KE"}, {55, "KG"},
      {56, "KR"}, {57, "KW"}, {58, "KZ"}, {59, "LB"}, {60, "LI"}, {61, "LT"},
      {62, "LU"}, {63, "LV"}, {64, "LY"}, {65, "MA"}, {66, "MC"}, {67, "MK"},
      {68, "MN"}, {69, "MO"}, {70, "MV"}, {71, "MX"}, {72, "MY"}, {73, "NI"},
      {74, "NL"}, {75, "NO"}, {76, "NZ"}, {77, "OM"}, {78, "PA"}, {79, "PE"},
      {80, "PH"}, {81, "PK"}, {82, "PL"}, {83, "PR"}, {84, "PT"}, {85, "PY"},
      {86, "QA"}, {87, "RO"}, {88, "RU"}, {89, "SA"}, {90, "SE"}, {91, "SG"},
      {92, "SI"}, {93, "SK"}, {95, "SV"}, {96, "SY"}, {97, "TH"}, {98, "TN"},
      {99, "TR"}, {100, "TT"}, {101, "TW"}, {102, "UA"}, {103, "US"},
      {104, "UY"}, {105, "UZ"}, {106, "VE"}, {107, "VN"}, {108, "YE"},
      {109, "ZA"}, {110, "ZW"}};
  AddConsoleCombo<uint8_t>(grid, row, "Country", kCountries, data.user.country,
                           refreshers);
  {
    std::map<uint64_t, std::string> profiles = {{0, "(none)"}};
    if (auto* pm = emulator_->kernel_state()->xam_state()->profile_manager()) {
      for (const auto& [xuid, account] : *pm->GetAccounts()) {
        profiles[xuid] = account.GetGamertagString();
      }
    }
    static std::map<uint64_t, std::string> profile_options;
    profile_options = profiles;
    AddConsoleCombo<uint64_t>(grid, row, "Default profile", profile_options,
                              data.user.default_profile, refreshers);
  }
  AddConsoleFlag<uint8_t>(grid, row, "Parental control",
                          data.user.parental_control_flags,
                          uint8_t(kernel::X_PC_FLAGS::PCEnabled), refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Dashboard initialized",
                           data.user.retail_flags,
                           uint32_t(kernel::X_RETAIL_FLAGS::DashboardInitialized),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "IPTV initialized", data.user.retail_flags,
                           uint32_t(kernel::X_RETAIL_FLAGS::IPTVEnabled),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "DVR initialized", data.user.retail_flags,
                           uint32_t(kernel::X_RETAIL_FLAGS::IPTVDVREnabled),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Kinect initialized",
                           data.user.retail_flags,
                           uint32_t(kernel::X_RETAIL_FLAGS::KinectInitialized),
                           refreshers);

  grid = NewSection(sections, "System: video", false);
  row = 0;
  static const std::map<uint32_t, std::string> kAvRegions = {
      {0x00400100, "NTSC"},
      {0x00400200, "NTSC-J"},
      {0x00400400, "PAL"},
      {0x00800300, "PAL 50Hz"}};
  AddConsoleCombo<uint32_t>(grid, row, "AV region", kAvRegions,
                            data.secured.av_region, refreshers);
  {
    std::map<int32_t, std::string> resolutions;
    for (const auto& r : kernel::XVGAResolution) {
      resolutions[int32_t(r.to_host())] = r.name_;
    }
    static std::map<int32_t, std::string> resolution_options;
    resolution_options = resolutions;
    AddConsoleCombo<int32_t>(
        grid, row, "Resolution", resolution_options, data.user.av_pack_hdmi_sz,
        refreshers, [&data](int32_t value) {
          // Widescreen follows the picked resolution, as the ImGui panel does.
          bool widescreen = kernel::Resolution(uint32_t(value)).is_widescreen();
          uint32_t flag = uint32_t(kernel::X_VIDEO_FLAGS::Widescreen);
          data.user.video_flags = widescreen ? (data.user.video_flags.get() | flag)
                                             : (data.user.video_flags.get() & ~flag);
        });
  }
  AddConsoleFlag<uint32_t>(grid, row, "Widescreen", data.user.video_flags,
                           uint32_t(kernel::X_VIDEO_FLAGS::Widescreen), refreshers);

  grid = NewSection(sections, "System: audio", false);
  row = 0;
  AddConsoleFlag<uint32_t>(grid, row, "Mono", data.user.audio_flags,
                           uint32_t(kernel::X_AUDIO_FLAGS::AnalogMono), refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Dolby Pro Logic", data.user.audio_flags,
                           uint32_t(kernel::X_AUDIO_FLAGS::DolbyProLogic),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Dolby Digital", data.user.audio_flags,
                           uint32_t(kernel::X_AUDIO_FLAGS::DolbyDigital),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Dolby Digital with WMA Pro",
                           data.user.audio_flags,
                           uint32_t(kernel::X_AUDIO_FLAGS::DolbyDigitalWithWMAPRO),
                           refreshers);
  AddConsoleFlag<uint32_t>(grid, row, "Low latency (unsupported)",
                           data.user.audio_flags,
                           uint32_t(kernel::X_AUDIO_FLAGS::LowLatency), refreshers);
  {
    gtk_grid_attach(GTK_GRID(grid), LeftLabel("Music player volume (%)"), 0, row,
                    1, 1);
    GtkWidget* spin = gtk_spin_button_new_with_range(0, 100, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                              double(data.user.music_volume.get()) * 100.0);
    gtk_widget_set_halign(spin, GTK_ALIGN_START);
    refreshers.push_back([spin, &data]() {
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                                double(data.user.music_volume.get()) * 100.0);
    });
    AttachSettingsCallback(spin, "value-changed", [&data](GtkWidget* w) {
      data.user.music_volume =
          float(gtk_spin_button_get_value(GTK_SPIN_BUTTON(w)) / 100.0);
    });
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row++, 1, 1);
  }

  // Save / Reset, like the ImGui panel: only while no title runs.
  GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* save = gtk_button_new_with_label("Save to the console configuration");
  GtkWidget* reset = gtk_button_new_with_label("Reset to defaults");
  GtkWidget* status = LeftLabel(
      emulator_->is_title_open()
          ? "Changes apply to the next launch; Save and Reset are disabled "
            "while a title runs (the emulated console reads them at start)."
          : "Changes are written by Save; they apply at the next launch.");
  gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
  gtk_widget_set_sensitive(save, !emulator_->is_title_open());
  gtk_widget_set_sensitive(reset, !emulator_->is_title_open());
  AttachSettingsCallback(save, "clicked", [this, xconfig, status](GtkWidget*) {
    xconfig->WriteXConfig(console_data_.get());
    gtk_label_set_text(GTK_LABEL(status), "Saved.");
    XELOGI("Console settings: saved from the Preferences window");
  });
  AttachSettingsCallback(reset, "clicked", [this, xconfig, status](GtkWidget*) {
    xconfig->SetDefaults();
    *console_data_ = *xconfig->GetXConfig();
    for (auto& refresh : console_refreshers_) {
      refresh();
    }
    gtk_label_set_text(GTK_LABEL(status), "Defaults restored (and saved).");
  });
  gtk_box_pack_start(GTK_BOX(buttons), save, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(outer), buttons, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(outer), status, FALSE, FALSE, 0);
  console_status_ = status;
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), outer,
                           gtk_label_new("Console"));
}

}  // namespace app
}  // namespace xe
#endif  // XE_PLATFORM_LINUX
