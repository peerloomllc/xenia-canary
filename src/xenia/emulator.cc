/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <cstdio>
#include <ranges>
#include <thread>

#include <lz4.h>

#include "xenia/emulator.h"

#include "config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tabulate/single_include/tabulate/tabulate.hpp"
#include "third_party/zarchive/include/zarchive/zarchivecommon.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"
#include "third_party/zarchive/src/sha_256.h"
#include "xenia/apu/audio_system.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xma_context.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_LINUX
#include <pthread.h>
#endif
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/memory.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/device.h"
#include "xenia/kernel/smc.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

DEFINE_int32(stats_log_seconds, 0,
             "Diagnostic: every N seconds log frames presented, audio frames "
             "submitted, XMA packets decoded and SDL audio callbacks (with "
             "how many went out silent). 0 = off.",
             "General");
DEFINE_int32(
    savestate_experiment_save_seconds, 0,
    "Experiment: N seconds after launch, call Emulator::SaveToFile on the "
    "path in --savestate_experiment_path from a host thread.",
    "General");
DEFINE_int32(
    savestate_experiment_restore_seconds, 0,
    "Experiment: N seconds after launch, call Emulator::RestoreFromFile on "
    "the same path. Must be later than the save.",
    "General");
DEFINE_string(savestate_experiment_path, "xenia_experiment.sav",
              "Experiment: save state path.", "General");
DEFINE_int32(savestate_experiment_cycles, 1,
             "Experiment: repeat the save+restore sequence this many times, "
             "20 s apart.", "General");
DEFINE_int32(savestate_experiment_restore_repeat, 1,
             "Experiment: how many times to restore, 20 s apart.", "General");
DEFINE_string(savestate_experiment_preload_path, "",
              "Experiment: a state to restore at "
              "--savestate_experiment_preload_seconds, before the save/restore "
              "cycle; gets the game somewhere a title-screen timer cannot.",
              "General");
DEFINE_int32(savestate_experiment_preload_seconds, 0,
             "Experiment: when to restore --savestate_experiment_preload_path.",
             "General");
DEFINE_bool(savestate_experiment_restore_only, false,
            "Experiment: skip the save; only restore "
            "--savestate_experiment_path at "
            "--savestate_experiment_restore_seconds.",
            "General");
DEFINE_bool(savestate_experiment_step_only, false,
            "Experiment: at --savestate_experiment_save_seconds, only Pause, "
            "step every guest thread to a safe point and Resume; write no "
            "file. The smallest reproduction of a save disturbing the game.",
            "General");
DEFINE_bool(pause_rewinds_guest_clock, true,
            "Resume() sets the guest clock back to where it was at Pause(), so "
            "the game sees no time pass while paused (a paused cutscene "
            "otherwise skips ahead on resume and its dialogue lines overlap).",
            "General");
DEFINE_int32(pause_experiment_pause_seconds, 0,
             "Experiment: N seconds after launch, call Emulator::Pause() "
             "from a host thread (what the pause hotkey does).",
             "General");
DEFINE_int32(pause_experiment_resume_seconds, 0,
             "Experiment: N seconds after launch, call Emulator::Resume().",
             "General");
DEFINE_int32(disc_swap_experiment_seconds, 0,
             "Experiment: N seconds after launch, swap the disc the way "
             "XamSwapDisc does, without waiting for the title to ask. Uses "
             "the playlist when one was launched, otherwise "
             "disc_swap_experiment_path.",
             "General");
DEFINE_int32(disc_swap_experiment_disc, 2,
             "Experiment: which disc number disc_swap_experiment_seconds "
             "asks for.",
             "General");
DEFINE_path(disc_swap_experiment_path, "",
            "Experiment: the disc image to swap to when there is no "
            "playlist.",
            "General");
DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");

DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_bool(allow_game_relative_writes, false,
            "Not useful to non-developers. Allows code to write to paths "
            "relative to game://. Used for "
            "generating test data to compare with original hardware. ",
            "General");

DECLARE_bool(allow_plugins);

DEFINE_int32(priority_class, 0,
             "Forces Xenia to use different process priority than default one. "
             "It might affect performance and cause unexpected bugs. Possible "
             "values: 0 - Normal, 1 - Above normal, 2 - High",
             "General");

DECLARE_int32(console_type);

namespace xe {
using namespace xe::literals;

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      audio_media_player_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      game_info_database_(),
      paused_(false),
      restoring_(false),
      restore_fence_() {
  if (cvars::priority_class != 0) {
    if (SetProcessPriorityClass(cvars::priority_class)) {
      XELOGI("Higher priority class request: Successful. New priority: {}",
             cvars::priority_class);
    }
  }

#if XE_PLATFORM_WIN32 == 1
  // Show a disclaimer that links to the quickstart
  // guide the first time they ever open the emulator
  uint64_t persistent_flags = GetPersistentEmulatorFlags();
  if (!(persistent_flags & EmulatorFlagDisclaimerAcknowledged)) {
    if ((MessageBoxW(
             nullptr,
             L"DISCLAIMER: Xenia is not for enabling illegal activity, and "
             "support is unavailable for illegally obtained software.\n\n"
             "Please respect this policy as no further reminders will be "
             "given.\n\nThe quickstart guide explains how to use digital or "
             "physical games from your Xbox 360 console.\n\nWould you like "
             "to open it?",
             L"Xenia", MB_YESNO | MB_ICONQUESTION) == IDYES)) {
      LaunchWebBrowser(
          "https://github.com/xenia-canary/xenia-canary/wiki/"
          "Quickstart#how-to-rip-games");
    }
    SetPersistentEmulatorFlags(persistent_flags |
                               EmulatorFlagDisclaimerAcknowledged);
  }
#endif
}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();
  audio_media_player_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  XELOGI("{}: Initializing Memory...", __func__);
  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    XELOGE("{}: Cannot initalize memory!", __func__);
    return result;
  }

  XELOGI("{}: Initializing Exports...", __func__);
  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#elif XE_ARCH_ARM64
  if (cvars::cpu == "a64") {
    backend.reset(new xe::cpu::backend::a64::A64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
      backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  XELOGI("{}: Initializing Processor...", __func__);
  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  if (!processor_->Setup(std::move(backend))) {
    XELOGE("{}: Cannot initalize processor!", __func__);
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("{}: Initializing Audio...", __func__);
  // Initialize the APU.
  if (audio_system_factory) {
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      XELOGE("{}: Cannot initalize audio_system!", __func__);
      return X_STATUS_NOT_IMPLEMENTED;
    }
  }

  XELOGI("{}: Initializing Graphics...", __func__);
  // Initialize the GPU.
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    XELOGE("{}: Cannot initalize graphics_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }

  XELOGI("{}: Initializing HID...", __func__);
  // Initialize the HID.
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    XELOGE("{}: Cannot initalize input_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      input_system_->AddDriver(std::move(input_drivers[i]));
    }
  }

  result = input_system_->Setup();
  if (result) {
    return result;
  }

  // Add inputSystem to UI
  imgui_drawer_->LoadInputSystem(input_system_.get());

  XELOGI("{}: Initializing VFS...", __func__);
  // Bring up the virtual filesystem used by the kernel.
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  patcher_ = std::make_unique<xe::patcher::Patcher>(storage_root_);

  XELOGI("{}: Initializing Kernel...", __func__);
  // Shared kernel state.
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);
#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  LOAD_KERNEL_MODULE(xam::XamModule);

  // 415608C3 anti-cheat checks if XDBM is loaded.
  if (cvars::console_type >= 0) {
    LOAD_KERNEL_MODULE(xbdm::XbdmModule);
  }
#undef LOAD_KERNEL_MODULE
  plugin_loader_ = std::make_unique<xe::patcher::PluginLoader>(
      kernel_state_.get(), storage_root() / "plugins");

  XELOGI("{}: Starting graphics_system...", __func__);
  // Setup the core components.
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  if (result) {
    XELOGE("{}: Failed to setup graphics_system!", __func__);
    return result;
  }

  if (audio_system_) {
    XELOGI("{}: Starting audio_system...", __func__);
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      XELOGE("{}: Failed to setup audio_system!", __func__);
      return result;
    }
    audio_media_player_ = std::make_unique<apu::AudioMediaPlayer>(
        audio_system_.get(), kernel_state_.get());
    audio_media_player_->Setup();
  }

  // Initialize emulator fallback exception handling last.
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);

  return result;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_module_hash_.reset();
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

const std::unique_ptr<vfs::Device> Emulator::CreateVfsDevice(
    const std::filesystem::path& path, const std::string_view mount_path) {
  // Must check if the type has changed e.g. XamSwapDisc
  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      auto parent_path = path.parent_path();
      return std::make_unique<vfs::HostPathDevice>(
          mount_path, parent_path, !cvars::allow_game_relative_writes);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      return kernel_state_->content_manager()->MountPackageUnregistered(
          mount_path, kernel_state_->content_manager()->OpenPackage(path));
    } break;
    case FileSignatureType::XISO: {
      return std::make_unique<vfs::DiscImageDevice>(mount_path, path);
    } break;
    case FileSignatureType::ZAR: {
      return std::make_unique<vfs::DiscZarchiveDevice>(mount_path, path);
    } break;
    case FileSignatureType::XBE:
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return nullptr;
      break;
  }
}

uint64_t Emulator::GetPersistentEmulatorFlags() {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = 0;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    // let the Set function create the key and initialize it to 0
    SetPersistentEmulatorFlags(0ULL);
    return 0ULL;
  }

  lstat = RegQueryValueExA(xenia_hkey, "XEFLAGS", 0, NULL,
                           reinterpret_cast<LPBYTE>(&value), &value_size);
  RegCloseKey(xenia_hkey);
  if (lstat) {
    return 0ULL;
  }
  return value;
#else
  return EmulatorFlagDisclaimerAcknowledged;
#endif
}
void Emulator::SetPersistentEmulatorFlags(uint64_t new_flags) {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = new_flags;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    lstat = RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  }

  lstat = RegSetValueExA(xenia_hkey, "XEFLAGS", 0, REG_QWORD,
                         reinterpret_cast<const BYTE*>(&value), 8);
  RegFlushKey(xenia_hkey);
  RegCloseKey(xenia_hkey);
#endif
}

X_STATUS Emulator::MountPath(const std::filesystem::path& path,
                             const std::string_view mount_path) {
  auto device = CreateVfsDevice(path, mount_path);
  if (!device || !device->Initialize()) {
    XELOGE(
        "Unable to mount the selected file, it is an unsupported format or "
        "corrupted.");
    return X_STATUS_NO_SUCH_FILE;
  }

  const std::string mpath = std::string(device->mount_path());

  if (!file_system_->RegisterDevice(std::move(device))) {
    XELOGE("Unable to register the input file to {}.", mount_path);
    return X_STATUS_NO_SUCH_FILE;
  }

  file_system_->UnregisterSymbolicLink(kDefaultPartitionSymbolicLink);
  file_system_->UnregisterSymbolicLink(kDefaultGameSymbolicLink);
  file_system_->UnregisterSymbolicLink("plugins:");

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink(kDefaultGameSymbolicLink, mpath);
  file_system_->RegisterSymbolicLink(kDefaultPartitionSymbolicLink, mpath);

  return X_STATUS_SUCCESS;
}

Emulator::FileSignatureType Emulator::GetFileSignature(
    const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");

  if (!file) {
    return FileSignatureType::Unknown;
  }

  const uint64_t file_size = std::filesystem::file_size(path);
  constexpr int64_t header_size = 4;

  if (file_size < header_size) {
    return FileSignatureType::Unknown;
  }

  char file_magic[header_size];
  fread(file_magic, sizeof(file_magic), 1, file);

  fourcc_t magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  fclose(file);

  switch (magic_value) {
    case xe::cpu::kXEX0Signature:
      return FileSignatureType::XEX0;
    case xe::cpu::kXEXQSignature:
      return FileSignatureType::XEXQ;
    case xe::cpu::kXEXHSignature:
      return FileSignatureType::XEXH;
    case xe::cpu::kXEX25Signature:
      return FileSignatureType::XEX25;
    case xe::cpu::kXEX1Signature:
      return FileSignatureType::XEX1;
    case xe::cpu::kXEX2Signature:
      return FileSignatureType::XEX2;
    case xe::vfs::kCONSignature:
      return FileSignatureType::CON;
    case xe::vfs::kLIVESignature:
      return FileSignatureType::LIVE;
    case xe::vfs::kPIRSSignature:
      return FileSignatureType::PIRS;
    case xe::vfs::kXSFSignature:
      return FileSignatureType::XISO;
    case xe::cpu::kXBESignature:
      return FileSignatureType::XBE;
    case xe::cpu::kElfSignature:
      return FileSignatureType::ELF;
    default:
      break;
  }

  magic_value = make_fourcc(file_magic[0], file_magic[1], 0, 0);

  if (xe::kernel::kEXESignature == magic_value) {
    return FileSignatureType::EXE;
  }

  file = xe::filesystem::OpenFile(path, "rb");
  xe::filesystem::Seek(file, -header_size, SEEK_END);
  fread(file_magic, 1, header_size, file);
  fclose(file);

  magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  if (xe::vfs::kZarMagic == magic_value) {
    return FileSignatureType::ZAR;
  }

  // Check if XISO
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscImageDevice>("", path);

  XELOGI("Checking for XISO");

  if (device->Initialize()) {
    return FileSignatureType::XISO;
  }

  XELOGE("{}: {} ({:08X})", __func__, path.extension(), magic_value);
  return FileSignatureType::Unknown;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  X_STATUS mount_result = X_STATUS_SUCCESS;

  // An .m3u is a plain list of the title's discs, one per line, so a
  // multi-disc title can be opened once and swap discs by itself. Blank
  // lines and # comments are skipped; relative entries resolve against the
  // playlist's own folder.
  if (path.extension() == ".m3u" || path.extension() == ".M3U") {
    disc_playlist_.clear();
    std::ifstream list(path);
    if (!list) {
      XELOGE("Unable to read the playlist {}", path.string());
      return X_STATUS_NO_SUCH_FILE;
    }
    std::string line;
    while (std::getline(list, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      size_t first = line.find_first_not_of(" \t");
      if (first == std::string::npos) {
        continue;
      }
      line = line.substr(first);
      if (line[0] == '#') {
        continue;
      }
      std::filesystem::path entry = xe::to_path(line);
      if (entry.is_relative()) {
        entry = path.parent_path() / entry;
      }
      std::error_code ec;
      if (!std::filesystem::exists(entry, ec)) {
        XELOGW("Playlist entry not found, skipping: {}", entry.string());
        continue;
      }
      disc_playlist_.push_back(entry);
    }
    if (disc_playlist_.empty()) {
      XELOGE("The playlist {} lists no readable discs", path.string());
      return X_STATUS_NO_SUCH_FILE;
    }
    XELOGI("Playlist {}: {} disc(s), starting with {}", path.string(),
           disc_playlist_.size(), disc_playlist_.front().filename().string());
    // The entries are not opened here: each one is read when a swap actually
    // asks for it. (Reading them all at launch once looked like it caused a
    // startup abort; that was an unlocked table in the Vulkan pipeline cache,
    // fixed since, and the lazy read is simply cheaper.)
    return LaunchPath(disc_playlist_.front());
  }

  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      mount_result = MountPath(path, "\\Device\\Harddisk0\\Partition1");
      return mount_result ? mount_result : LaunchXexFile(path);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      mount_result = MountPath(path, "\\Device\\Package_0");
      return mount_result ? mount_result : LaunchStfsContainer(path);
    } break;
    case FileSignatureType::XISO: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      disc_mount_path_ = "\\Device\\Cdrom0";
      return mount_result ? mount_result : LaunchDiscImage(path);
    } break;
    case FileSignatureType::XBE: {
      XELOGE("OG Xbox games are not supported");
      return X_STATUS_NOT_SUPPORTED;
    } break;
    case FileSignatureType::ZAR: {
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      disc_mount_path_ = "\\Device\\Cdrom0";
      return mount_result ? mount_result : LaunchDiscArchive(path);
    } break;
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return X_STATUS_NOT_SUPPORTED;
      break;
  }
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex
  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = fmt::format("{}\\", kDefaultGameSymbolicLink) +
                 xe::path_to_utf8(file_name);
  X_STATUS result = CompleteLaunch(path, fs_path);

  if (XFAILED(result)) {
    return result;
  }

  kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;

  if (!kernel::IsSystemTitle(kernel_state_->title_id())) {
    return result;
  }

  const std::string mount_path =
      utf8::find_base_guest_path(kernel_state_->GetExecutableModule()->path());

  // System related symlinks. This should point to dashboard location in the
  // future.
  file_system_->RegisterSymbolicLink("\\SystemRoot", mount_path);

  auto module = kernel_state_->LoadUserModule("xam.xex");

  if (!module) {
    module = kernel_state_->LoadUserModule("$flash_xam.xex");
  }

  if (module) {
    result = kernel_state_->FinishLoadingUserModule(module, false);
  }

  return result;
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchDiscArchive(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kDownload;
  return result;
}

X_STATUS Emulator::LaunchDefaultModule(const std::filesystem::path& path) {
  cvars::launch_module = "";
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (XSUCCEEDED(result)) {
    kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;
    auto title_id = kernel_state_->title_id();
    if (!kernel::IsSystemTitle(title_id)) {
      // Assumption that any loaded game is loaded as a disc.
      kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
    }
  }
  return result;
}

void Emulator::set_content_root(const std::filesystem::path& content_root) {
  content_root_ = content_root;
  if (kernel_state_ && kernel_state_->content_manager()) {
    kernel_state_->content_manager()->set_root_path(content_root);
  }
  XELOGI("Content root: {}", content_root_);
}

X_STATUS Emulator::DataMigration(const uint64_t xuid) {
  uint32_t failure_count = 0;
  const std::string xuid_string = fmt::format("{:016X}", xuid);
  const std::string common_xuid_string = fmt::format("{:016X}", 0);
  const std::filesystem::path path_to_profile_data =
      content_root_ / xuid_string / "FFFE07D1" / "00010000" / xuid_string;
  // Filter directories inside. First we need to find any content type
  // directories.
  // Savefiles must go to user specific directory
  // Everything else goes to common
  const auto titles_to_move = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root_),
      std::regex("[A-Fa-f0-9]{8}"));

  for (const auto& title : titles_to_move) {
    if (xe::path_to_utf8(title.name) == "FFFE07D1" ||
        xe::path_to_utf8(title.name) == "00000000") {
      // SKip any dashboard/profile related data that was previously installed
      continue;
    }

    const auto content_type_dirs = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(title.path / title.name),
        std::regex("[A-Fa-f0-9]{8}"));

    for (const auto& content_type : content_type_dirs) {
      const std::string used_xuid =
          xe::path_to_utf8(content_type.name) == "00000001"
              ? xuid_string
              : common_xuid_string;

      const auto previous_path = content_root_ / title.name / content_type.name;
      const auto path = content_root_ / used_xuid / title.name;

      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
      }

      std::error_code ec;
      std::filesystem::rename(previous_path, path / content_type.name, ec);

      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, previous_path, path / content_type.name, ec.message(),
               ec.value());
      }
    }
    // Other directories:
    // Headers - Just copy everything to both common and xuid locations
    // profile - ?
    if (std::filesystem::exists(title.path / title.name / "Headers")) {
      const auto xuid_path =
          content_root_ / xuid_string / title.name / "Headers";

      std::filesystem::create_directories(xuid_path);

      std::error_code ec;
      // Copy to specific user
      std::filesystem::copy(title.path / title.name / "Headers", xuid_path,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, title.path / title.name / "Headers", xuid_path,
               ec.message(), ec.value());
      }

      const auto header_types =
          xe::filesystem::ListDirectories(title.path / title.name / "Headers");

      if (!(header_types.size() == 1 &&
            header_types.at(0).name == "00000001")) {
        const auto common_path =
            content_root_ / common_xuid_string / title.name / "Headers";

        std::filesystem::create_directories(common_path);

        // Copy to common, skip cases where only savefile header is available
        std::filesystem::copy(title.path / title.name / "Headers", common_path,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
        if (ec) {
          failure_count++;
          XELOGW(
              "{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
              __func__, title.path / title.name / "Headers", common_path,
              ec.message(), ec.value());
        }
      }

      if (!ec) {
        // Remove previous directory
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "Headers", ec);
      }
    }

    if (std::filesystem::exists(title.path / title.name / "profile")) {
      // Find directory with previous username. There should be only one!
      const auto old_profile_data =
          xe::filesystem::ListDirectories(title.path / title.name / "profile");

      xe::filesystem::FileInfo entry_to_copy = xe::filesystem::FileInfo();
      if (old_profile_data.size() != 1) {
        for (const auto& entry : old_profile_data) {
          if (entry.name == "User") {
            entry_to_copy = entry;
          }
        }
      } else {
        entry_to_copy = old_profile_data.front();
      }

      const auto path_from =
          title.path / title.name / "profile" / entry_to_copy.name;
      std::error_code ec;
      // Move files from inside to outside for convenience
      std::filesystem::rename(path_from, path_to_profile_data / title.name, ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, path_from, path_to_profile_data / title.name,
               ec.message(), ec.value());
      } else {
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "profile", ec);
      }
    }

    const auto remaining_file_list =
        xe::filesystem::ListDirectories(title.path / title.name);

    if (remaining_file_list.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(title.path / title.name, ec);
    }
  }

  std::string migration_status_message =
      fmt::format("Migration finished with {} {}.", failure_count,
                  failure_count == 1 ? "error" : "errors");

  if (failure_count) {
    migration_status_message.append(
        " For more information check xenia.log file.");
  }
  new xe::ui::HostNotificationWindow(imgui_drawer_, "Migration Status",
                                     migration_status_message, 0);
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::ProcessContentPackageHeader(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.name_ = "Invalid Content Package!";
  installation_info.content_type_ = XContentType::kInvalid;
  installation_info.data_installation_path_ = xe::path_to_utf8(path.filename());

  auto package = kernel_state_->content_manager()->OpenPackage(path);
  if (!package) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_result_ = X_STATUS_INVALID_PARAMETER;
    installation_info.installation_error_message_ = "Cannot open package!";
    return X_STATUS_INVALID_PARAMETER;
  }

  const auto header = package->GetContainerHeader();

  if (!header || !header->content_header.is_magic_valid()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_result_ = X_STATUS_INVALID_PARAMETER;
    installation_info.installation_error_message_ = "Invalid Package Type!";
    return X_STATUS_INVALID_PARAMETER;
  }

  // Always install savefiles to user signed to slot 0.
  const auto profile =
      kernel_state_->xam_state()->profile_manager()->GetProfile(
          static_cast<uint8_t>(0));

  uint64_t xuid = header->content_metadata.profile_id;
  if (header->content_metadata.content_type == XContentType::kSavedGame &&
      profile) {
    xuid = profile->xuid();
  }

  installation_info.filename_ = path.filename();
  installation_info.data_installation_path_ =
      content_root() /
      fmt::format(
          "{:016X}/{:08X}/{:08X}/", xuid,
          header->content_metadata.execution_info.title_id.get(),
          static_cast<uint32_t>(header->content_metadata.content_type.get()));

  installation_info.header_installation_path_ =
      content_root() /
      fmt::format(
          "{:016X}/{:08X}/Headers/{:08X}/{}.header", xuid,
          header->content_metadata.execution_info.title_id.get(),
          static_cast<uint32_t>(header->content_metadata.content_type.get()),
          path.filename());

  installation_info.name_ =
      xe::to_utf8(header->content_metadata.display_name(XLanguage::kEnglish));
  installation_info.name_.append(
      fmt::format(" (filename: {})", installation_info.filename_));

  installation_info.content_type_ =
      static_cast<XContentType>(header->content_metadata.content_type);
  installation_info.content_size_ = package->GetPackageSize();
  installation_info.installation_state_ = InstallState::pending;

  installation_info.icon_ = imgui_drawer_->LoadImGuiIcon(
      std::span<const uint8_t>(header->content_metadata.title_thumbnail,
                               header->content_metadata.title_thumbnail_size));

  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::InstallContentPackage(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.installation_state_ = InstallState::preparing;

  auto package =
      kernel_state_->content_manager()->OpenAndMountPackage(path, "");

  if (!package) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ =
        "Device initialization failed!";
    installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  if (!std::filesystem::exists(content_root())) {
    const std::error_code ec = xe::filesystem::CreateFolder(content_root());
    if (ec) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ = ec.message();
      installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
      kernel_state_->content_manager()->CloseContentByDeviceName(
          package->GetDevicePath());
      return X_STATUS_ACCESS_DENIED;
    }
  }

  const auto disk_space = std::filesystem::space(content_root());
  if (disk_space.available < installation_info.content_size_ * 1.1f) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Insufficient disk space!";
    installation_info.installation_result_ = X_STATUS_DISK_FULL;
    kernel_state_->content_manager()->CloseContentByDeviceName(
        package->GetDevicePath());
    return X_STATUS_DISK_FULL;
  }

  if (std::filesystem::exists(installation_info.data_installation_path_)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code =
        xe::filesystem::CreateFolder(installation_info.data_installation_path_);
    if (error_code) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ =
          "Cannot Create Content Directory!";
      installation_info.installation_result_ = error_code.value();
      kernel_state_->content_manager()->CloseContentByDeviceName(
          package->GetDevicePath());
      return error_code.value();
    }
  }

  installation_info.installation_state_ = InstallState::installing;

  X_STATUS error_code = kernel_state_->content_manager()->InstallContentPackage(
      package,
      installation_info.data_installation_path_ / installation_info.filename_,
      installation_info.header_installation_path_,
      installation_info.currently_installed_size_);

  if (error_code != X_ERROR_SUCCESS) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ =
        "Cannot install package file!";
    installation_info.installation_result_ = error_code;
  } else {
    installation_info.installation_state_ = InstallState::installed;
    installation_info.currently_installed_size_ =
        installation_info.content_size_;
  }

  kernel_state()->BroadcastNotification(kXNotificationLiveContentInstalled, 0);

  kernel_state_->content_manager()->CloseContentByDeviceName(
      package->GetDevicePath());

  if (installation_info.content_type_ == XContentType::kProfile) {
    kernel_state_->xam_state()->profile_manager()->ReloadProfiles();
  }

  return error_code;
}

X_STATUS Emulator::ExtractContentPackage(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.installation_state_ = InstallState::preparing;

  auto package =
      kernel_state_->content_manager()->OpenAndMountPackage(path, "");

  if (!package) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Cannot mount package!";
    return X_STATUS_INVALID_PARAMETER;
  }

  const auto disk_space =
      std::filesystem::space(installation_info.data_installation_path_);
  if (disk_space.available < installation_info.content_size_ * 1.1f) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Insufficient disk space!";
    installation_info.installation_result_ = X_STATUS_DISK_FULL;
    kernel_state_->content_manager()->CloseContentByDeviceName(
        package->GetDevicePath());
    return X_STATUS_DISK_FULL;
  }

  installation_info.installation_state_ = InstallState::installing;

  X_STATUS error_code = kernel_state_->content_manager()->InstallContentPackage(
      package,
      installation_info.data_installation_path_ / installation_info.filename_,
      installation_info.header_installation_path_,
      installation_info.currently_installed_size_, true);

  installation_info.installation_result_ = error_code;
  installation_info.installation_state_ = error_code == X_STATUS_SUCCESS
                                              ? InstallState::installed
                                              : InstallState::failed;

  if (installation_info.currently_installed_size_ < package->GetPackageSize() &&
      error_code == X_STATUS_SUCCESS) {
    installation_info.currently_installed_size_ = package->GetPackageSize();
  }

  kernel_state_->content_manager()->CloseContentByDeviceName(
      package->GetDevicePath());

  return error_code;
}

X_STATUS Emulator::ExtractZarchivePackage(
    const std::filesystem::path& path,
    const std::filesystem::path& extract_dir) {
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscZarchiveDevice>("", path);
  if (!device->Initialize()) {
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  if (std::filesystem::exists(extract_dir)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(extract_dir, error_code);
    if (error_code) {
      return error_code.value();
    }
  }

  uint64_t progress = 0;
  return vfs::VirtualFileSystem::ExtractDeviceFiles(device.get(), extract_dir,
                                                    progress);
}

X_STATUS Emulator::CreateZarchivePackage(
    const std::filesystem::path& inputDirectory,
    const std::filesystem::path& outputFile) {
  std::vector<uint8_t> buffer;
  buffer.resize(64 * 1024);

  std::error_code ec;
  PackContext packContext;
  packContext.outputFilePath = outputFile;

  ZArchiveWriter zWriter(
      [](int32_t partIndex, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile =
            std::ofstream(packContext->outputFilePath, std::ios::binary);

        if (!packContext->currentOutputFile.is_open()) {
          XELOGI("Failed to create output file: {}\n",
                 packContext->outputFilePath.string());
          packContext->hasError = true;
        }
      },
      [](const void* data, size_t length, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile.write(
            reinterpret_cast<const char*>(data), length);
      },
      &packContext);

  if (packContext.hasError) {
    return X_STATUS_UNSUCCESSFUL;
  }

  for (auto const& dirEntry :
       std::filesystem::recursive_directory_iterator(inputDirectory)) {
    std::filesystem::path pathEntry =
        std::filesystem::relative(dirEntry.path(), inputDirectory, ec);

    if (ec) {
      XELOGI("Failed to get relative path {}\n", pathEntry.string());
      return X_STATUS_UNSUCCESSFUL;
    }

    if (dirEntry.is_directory()) {
      if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false)) {
        XELOGI("Failed to create directory {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }
    } else if (dirEntry.is_regular_file()) {
      // Don't pack itself to prevent infinite packing.
      if (dirEntry == outputFile) {
        continue;
      }

      XELOGI("Adding file: {}\n", pathEntry.string());

      if (!zWriter.StartNewFile(pathEntry.generic_string().c_str())) {
        XELOGI("Failed to create archive file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      std::filesystem::path file_to_pack_path = inputDirectory / pathEntry;
      FILE* file = xe::filesystem::OpenFile(file_to_pack_path, "rb");

      if (!file) {
        XELOGI("Failed to open input file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      const uint64_t file_size = std::filesystem::file_size(file_to_pack_path);
      uint64_t total_bytes_read = 0;

      while (total_bytes_read < file_size) {
        uint64_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);

        total_bytes_read += bytes_read;

        zWriter.AppendData(buffer.data(), bytes_read);
      }

      fclose(file);
    }

    if (packContext.hasError) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  zWriter.Finalize();

  return X_STATUS_SUCCESS;
}

void Emulator::Pause(bool capture_edram) {
  XELOGI("anchor xe::FlushLog at {}", reinterpret_cast<void*>(&xe::FlushLog));
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause(capture_edram);
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      uint32_t previous_count = 0;
      if (!thread->thread()->Suspend(&previous_count)) {
        // It finished between is_running() above and the suspend. Recording
        // it would leave a dead thread to be resumed later.
        XELOGW("Pause: thread {:08X} '{}' could not be suspended, skipping",
               thread->handle(), thread->name());
        continue;
      }
      if (previous_count != 0) {
        XELOGW("Pause: thread {:08X} '{}' was already suspended (count {})",
               thread->handle(), thread->name(), previous_count);
      }
      paused_threads_.push_back(thread);
    }
  }

  pause_guest_tick_count_ = Clock::QueryGuestTickCount();
  XELOGI("! EMULATOR PAUSED ! ({} guest threads suspended, guest clock {:.3f} s)",
         paused_threads_.size(),
         double(pause_guest_tick_count_) / Clock::guest_tick_frequency());
  // Listeners may post to the UI thread; never do that while holding the
  // global critical region.
  lock.unlock();
  on_pause_state_changed(true);
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  // The guest clock ran on through the pause (it is derived from the host
  // clock). Set it back, as a save-state restore does, before any guest
  // thread runs again: otherwise a cutscene's timeline jumps ahead by the
  // pause and queues its next lines over the ones still in the audio
  // buffers (Lost Odyssey, notes/42).
  const uint64_t now_ticks = Clock::QueryGuestTickCount();
  if (cvars::pause_rewinds_guest_clock && pause_guest_tick_count_ &&
      now_ticks > pause_guest_tick_count_) {
    Clock::SetGuestTickCount(pause_guest_tick_count_);
    kernel_state_->UpdateKeTimestampBundle();
  }
  XELOGI("! EMULATOR RESUMING ! guest clock {:.3f} s at pause, {:.3f} s now, {}",
         double(pause_guest_tick_count_) / Clock::guest_tick_frequency(),
         double(now_ticks) / Clock::guest_tick_frequency(),
         cvars::pause_rewinds_guest_clock ? "set back" : "left running");
  pause_guest_tick_count_ = 0;

  graphics_system_->Resume();
  audio_system_->Resume();

  // Resume exactly the threads Pause() suspended. The previous version
  // resumed every thread that was *not* running (exited ones) and left the
  // suspended ones suspended, so the title never came back.
  for (auto& thread : paused_threads_) {
    thread->thread()->Resume(nullptr);
  }
  XELOGI("! EMULATOR RESUMED ! ({} guest threads resumed)",
         paused_threads_.size());
  paused_threads_.clear();
  on_pause_state_changed(false);
}

bool Emulator::StepAllGuestThreads() {
  Pause();
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  size_t stepped = 0, failed = 0;
  for (auto thread : threads) {
    if (!thread->is_guest_thread() || !thread->is_running()) {
      continue;
    }
    uint32_t pc = processor_->StepToGuestSafePoint(thread->thread_id());
    XELOGI("STEP EXPERIMENT: thread {:08X} id={} '{}' -> pc {:08X}",
           thread->handle(), thread->thread_id(), thread->name(), pc);
    if (pc) {
      ++stepped;
    } else {
      ++failed;
    }
  }
  XELOGI("STEP EXPERIMENT: {} stepped, {} failed", stepped, failed);
  Resume();
  return failed == 0;
}

namespace {

// Format 2 container: this header, then chunk_count chunks of the serialised
// state, each preceded by a SaveStateChunkHeader. A chunk whose stored_size
// equals its raw_size is stored as-is (LZ4 could not shrink it).
struct SaveStateContainerHeader {
  uint32_t signature;  // kSaveStateContainerSignature
  uint32_t version;    // kSaveStateFormatVersion of the writer
  uint32_t flags;      // kSaveStateFlagLz4
  uint32_t chunk_size;
  uint64_t raw_size;  // serialised state size
  uint32_t chunk_count;
  uint32_t has_title_id;
  uint32_t title_id;
  uint32_t reserved;
  // Format 3 and later; a format 2 header ends above.
  uint32_t media_id;
  uint8_t disc_number;
  uint8_t disc_count;
  uint16_t reserved2;
  // Format 6 and later: the guest clock at the save.
  uint64_t guest_tick_count;
  uint64_t guest_system_time;
};
constexpr size_t kSaveStateHeaderSizeV2 = 40;
constexpr size_t kSaveStateHeaderSizeV3 = 48;
static_assert(sizeof(SaveStateContainerHeader) == 64);

size_t SaveStateHeaderSize(uint32_t version) {
  if (version >= 6) return sizeof(SaveStateContainerHeader);
  if (version >= 3) return kSaveStateHeaderSizeV3;
  return kSaveStateHeaderSizeV2;
}

// Reads the container header (or the legacy stream's start). The file is
// left positioned after the header. Returns false if it is not a save state.
bool ReadSaveStateHeader(FILE* file, SaveStateContainerHeader* header,
                         uint32_t* out_legacy_title_id,
                         bool* out_legacy_has_title_id) {
  uint32_t signature = 0;
  xe::filesystem::Seek(file, 0, SEEK_SET);
  if (fread(&signature, sizeof(signature), 1, file) != 1) {
    return false;
  }
  *header = {};
  if (signature == kEmulatorSaveSignature) {
    // Legacy: signature, bool has_title_id, u32 title_id.
    uint8_t has_title_id = 0;
    uint32_t title_id = 0;
    if (fread(&has_title_id, 1, 1, file) != 1 ||
        (has_title_id && fread(&title_id, 4, 1, file) != 1)) {
      return false;
    }
    header->signature = signature;
    header->version = 1;
    header->has_title_id = has_title_id;
    header->title_id = has_title_id ? title_id : 0;
    if (out_legacy_title_id) *out_legacy_title_id = title_id;
    if (out_legacy_has_title_id) *out_legacy_has_title_id = has_title_id;
    return true;
  }
  if (signature != kSaveStateContainerSignature) {
    return false;
  }
  xe::filesystem::Seek(file, 0, SEEK_SET);
  if (fread(header, kSaveStateHeaderSizeV2, 1, file) != 1) {
    return false;
  }
  if (header->version >= 3) {
    size_t rest = SaveStateHeaderSize(header->version) - kSaveStateHeaderSizeV2;
    if (fread(reinterpret_cast<uint8_t*>(header) + kSaveStateHeaderSizeV2,
              rest, 1, file) != 1) {
      return false;
    }
  }
  return true;
}

struct SaveStateChunkHeader {
  uint32_t stored_size;
  uint32_t raw_size;
};

constexpr uint32_t kSaveStateFlagLz4 = 1u << 0;
constexpr uint32_t kSaveStateChunkSize = uint32_t(4_MiB);

int64_t ElapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - since)
      .count();
}

}  // namespace

uint8_t* Emulator::AcquireStateBuffer() {
  if (!state_buffer_) {
    for (size_t size : {size_t(4_GiB), size_t(2_GiB)}) {
      state_buffer_ = reinterpret_cast<uint8_t*>(memory::AllocFixed(
          nullptr, size, memory::AllocationType::kReserveCommit,
          memory::PageAccess::kReadWrite));
      if (state_buffer_) {
        state_buffer_size_ = size;
        break;
      }
    }
  }
  return state_buffer_;
}

namespace {
// Logs the saving thread's whereabouts every 10 s until destroyed.
class SaveWatchdog {
 public:
  explicit SaveWatchdog(cpu::Processor* processor) : processor_(processor) {
#if XE_PLATFORM_LINUX
    saver_ = pthread_self();
#endif
    thread_ = std::thread([this]() { Run(); });
  }
  ~SaveWatchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    int seconds = 0;
    while (!cv_.wait_for(lock, std::chrono::seconds(10),
                         [this]() { return done_; })) {
      seconds += 10;
      lock.unlock();
      std::string where;
#if XE_PLATFORM_LINUX
      uint64_t pcs[32];
      cpu::StackFrame frames[32];
      size_t count = processor_->stack_walker()->CaptureStackTrace(
          reinterpret_cast<void*>(saver_), pcs, 0, xe::countof(pcs), nullptr,
          nullptr);
      processor_->stack_walker()->ResolveStack(pcs, frames, count);
      for (size_t i = 0; i < count; ++i) {
        bool guest = frames[i].type == cpu::StackFrame::Type::kGuest;
        where += fmt::format(
            " [{}:{}{:08X}{}{}]", i, guest ? "g" : "h",
            guest ? uint64_t(frames[i].guest_pc) : uint64_t(frames[i].host_pc),
            frames[i].host_symbol.name[0] ? " " : "",
            frames[i].host_symbol.name);
      }
      if (!count) {
        where = " (saver stack not captured)";
      }
#endif
      XELOGE("SaveToFile watchdog: still paused after {} s{}; saver at{}",
             seconds, cpu::Processor::DescribeGlobalLockOwner(), where);
      lock.lock();
    }
  }

  cpu::Processor* processor_;
#if XE_PLATFORM_LINUX
  pthread_t saver_ = 0;
#endif
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};
}  // namespace

bool Emulator::SaveToFile(const std::filesystem::path& path,
                          std::function<void()> on_resumed) {
  std::lock_guard<std::mutex> buffer_lock(state_buffer_mutex_);
  last_save_error_.clear();
  // A system dialog (message box, keyboard) completes a guest request from
  // the host UI thread; a state saved while one is up would wait for that
  // completion forever.
  if (kernel_state_ && kernel_state_->xam_state() &&
      kernel_state_->xam_state()->IsUIActive()) {
    last_save_error_ = "a system dialog is open; close it first";
    XELOGE("SaveToFile: {}", last_save_error_);
    return false;
  }
  uint8_t* buffer = AcquireStateBuffer();
  if (!buffer) {
    XELOGE("SaveToFile: could not reserve the staging buffer");
    return false;
  }

  // Serialise into memory while the game is stopped; compress and write
  // after it is running again. The GPU reads its EDRAM back first.
  auto t0 = std::chrono::steady_clock::now();
  // A save that stops making progress freezes the game with it (one hotkey
  // save stalled after "XThread ... serializing", notes/40). Every 10 s of
  // pause this logs where the saving thread is and who owns the global
  // critical region, so the next such stall leaves evidence.
  SaveWatchdog watchdog(processor_.get());
  Pause(true);
  ByteStream stream(buffer, state_buffer_size_);
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  save_memory_fixups_.clear();
  bool ok = processor_->Save(&stream) && graphics_system_->Save(&stream) &&
            audio_system_->Save(&stream) &&
            audio_media_player_->Save(&stream) &&  // format 8
            kernel_state_->Save(&stream);
  if (ok) {
    // Threads saved inside RtlEnterCriticalSection's wait: their increment
    // of the lock count leaves the image (the re-issued call restores it)
    // and comes back for the live session right after.
    for (const auto& [address, delta] : save_memory_fixups_) {
      *memory_->TranslateVirtual<int32_t*>(address) += delta;
    }
    XELOGI("SaveToFile: memory section at raw offset {}", stream.offset());
    ok = memory_->Save(&stream);
    for (const auto& [address, delta] : save_memory_fixups_) {
      *memory_->TranslateVirtual<int32_t*>(address) -= delta;
    }
    if (!save_memory_fixups_.empty()) {
      XELOGI("SaveToFile: {} lock count(s) adjusted in the image",
             save_memory_fixups_.size());
    }
    save_memory_fixups_.clear();
  }
  if (!ok && last_save_error_.empty()) {
    last_save_error_ =
        "a guest thread could not be stopped at a safe point (see the log)";
  }
  const uint64_t raw_size = stream.offset();
  // The guest clock at the save, so a restore continues from it: the value
  // at the pause, which Resume() sets the clock back to.
  const uint64_t guest_tick_count = pause_guest_tick_count_
                                        ? pause_guest_tick_count_
                                        : Clock::QueryGuestTickCount();
  const uint64_t guest_system_time = Clock::QueryGuestSystemTime();
  Resume();
  const int64_t paused_ms = ElapsedMs(t0);
  if (on_resumed) {
    on_resumed();
  }
  if (!ok) {
    XELOGE("SaveToFile: save failed after {} ms paused, previous state kept",
           paused_ms);
    return false;
  }

  // Write to a temporary file and rename at the end, so a failed write
  // leaves the previous state untouched.
  auto t1 = std::chrono::steady_clock::now();
  std::filesystem::path temp_path = path;
  temp_path += ".tmp";
  std::error_code ec;
  FILE* file = filesystem::OpenFile(temp_path, "wb");
  if (!file) {
    XELOGE("SaveToFile: could not create {}", temp_path.string());
    return false;
  }

  SaveStateContainerHeader header = {};
  header.signature = kSaveStateContainerSignature;
  header.version = kSaveStateFormatVersion;
  header.flags = kSaveStateFlagLz4;
  header.chunk_size = kSaveStateChunkSize;
  header.raw_size = raw_size;
  header.chunk_count =
      uint32_t((raw_size + kSaveStateChunkSize - 1) / kSaveStateChunkSize);
  header.has_title_id = title_id_.has_value() ? 1 : 0;
  header.title_id = title_id_.value_or(0);
  header.media_id = media_id_;
  header.disc_number = disc_number_;
  header.disc_count = disc_count_;
  header.guest_tick_count = guest_tick_count;
  header.guest_system_time = guest_system_time;

  bool write_ok = fwrite(&header, sizeof(header), 1, file) == 1;
  std::vector<uint8_t> compressed(LZ4_compressBound(kSaveStateChunkSize));
  uint64_t stored_total = 0;
  for (uint64_t offset = 0; write_ok && offset < raw_size;
       offset += kSaveStateChunkSize) {
    SaveStateChunkHeader chunk;
    chunk.raw_size = uint32_t(std::min<uint64_t>(kSaveStateChunkSize,
                                                 raw_size - offset));
    int n = LZ4_compress_default(
        reinterpret_cast<const char*>(buffer + offset),
        reinterpret_cast<char*>(compressed.data()), int(chunk.raw_size),
        int(compressed.size()));
    const uint8_t* src = buffer + offset;
    chunk.stored_size = chunk.raw_size;
    if (n > 0 && uint32_t(n) < chunk.raw_size) {
      chunk.stored_size = uint32_t(n);
      src = compressed.data();
    }
    write_ok = fwrite(&chunk, sizeof(chunk), 1, file) == 1 &&
               fwrite(src, 1, chunk.stored_size, file) == chunk.stored_size;
    stored_total += chunk.stored_size;
  }
  write_ok = (fclose(file) == 0) && write_ok;

  if (!write_ok) {
    XELOGE("SaveToFile: writing {} failed, removed, previous state kept",
           temp_path.string());
    std::filesystem::remove(temp_path, ec);
    return false;
  }
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    XELOGE("SaveToFile: could not rename {} to {}: {}", temp_path.string(),
           path.string(), ec.message());
    std::filesystem::remove(temp_path, ec);
    return false;
  }
  XELOGI(
      "SaveToFile: {} bytes serialised in {} ms paused; {} chunks, {} bytes "
      "on disk ({:.2f}x), compressed and written in {} ms",
      raw_size, paused_ms, header.chunk_count, stored_total,
      stored_total ? double(raw_size) / double(stored_total) : 0.0,
      ElapsedMs(t1));
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> buffer_lock(state_buffer_mutex_);
  restore_warnings_.clear();

  // Get the serialised stream into memory: a format 1 file is mapped as it
  // is, a format 2 container is decompressed into the staging buffer. All
  // of this happens before the running title is touched, so a bad file
  // fails without terminating the game.
  std::unique_ptr<MappedMemory> map;
  uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t version = 1;

  FILE* file = filesystem::OpenFile(path, "rb");
  if (!file) {
    XELOGE("RestoreFromFile: could not open {}", path.string());
    return false;
  }
  SaveStateContainerHeader header = {};
  if (!ReadSaveStateHeader(file, &header, nullptr, nullptr)) {
    fclose(file);
    XELOGE("RestoreFromFile: {} is not a save state", path.string());
    return false;
  }
  if (header.version == 1) {
    fclose(file);
    map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
    if (!map) {
      return false;
    }
    data = map->data();
    size = map->size();
  } else {
    auto t0 = std::chrono::steady_clock::now();
    if (header.version > kSaveStateFormatVersion) {
      fclose(file);
      XELOGE(
          "RestoreFromFile: {} is format {}, this build reads up to format {}",
          path.string(), header.version, kSaveStateFormatVersion);
      return false;
    }
    if (header.flags & ~kSaveStateFlagLz4) {
      fclose(file);
      XELOGE("RestoreFromFile: {} uses unknown flags {:08X}", path.string(),
             header.flags);
      return false;
    }
    if (header.version < kSaveStateFirstVersionWithTimers) {
      // Format 4 added timers. Restoring an older file rebuilds the object
      // table without the periodic timer the game had already created, and
      // a title whose audio mixer runs off one then submits silence for
      // ever (notes/50). The file still loads; say why the sound died.
      XELOGW(
          "RestoreFromFile: {} is format {}, before timers were saved "
          "(format {}); the title's audio will stop",
          path.string(), header.version, kSaveStateFirstVersionWithTimers);
      AddRestoreWarning(fmt::format(
          "saved in format {} (before format {}): no timers in this file, "
          "so audio will stop - re-save this slot",
          header.version, kSaveStateFirstVersionWithTimers));
    }
    SaveStateFileInfo info;
    info.version = header.version;
    info.title_id = header.has_title_id ? header.title_id : 0;
    info.media_id = header.media_id;
    info.disc_number = header.disc_number;
    info.disc_count = header.disc_count;
    info.raw_size = header.raw_size;
    std::string mismatch = SaveStateMismatch(info);
    if (!mismatch.empty()) {
      fclose(file);
      XELOGE("RestoreFromFile: {}: {}", path.string(), mismatch);
      return false;
    }
    uint8_t* buffer = AcquireStateBuffer();
    if (!buffer || header.raw_size > state_buffer_size_ ||
        header.chunk_size == 0 || header.chunk_size > 256_MiB) {
      fclose(file);
      XELOGE("RestoreFromFile: {} is {} bytes of state, cannot stage it",
             path.string(), header.raw_size);
      return false;
    }
    std::vector<uint8_t> compressed(header.chunk_size);
    uint64_t offset = 0;
    bool read_ok = true;
    for (uint32_t i = 0; read_ok && i < header.chunk_count; ++i) {
      SaveStateChunkHeader chunk;
      if (fread(&chunk, sizeof(chunk), 1, file) != 1 ||
          chunk.raw_size > header.chunk_size ||
          chunk.stored_size > chunk.raw_size ||
          offset + chunk.raw_size > header.raw_size) {
        read_ok = false;
        break;
      }
      if (chunk.stored_size == chunk.raw_size) {
        read_ok = fread(buffer + offset, 1, chunk.raw_size, file) ==
                  chunk.raw_size;
      } else {
        read_ok = fread(compressed.data(), 1, chunk.stored_size, file) ==
                  chunk.stored_size;
        if (read_ok) {
          int n = LZ4_decompress_safe(
              reinterpret_cast<const char*>(compressed.data()),
              reinterpret_cast<char*>(buffer + offset), int(chunk.stored_size),
              int(chunk.raw_size));
          read_ok = n == int(chunk.raw_size);
        }
      }
      offset += chunk.raw_size;
    }
    fclose(file);
    if (!read_ok || offset != header.raw_size) {
      XELOGE("RestoreFromFile: {} is corrupt (chunk data at {} of {})",
             path.string(), offset, header.raw_size);
      return false;
    }
    data = buffer;
    size = size_t(header.raw_size);
    version = header.version;
    XELOGI("RestoreFromFile: {} bytes decompressed from {} chunks in {} ms",
           size, header.chunk_count, ElapsedMs(t0));
  }

  // Validate the header before touching the running title: a bad file must
  // not leave the game terminated and paused.
  ByteStream stream(data, size);
  if (size < 16 || stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    XELOGE("RestoreFromFile: {} is not a save state", path.string());
    return false;
  }
  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    XELOGE("RestoreFromFile: {} is for title {:08X}, not {:08X}", path.string(),
           title_id.value_or(0), title_id_.value_or(0));
    return false;
  }
  save_state_version_ = version;

  restoring_ = true;
  struct TimestampGuard {
    kernel::KernelState* ks;
    ~TimestampGuard() { ks->set_timestamp_updates_paused(false); }
  } timestamp_guard{kernel_state_.get()};
  // Keep the 1 ms KeTimeStampBundle timer from overwriting the restored
  // bundle before the guest clock has been read from it (older files).
  kernel_state_->set_timestamp_updates_paused(true);

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (header.version >= 8 && !audio_media_player_->Restore(&stream)) {
    XELOGE("Could not restore the media player!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // The state carries the guest memory from before any patch enabled since
  // the save (a 60 fps patch writes one byte the restore just put back), so
  // the title's enabled patches go on again, before anything runs.
  if (patcher_ && kernel_state_) {
    auto module = kernel_state_->GetExecutableModule();
    if (module) {
      // The patch DB matches on the hash of the unpatched code section,
      // computed when the title loaded. Hashing the restored memory instead
      // (as this did before) sees the bytes of every patch that was on at
      // the save, matches nothing, and re-applies nothing.
      std::optional<uint64_t> hash = title_module_hash_;
      if (!hash.has_value()) {
        if (!module->hash().has_value()) {
          module->CalculateHash();
        }
        hash = module->hash();
        XELOGW("RestoreFromFile: no launch-time module hash; using the hash "
               "of the restored code, which may not match the patch DB");
      }
      XELOGI("RestoreFromFile: re-applying patches for {:08X} (hash {:016X})",
             module->title_id(), hash.value_or(0));
      patcher_->ApplyPatchesForTitle(memory_.get(), module->title_id(), hash);
    }
  }

  // The guest clock (time base, uptime) is per process and otherwise jumps
  // at a restore: forward within a session (a cutscene then skips to its
  // end), backwards after a relaunch (the game's timed steps then wait for
  // the old session's time to come round: minutes on a loading screen).
  // Continue from the tick count at the save. Only format 6 files carry it;
  // the guest's KeTimeStampBundle is no use, TerminateTitle drops it and a
  // fresh one reads this process's uptime.
  if (header.version >= 6 && header.guest_tick_count) {
    uint64_t now = Clock::QueryGuestTickCount();
    Clock::SetGuestTickCount(header.guest_tick_count);
    // The Resume() below sets the clock back to its value at the Pause()
    // above unless told this is the value to keep.
    pause_guest_tick_count_ = header.guest_tick_count;
    kernel_state_->set_timestamp_updates_paused(false);
    kernel_state_->UpdateKeTimestampBundle();
    XELOGI("RestoreFromFile: guest clock set to {:.3f} s from the header (was "
           "{:.3f} s)",
           double(header.guest_tick_count) / Clock::guest_tick_frequency(),
           double(now) / Clock::guest_tick_frequency());
  } else {
    XELOGW(
        "RestoreFromFile: format {} file has no guest clock; timed steps in "
        "the game may stall until the old session's time comes round. Save "
        "again with this build.",
        header.version);
  }

  // Update the main thread, and hand every restored guest thread (created
  // suspended by XThread::Restore) to Resume(), which only resumes what it
  // is told about; the threads Pause() stopped were terminated above.
  // Pause() also suspended the host XThreads (GPU frame limiter, XMA
  // decoder, audio worker, kernel dispatch). TerminateTitle only removes
  // guest threads, so put those back before the list is replaced or they
  // stay parked forever.
  for (auto& thread : paused_threads_) {
    if (!thread->is_guest_thread() && thread->thread()) {
      thread->thread()->Resume(nullptr);
    }
  }
  paused_threads_.clear();
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
    }
    // A thread the guest created suspended and never started has a host
    // thread too, but is not running; the guest's own resume starts it.
    if (thread->is_guest_thread() && thread->thread() && thread->is_running()) {
      // A thread another thread had suspended stays parked until the
      // game's NtResumeThread; one parked in its own NtSuspendThread runs
      // to re-issue that call.
      if (thread->suspend_count() > 0 &&
          !thread->restored_self_suspend_pending()) {
        XELOGI("RestoreFromFile: thread {:08X} left suspended (guest suspend "
               "count {})",
               thread->handle(), thread->suspend_count());
        continue;
      }
      paused_threads_.push_back(thread);
    }
  }
  XELOGI("RestoreFromFile: {} guest threads restored", paused_threads_.size());

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

bool Emulator::ReadSaveStateInfo(const std::filesystem::path& path,
                                 SaveStateFileInfo* out_info) {
  FILE* file = filesystem::OpenFile(path, "rb");
  if (!file) {
    return false;
  }
  SaveStateContainerHeader header = {};
  bool ok = ReadSaveStateHeader(file, &header, nullptr, nullptr);
  fclose(file);
  if (!ok) {
    return false;
  }
  *out_info = {};
  out_info->version = header.version;
  out_info->title_id = header.has_title_id ? header.title_id : 0;
  out_info->media_id = header.media_id;
  out_info->disc_number = header.disc_number;
  out_info->disc_count = header.disc_count;
  out_info->raw_size = header.raw_size;
  return true;
}

std::string Emulator::SaveStateMismatch(const SaveStateFileInfo& info) const {
  if (info.title_id != title_id()) {
    return fmt::format("saved for title {:08X}, {:08X} is running",
                       info.title_id, title_id());
  }
  if (!info.has_disc_info()) {
    // Format 1 and 2 files carry no disc information; nothing to check.
    return "";
  }
  if (info.disc_count > 1 || disc_count_ > 1) {
    if (info.disc_number != disc_number_) {
      return fmt::format("saved on disc {} of {}, disc {} is running",
                         info.disc_number, info.disc_count, disc_number_);
    }
  }
  if (info.media_id && media_id_ && info.media_id != media_id_) {
    return fmt::format(
        "saved from a different disc image (media id {:08X}, running {:08X})",
        info.media_id, media_id_);
  }
  return "";
}

bool Emulator::ReadDiscInfo(const std::filesystem::path& path,
                            DiscInfo* out) {
  if (!out) {
    return false;
  }
  *out = DiscInfo();
  std::vector<uint8_t> header;
  std::unique_ptr<vfs::Device> device;
  switch (GetFileSignature(path)) {
    case FileSignatureType::XEX0:
    case FileSignatureType::XEXQ:
    case FileSignatureType::XEXH:
    case FileSignatureType::XEX25:
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2: {
      auto* f = xe::filesystem::OpenFile(path, "rb");
      if (!f) {
        return false;
      }
      header.resize(64 * 1024);
      size_t n = fread(header.data(), 1, header.size(), f);
      fclose(f);
      header.resize(n);
    } break;
    case FileSignatureType::XISO:
      device = std::make_unique<vfs::DiscImageDevice>("\\Device\\DiscScan",
                                                      path);
      break;
    case FileSignatureType::ZAR:
      device = std::make_unique<vfs::DiscZarchiveDevice>(
          "\\Device\\DiscScan", path);
      break;
    default:
      return false;
  }
  if (device) {
    if (!device->Initialize()) {
      return false;
    }
    auto* entry = device->ResolvePath("default.xex");
    if (!entry) {
      return false;
    }
    vfs::File* file = nullptr;
    if (entry->Open(vfs::FileAccess::kFileReadData, &file) !=
            X_STATUS_SUCCESS ||
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
        out->title_id = info->title_id;
        out->media_id = info->media_id;
        out->disc_number = info->disc_number;
        out->disc_count = info->disc_count;
      }
    }
  }
  return out->title_id != 0;
}

std::filesystem::path Emulator::PlaylistDisc(uint8_t n) {
  for (const auto& entry : disc_playlist_) {
    DiscInfo info;
    if (ReadDiscInfo(entry, &info) && info.disc_number == n) {
      return entry;
    }
  }
  return std::filesystem::path();
}

bool Emulator::SwapDisc(const std::filesystem::path& path,
                        uint8_t requested_disc, std::string* out_reason) {
  auto reason = [out_reason](std::string text) {
    XELOGW("SwapDisc refused: {}", text);
    if (out_reason) {
      *out_reason = std::move(text);
    }
    return false;
  };

  DiscInfo info;
  if (!ReadDiscInfo(path, &info)) {
    return reason("that file is not a disc image this title can use");
  }
  // The running title's own ids, so a disc from another game is refused.
  if (title_id_.has_value() && info.title_id != title_id_.value()) {
    return reason(fmt::format("that disc is for title {:08X}, not {:08X}",
                              info.title_id, title_id_.value()));
  }
  // Not the media id: it identifies the physical disc, not the release, so
  // every disc of a title has its own. Lost Odyssey's discs 1 and 2 are
  // 368DE6DD and 1888BE4E. Title id and disc number are what must match.
  if (requested_disc && info.disc_number != requested_disc) {
    return reason(fmt::format("that is disc {}, the title asked for disc {}",
                              info.disc_number, requested_disc));
  }

  // Open the tray around the change: a title that watches the tray rather
  // than calling XamSwapDisc (Blue Dragon polls XamLoaderGetDvdTrayState)
  // sees the disc leave and a new one arrive.
  auto* smc = kernel_state_ ? kernel_state_->smc() : nullptr;
  if (smc) {
    smc->SetTrayState(X_DVD_TRAY_STATE::OPEN);
    XELOGI("Disc swap: DVD tray reported open");
  }

  // Drop the outgoing disc rather than leaving it registered behind the new
  // one, where anything resolving the device by name still reads it.
  // The device itself stays alive until the title exits: a file handle the
  // guest still holds on the old disc points into its entries and its memory
  // map, and a read through it after the device was destroyed crashed the
  // process (Lost Odyssey streaming from the title screen when a timed swap
  // fired).
  const std::string previous_mount = disc_mount_path_;
  if (!previous_mount.empty()) {
    std::unique_ptr<vfs::Device> outgoing =
        file_system_->DetachDevice(previous_mount);
    if (outgoing) {
      ejected_discs_.push_back(std::move(outgoing));
    }
    XELOGI("Disc swap: unmounted the outgoing disc at {}", previous_mount);
  }

  const std::string mount_path =
      previous_mount.empty() ? "\\Device\\Cdrom0" : previous_mount;
  X_STATUS result = MountPath(path, mount_path);
  if (result != X_STATUS_SUCCESS) {
    if (smc) {
      smc->SetTrayState(X_DVD_TRAY_STATE::CLOSED);
    }
    return reason("that disc image could not be mounted");
  }
  disc_mount_path_ = mount_path;
  disc_number_ = info.disc_number;

  if (smc) {
    smc->SetTrayState(X_DVD_TRAY_STATE::CLOSED);
    XELOGI("Disc swap: DVD tray reported closed");
  }
  XELOGI("Disc swapped to {} (disc {} of {}) at {}", path.string(),
         info.disc_number, info.disc_count, mount_path);
  return true;
}

const std::filesystem::path Emulator::GetNewDiscPath(
    std::string window_message) {
  std::filesystem::path path = "";

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title(!window_message.empty() ? window_message
                                                 : "Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      {"All Files (*.*)", "*.*"},
  });

  if (file_picker->Show()) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }
  return path;
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  const bool in_guest_code = ex->pc() >= code_base && ex->pc() < code_end;

  if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past
    // it (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!in_guest_code) {
    // Didn't occur in guest code. Let it pass. Checked before the debugger
    // probe below: that reads /proc from inside a signal handler and is
    // slow, and this path is taken for every fault no other handler wanted.
    return false;
  }

  if (debugging::IsDebuggerAttached()) {
    // Xenia's debugger isn't attached but another one is; pass it to that
    // debugger.
    return false;
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());
  assert_not_null(guest_function);

  auto context = current_thread->thread_state()->context();

  std::string crash_msg;
  crash_msg.append("==== CRASH DUMP ====\n");
  crash_msg.append(fmt::format("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})\n",
                               current_thread->thread()->system_id(),
                               current_thread->thread_id()));
  crash_msg.append(
      fmt::format("Thread Handle: 0x{:08X}\n", current_thread->handle()));
  crash_msg.append(
      fmt::format("PC: 0x{:08X}\n",
                  guest_function->MapMachineCodeToGuestAddress(ex->pc())));
  if (ex->code() == Exception::Code::kAccessViolation) {
    const char* op_str = "unknown";
    if (ex->access_violation_operation() ==
        Exception::AccessViolationOperation::kRead) {
      op_str = "read";
    } else if (ex->access_violation_operation() ==
               Exception::AccessViolationOperation::kWrite) {
      op_str = "write";
    }
    crash_msg.append(fmt::format("Access Violation: {} at 0x{:016X}\n", op_str,
                                 ex->fault_address()));
  } else if (ex->code() == Exception::Code::kIllegalInstruction) {
    crash_msg.append("Illegal Instruction\n");
  }
  crash_msg.append("Registers:\n");
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" r{:<3} = {:016X}\n", i, context->r[i]));
  }
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" f{:<3} = {:016X} = (double){} = (float){}\n",
                                 i,
                                 *reinterpret_cast<uint64_t*>(&context->f[i]),
                                 context->f[i], *(float*)&context->f[i]));
  }
  for (int i = 0; i < 128; i++) {
    crash_msg.append(
        fmt::format(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]\n", i,
                    context->v[i].u32[0], context->v[i].u32[1],
                    context->v[i].u32[2], context->v[i].u32[3]));
  }
  XELOGE("{}", crash_msg);
  std::string crash_dlg = fmt::format(
      "The guest has crashed.\n\n"
      "Xenia has now paused itself.\n\n"
      "{}",
      crash_msg);
  // Display a dialog telling the user the guest has crashed.
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this,
                                                              &crash_dlg]() {
      xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_, "Uh-oh!", crash_dlg);
    });
  }

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  while (true) {
    if (main_thread_) {
      xe::threading::Wait(main_thread_->thread(), false);
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::ranges::find(std::as_const(game_config_load_callbacks_), callback) !=
      game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it =
      std::ranges::find(std::as_const(game_config_load_callbacks_), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  std::string path(fmt::format("{}\\", kDefaultGameSymbolicLink));

  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  if (!xam->loader_data().launch_path.empty()) {
    std::string symbolic_link_path;
    if (kernel_state_->file_system()->FindSymbolicLink(kDefaultGameSymbolicLink,
                                                       symbolic_link_path)) {
      std::filesystem::path file_path = symbolic_link_path;
      // Remove previous symbolic links.
      // Some titles can provide root within specific directory.
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultPartitionSymbolicLink);
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultGameSymbolicLink);

      file_path /= std::filesystem::path(xam->loader_data().launch_path);
      const auto registered_path =
          xe::path_to_utf8(file_path.parent_path()) + kGuestPathSeparator;

      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultPartitionSymbolicLink, registered_path);
      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultGameSymbolicLink, registered_path);

      return xe::path_to_utf8(file_path);
    }
  }

  if (!cvars::launch_module.empty()) {
    return path + cvars::launch_module;
  }

  return path + "default.xex";
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields we use + to bypass it
  return fmt::format("{}.{}.{}.{}", +version.major, +version.minor,
                     +version.build, +version.qfe);
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
  // Making changes to the UI (setting the icon) and executing game config
  // load callbacks which expect to be called from the UI thread.
  // If not on UI thread, dispatch to it synchronously.
  if (!display_window_->app_context().IsInUIThread()) {
    X_STATUS result = X_STATUS_UNSUCCESSFUL;
    display_window_->app_context().CallInUIThreadSynchronous(
        [this, &path, &module_path, &result]() {
          result = CompleteLaunch(path, module_path);
        });
    return result;
  }

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we
  // need to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us
  // to make sure any HostPathDevices are ready beforehand. (see comment above
  // cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_module_hash_.reset();
  title_name_ = "";
  title_version_ = "";
  display_window_->SetIcon(nullptr, 0);

  // Allow xam to request module loads.
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");

  XELOGI("Loading module {}", module_path);
  auto module = kernel_state_->LoadUserModule(module_path);
  if (!module) {
    XELOGE("Failed to load user module {}", path);
    return X_STATUS_NOT_FOUND;
  }

  if (!module->is_executable()) {
    kernel_state_->UnloadUserModule(module, false);
    XELOGE("Failed to load user module {}", path);
    return X_STATUS_NOT_SUPPORTED;
  }

  X_RESULT result = kernel_state_->ApplyTitleUpdate(module);
  if (XFAILED(result)) {
    XELOGE("Failed to apply title update! Cannot run module {}", path);
    return result;
  }

  result = kernel_state_->FinishLoadingUserModule(module);
  if (XFAILED(result)) {
    XELOGE("Failed to initialize user module {}", path);
    return result;
  }
  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  uint32_t workspace_address = 0;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  kernel_state_->memory()
      ->LookupHeapByType(false, 0x1000)
      ->Alloc(module->workspace_size(), 0x1000,
              kMemoryAllocationReserve | kMemoryAllocationCommit,
              kMemoryProtectRead | kMemoryProtectWrite, false,
              &workspace_address);

  if (!info) {
    title_id_ = 0;
    disc_number_ = disc_count_ = 0;
    media_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
    disc_number_ = info->disc_number;
    disc_count_ = info->disc_count;
    media_id_ = info->media_id;
    XELOGI("Title {:08X}: disc {} of {}, media id {:08X}", title_id_.value(),
           disc_number_, disc_count_, media_id_);
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled
    // by the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const auto db = kernel_state_->module_xdbf(module);

    game_info_database_ =
        std::make_unique<kernel::util::GameInfoDatabase>(db.get());
    kernel_state_->xam_state()->LoadSpaInfo(db.get());

    kernel_state_->xam_state()->user_tracker()->AddTitleToPlayedList();

    if (game_info_database_->IsValid()) {
      title_name_ = game_info_database_->GetTitleName(static_cast<XLanguage>(
          kernel_state_->xconfig()->ReadSetting<uint32_t>(
              kernel::XCONFIG_USER_CATEGORY, kernel::XCONFIG_USER_LANGUAGE)));
      XELOGI("Title name: {}", title_name_);

      // Show achievments data
      tabulate::Table table;
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Title", "Description", "Type", "Gamerscore"});

      const std::vector<kernel::util::GameInfoDatabase::Achievement>
          achievement_list = game_info_database_->GetAchievements();
      for (const kernel::util::GameInfoDatabase::Achievement& entry :
           achievement_list) {
        const std::string type = GetAchievementTypeName(
            kernel::xam::GetAchievementType(entry.flags));

        table.add_row({fmt::format("{}", entry.id), entry.label,
                       entry.description, type,
                       fmt::format("{}", entry.gamerscore)});
      }
      XELOGI("\n-------------------- ACHIEVEMENTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::Property>
          properties_list = game_info_database_->GetProperties();

      // 4D5307DC SPA contains a lot of properties, limit properties to log.
      const auto properties_list_limit =
          properties_list | std::views::take(150);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "Name", "Matchmaking", "Data Size"});

      for (const kernel::util::GameInfoDatabase::Property& entry :
           properties_list_limit) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.data_size)});
      }

      std::string properties_totals;

      if (properties_list.size() > properties_list_limit.size()) {
        properties_totals =
            fmt::format("\nProperties: {}/{}", properties_list_limit.size(),
                        properties_list.size());
      }

      XELOGI("\n-------------------- PROPERTIES --------------------{}\n{}",
             properties_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::Context> contexts_list =
          game_info_database_->GetContexts();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row(
          {"ID", "Name", "Matchmaking", "Default Value", "Max Value"});

      for (const kernel::util::GameInfoDatabase::Context& entry :
           contexts_list) {
        std::string label =
            string_util::remove_eol(string_util::trim(entry.description));

        table.add_row({fmt::format("{:08X}", entry.id), label,
                       entry.is_matchmaking ? "True" : "False",
                       fmt::format("{}", entry.default_value),
                       fmt::format("{}", entry.max_value)});
      }
      XELOGI("\n-------------------- CONTEXTS --------------------\n{}",
             table.str());

      const std::vector<kernel::util::GameInfoDatabase::StatsView> stats_views =
          game_info_database_->GetStatsViews();

      // 4D5307EA SPA contains a lot of stats, limit views to log.
      const auto stats_views_limit = stats_views | std::views::take(100);

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"ID", "View Type", "Name", "Skilled", "Arbitrated",
                     "Hidden", "Team View", "Online Only"});

      for (const kernel::util::GameInfoDatabase::StatsView& entry :
           stats_views_limit) {
        const std::string name =
            string_util::remove_eol(string_util::trim(entry.view.name));

        const std::string view_type =
            kernel::xam::GetViewTypeName(entry.view.view_type);

        table.add_row({fmt::format("{:08X}", entry.view.id), view_type, name,
                       entry.view.skilled ? "True" : "False",
                       entry.view.arbitrated ? "True" : "False",
                       entry.view.hidden ? "True" : "False",
                       entry.view.team_view ? "True" : "False",
                       entry.view.online_only ? "True" : "False"});
      }

      std::string stats_view_totals;

      if (stats_views.size() > stats_views_limit.size()) {
        stats_view_totals = fmt::format(
            "\nViews: {}/{}", stats_views_limit.size(), stats_views.size());
      }
      XELOGI("\n-------------------- STATS VIEWS --------------------{}\n{}",
             stats_view_totals.c_str(), table.str());

      const std::vector<kernel::util::GameInfoDatabase::PresenceMode>
          presence_modes = game_info_database_->GetPresenceModes();

      table = tabulate::Table();
      table.format().multi_byte_characters(true);
      table.add_row({"Context Value", "Contexts Count", "Properties Count"});

      for (const kernel::util::GameInfoDatabase::PresenceMode& entry :
           presence_modes) {
        table.add_row(
            {fmt::format("{}", entry.context_value),
             fmt::format("{}", entry.property_bag.contexts.size()),
             fmt::format("{}", entry.property_bag.properties.size())});
      }
      XELOGI("\n-------------------- PRESENCE MODES --------------------\n{}",
             table.str());

      auto icon_block = game_info_database_->GetIcon();
      if (!icon_block.empty()) {
        display_window_->SetIcon(icon_block.data(), icon_block.size());
      }
    }
  }

  // Initialize shader storage asynchronously - pipeline compilation happens in
  // background while the game goes through its normal startup (loading screens,
  // intro videos, etc.). With async_shader_compilation enabled, draws are
  // skipped until pipelines are ready, so this is safe. By the time actual
  // gameplay starts, most cached pipelines should be compiled.
  if (graphics_system_) {
    on_shader_storage_initialization(true);
    graphics_system_->InitializeShaderStorage(
        cache_root_, title_id_.value(), false,
        [this]() { on_shader_storage_initialization(false); });
  }

  auto main_thread = kernel_state_->LaunchModule(module);
  if (!main_thread) {
    return X_STATUS_UNSUCCESSFUL;
  }
  main_thread_ = main_thread;
  on_launch(title_id_.value(), title_name_);

  // Plugins must be loaded after calling LaunchModule() and
  // FinishLoadingUserModule() which will apply TUs and patching to the main
  // xex.
  if (cvars::allow_plugins) {
    if (plugin_loader_->IsAnyPluginForTitleAvailable(title_id_.value(),
                                                     module->hash().value())) {
      plugin_loader_->LoadTitlePlugins(title_id_.value(),
                                       module->hash().value());
    }
  }

  // Resume the main thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  main_thread_->Resume();

  if (cvars::pause_experiment_pause_seconds > 0) {
    std::thread([this]() {
      xe::threading::set_name("Pause Experiment");
      std::this_thread::sleep_for(
          std::chrono::seconds(cvars::pause_experiment_pause_seconds));
      auto t0 = std::chrono::steady_clock::now();
      Pause();
      XELOGI("PAUSE EXPERIMENT: paused in {} ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count());
      if (cvars::pause_experiment_resume_seconds >
          cvars::pause_experiment_pause_seconds) {
        std::this_thread::sleep_for(
            std::chrono::seconds(cvars::pause_experiment_resume_seconds -
                                 cvars::pause_experiment_pause_seconds));
        t0 = std::chrono::steady_clock::now();
        Resume();
        XELOGI("PAUSE EXPERIMENT: resumed in {} ms",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count());
      }
    }).detach();
  }

  if (cvars::disc_swap_experiment_seconds > 0) {
    std::thread([this]() {
      xe::threading::set_name("Disc Swap Experiment");
      std::this_thread::sleep_for(
          std::chrono::seconds(cvars::disc_swap_experiment_seconds));
      const uint8_t wanted =
          static_cast<uint8_t>(cvars::disc_swap_experiment_disc);
      std::filesystem::path path = PlaylistDisc(wanted);
      if (path.empty()) {
        path = cvars::disc_swap_experiment_path;
      }
      if (path.empty()) {
        XELOGE("DISC SWAP EXPERIMENT: no playlist and no path given");
        return;
      }
      XELOGI("DISC SWAP EXPERIMENT: asking for disc {} from {}", wanted,
             path.string());
      std::string refusal;
      auto t0 = std::chrono::steady_clock::now();
      const bool ok = SwapDisc(path, wanted, &refusal);
      XELOGI("DISC SWAP EXPERIMENT: {} in {} ms{}", ok ? "ok" : "refused",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count(),
             ok ? "" : (" - " + refusal));
    }).detach();
  }

  if (cvars::stats_log_seconds > 0) {
    std::thread([this]() {
      xe::threading::set_name("Stats Log");
      const int period = cvars::stats_log_seconds;
      uint64_t swaps = 0, audio = 0, silent = 0, xma = 0, cbs = 0, starved = 0,
               vblanks = 0;
      uint64_t draws = 0, passes = 0, rtxfers = 0, resolves = 0,
               resolve_px = 0;
      uint64_t ui_calls = 0, ui_calls_queued = 0;
      uint64_t rtxfer_bound = 0;
      uint64_t rtxfer_pushed = 0;
      uint64_t gpu_total_ns = 0, gpu_xfer_ns = 0, gpu_resolve_ns = 0;
      uint64_t scissor_area = 0;
      uint64_t frag_invocations = 0;
      auto t0 = std::chrono::steady_clock::now();
      while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(period));
        if (!graphics_system_ || !graphics_system_->command_processor() ||
            !audio_system_) {
          continue;
        }
        uint64_t s = graphics_system_->command_processor()->swap_count();
        uint64_t a = audio_system_->submitted_frame_count();
        uint64_t q = audio_system_->silent_frame_count();
        uint64_t x = apu::XmaContext::decoded_packet_count_.load();
        uint64_t c = apu::AudioDriver::callback_count_.load();
        uint64_t st = apu::AudioDriver::starved_callback_count_.load();
        uint64_t d = gpu::CommandProcessor::stats_draw_count_.load();
        uint64_t rp = gpu::CommandProcessor::stats_render_pass_count_.load();
        uint64_t tx = gpu::RenderTargetCache::stats_transfer_count_.load();
        uint64_t txb = gpu::RenderTargetCache::stats_transfer_bounded_eligible_.load();
        uint64_t txp = gpu::RenderTargetCache::stats_transfer_bounded_pushed_.load();
        uint64_t rv = gpu::RenderTargetCache::stats_resolve_count_.load();
        uint64_t rvp = gpu::RenderTargetCache::stats_resolve_pixels_.load();
        uint64_t gt = gpu::CommandProcessor::stats_gpu_total_ns_.load();
        uint64_t gx = gpu::CommandProcessor::stats_gpu_transfer_ns_.load();
        uint64_t gr = gpu::CommandProcessor::stats_gpu_resolve_ns_.load();
        uint64_t sca = gpu::CommandProcessor::stats_scissor_area_sum_.load();
        uint64_t fi = gpu::CommandProcessor::stats_gpu_fragment_invocations_.load();
        uint64_t vb = gpu::GraphicsSystem::stats_vblank_count_.load();
        uint64_t uic = ui::WindowedAppContext::stats_ui_calls_executed_.load();
        uint64_t uiq = ui::WindowedAppContext::stats_ui_calls_queued_.load();
        double t = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
        XELOGI(
            "STATS t={:.0f}s guest={:.1f}s vblanks +{} ({:.1f}/s) swaps +{} "
            "({:.1f}/s) audio_frames "
            "+{} ({:.1f}/s, {} silent) xma_packets +{} sdl_callbacks +{} "
            "starved +{} draws +{} passes +{} rt_transfers +{} (boundable +{} "
            "pushed +{}) resolves +{} ({:.1f} MPix) gpu_ms +{:.0f} (xfer "
            "{:.0f}, resolve {:.0f}) scissor_kpx/draw {:.0f} frag +{:.0f}M "
            "ui_calls +{} (queued +{}, backlog {}){}",
            t,
            double(Clock::QueryGuestTickCount()) /
                Clock::guest_tick_frequency(),
            vb - vblanks, double(vb - vblanks) / period, s - swaps,
            double(s - swaps) / period, a - audio, double(a - audio) / period,
            q - silent, x - xma, c - cbs, st - starved, d - draws, rp - passes,
            tx - rtxfers, txb - rtxfer_bound, txp - rtxfer_pushed,
            rv - resolves, double(rvp - resolve_px) / 1e6,
            double(gt - gpu_total_ns) / 1e6, double(gx - gpu_xfer_ns) / 1e6,
            double(gr - gpu_resolve_ns) / 1e6,
            (d - draws) ? double(sca - scissor_area) / (d - draws) / 1e3 : 0.0,
            double(fi - frag_invocations) / 1e6, uic - ui_calls,
            uiq - ui_calls_queued, uiq - uic,
            is_title_open() ? "" : " (no title)");
        ui_calls = uic;
        ui_calls_queued = uiq;
        scissor_area = sca;
        frag_invocations = fi;
        gpu_total_ns = gt;
        gpu_xfer_ns = gx;
        gpu_resolve_ns = gr;
        draws = d;
        passes = rp;
        rtxfers = tx;
        rtxfer_bound = txb;
        rtxfer_pushed = txp;
        resolves = rv;
        resolve_px = rvp;
        vblanks = vb;
        swaps = s;
        audio = a;
        silent = q;
        xma = x;
        cbs = c;
        starved = st;
      }
    }).detach();
  }
  if (cvars::savestate_experiment_save_seconds > 0) {
    std::thread([this]() {
      xe::threading::set_name("Savestate Experiment");
      std::filesystem::path path = cvars::savestate_experiment_path;
      int elapsed = 0;
      if (cvars::savestate_experiment_preload_seconds > 0 &&
          !cvars::savestate_experiment_preload_path.empty()) {
        std::this_thread::sleep_for(
            std::chrono::seconds(cvars::savestate_experiment_preload_seconds));
        elapsed = cvars::savestate_experiment_preload_seconds;
        XELOGI("SAVESTATE EXPERIMENT: preloading {}",
               cvars::savestate_experiment_preload_path);
        bool ok = RestoreFromFile(cvars::savestate_experiment_preload_path);
        XELOGI("SAVESTATE EXPERIMENT: preload {}", ok ? "ok" : "FAILED");
      }
      if (cvars::savestate_experiment_save_seconds > elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(
            cvars::savestate_experiment_save_seconds - elapsed));
      }
      if (cvars::savestate_experiment_step_only) {
        XELOGI("SAVESTATE EXPERIMENT: step-only (pause, step all, resume)");
        auto t0 = std::chrono::steady_clock::now();
        bool ok = StepAllGuestThreads();
        XELOGI("SAVESTATE EXPERIMENT: step-only {} in {} ms",
               ok ? "ok" : "FAILED",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count());
        return;
      }
      for (int cycle = 0;
           cycle < std::max(1, cvars::savestate_experiment_cycles); ++cycle) {
        if (cycle) {
          std::this_thread::sleep_for(std::chrono::seconds(20));
          XELOGI("SAVESTATE EXPERIMENT: cycle {}", cycle + 1);
        }
        auto t0 = std::chrono::steady_clock::now();
        int64_t ms = 0;
        bool ok = true;
        if (cvars::savestate_experiment_restore_only) {
          XELOGI("SAVESTATE EXPERIMENT: restore-only, not saving");
        } else {
          XELOGI("SAVESTATE EXPERIMENT: saving to {}", path.string());
          ok = SaveToFile(path);
          ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
          XELOGI("SAVESTATE EXPERIMENT: save {} in {} ms, {} bytes",
                 ok ? "ok" : "FAILED", ms,
                 std::filesystem::exists(path)
                     ? std::filesystem::file_size(path)
                     : 0);
        }
        if (ok && cvars::savestate_experiment_restore_seconds >
                      cvars::savestate_experiment_save_seconds) {
          std::this_thread::sleep_for(std::chrono::seconds(
              cvars::savestate_experiment_restore_seconds -
              cvars::savestate_experiment_save_seconds));
          for (int i = 0; i < std::max(1, cvars::savestate_experiment_restore_repeat);
               ++i) {
            if (i) {
              std::this_thread::sleep_for(std::chrono::seconds(20));
            }
            XELOGI("SAVESTATE EXPERIMENT: restoring from {} (#{})", path.string(),
                   i + 1);
            t0 = std::chrono::steady_clock::now();
            ok = RestoreFromFile(path);
            ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
            XELOGI("SAVESTATE EXPERIMENT: restore #{} {} in {} ms", i + 1,
                   ok ? "ok" : "FAILED", ms);
          }
        }
      }
    }).detach();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xe
