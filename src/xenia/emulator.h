/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_EMULATOR_H_
#define XENIA_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "xenia/apu/audio_media_player.h"
#include "xenia/base/delegate.h"
#include "xenia/base/exception_handler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/util/game_info_database.h"
#include "xenia/kernel/util/xlast.h"
#include "xenia/memory.h"
#include "xenia/patcher/patcher.h"
#include "xenia/patcher/plugin_loader.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {
class AudioSystem;
}  // namespace apu
namespace cpu {
class ExportResolver;
class Processor;
class ThreadState;
}  // namespace cpu
namespace gpu {
class GraphicsSystem;
}  // namespace gpu
namespace hid {
class InputDriver;
class InputSystem;
}  // namespace hid
namespace ui {
class ImGuiDrawer;
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {

constexpr fourcc_t kEmulatorSaveSignature = make_fourcc("XSAV");
// Save-state file container (format 2): header + LZ4 chunks around the
// serialised stream that starts with kEmulatorSaveSignature. A format 1 file
// is that stream alone, uncompressed.
constexpr fourcc_t kSaveStateContainerSignature = make_fourcc("XSSC");
// 3 adds the disc (number, count, media id) to the header. 4 adds I/O
// completion ports, timers and the file -> port links (kernel section).
// 5 adds each thread's life state and creation parameters, so a thread
// created suspended and not yet started is restored that way. 6 adds the
// guest clock (tick count, system time) to the header. 7 adds the mounted
// content packages (DLC, saves opened through XAM) to the kernel section, so
// their files can be reopened.
constexpr uint32_t kSaveStateFormatVersion = 9;

// What the container header of a save-state file says, without loading it.
struct SaveStateFileInfo {
  uint32_t version = 0;  // 1 = legacy raw stream, 2+ = container
  uint32_t title_id = 0;
  uint32_t media_id = 0;     // format 3+, else 0
  uint8_t disc_number = 0;   // format 3+, else 0
  uint8_t disc_count = 0;    // format 3+, else 0
  uint64_t raw_size = 0;     // format 2+, else 0
  bool has_disc_info() const { return version >= 3; }
};
static constexpr std::string_view kDefaultGameSymbolicLink = "GAME:";
static constexpr std::string_view kDefaultPartitionSymbolicLink = "D:";
static constexpr std::string_view kDefaultUpdateSymbolicLink = "UPDATE:";

// The main type that runs the whole emulator.
// This is responsible for initializing and managing all the various subsystems.
class Emulator {
 public:
  // This is the class for the top-level callbacks. They may be called in an
  // undefined order, so among them there must be no dependencies on each other,
  // especially hierarchical ones. If hierarchical handling is needed, for
  // instance, if a specific implementation of a subsystem needs to handle
  // changes, but the entire implementation must be reloaded, the implementation
  // in this example _must not_ register / unregister its own callback - rather,
  // the proper ordering and hierarchy should be constructed in a single
  // callback (in this example, for the whole subsystem).
  //
  // All callbacks must be created and destroyed in the UI thread only (or the
  // thread that takes its place in the architecture of the specific app if
  // there's no UI), as they are invoked in the UI thread.
  class GameConfigLoadCallback {
   public:
    GameConfigLoadCallback(Emulator& emulator);
    GameConfigLoadCallback(const GameConfigLoadCallback& callback) = delete;
    GameConfigLoadCallback& operator=(const GameConfigLoadCallback& callback) =
        delete;
    virtual ~GameConfigLoadCallback();

    // The callback is invoked in the UI thread (or the thread that takes its
    // place in the architecture of the specific app if there's no UI).
    virtual void PostGameConfigLoad() = 0;

   protected:
    Emulator& emulator() const { return emulator_; }

   private:
    Emulator& emulator_;
  };

  explicit Emulator(const std::filesystem::path& command_line,
                    const std::filesystem::path& storage_root,
                    const std::filesystem::path& content_root,
                    const std::filesystem::path& cache_root);
  ~Emulator();

  // Full command line used when launching the process.
  const std::filesystem::path& command_line() const { return command_line_; }

  // Folder persistent internal emulator data is stored in.
  const std::filesystem::path& storage_root() const { return storage_root_; }

  // Folder guest content is stored in.
  const std::filesystem::path& content_root() const { return content_root_; }
  // Points the emulator and the kernel's content manager at another folder
  // (Content > Content Folder). Only meant for when no title is running:
  // packages mounted from the old folder stay where they are.
  void set_content_root(const std::filesystem::path& content_root);

  // Folder files safe to remove without significant side effects are stored in.
  const std::filesystem::path& cache_root() const { return cache_root_; }

  // Name of the title in the default language.
  const std::string& title_name() const { return title_name_; }

  // Version of the title as a string.
  const std::string& title_version() const { return title_version_; }

  // Currently running title ID
  uint32_t title_id() const {
    return !title_id_.has_value() ? 0 : title_id_.value();
  }

  // Are we currently running a title?
  bool is_title_open() const { return title_id_.has_value(); }

  // Disc of a multi-disc title, from the XEX execution info (0 if unknown).
  uint8_t disc_number() const { return disc_number_; }
  uint8_t disc_count() const { return disc_count_; }
  uint32_t media_id() const { return media_id_; }
  bool is_multi_disc() const { return disc_count_ > 1; }

  // Window used for displaying graphical output. Can be null.
  ui::Window* display_window() const { return display_window_; }

  // ImGui drawer for various kinds of dialogs requested by the guest. Can be
  // null.
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }

  // Guest memory system modelling the RAM (both virtual and physical) of the
  // system.
  Memory* memory() const { return memory_.get(); }

  // Virtualized processor that can execute PPC code.
  cpu::Processor* processor() const { return processor_.get(); }

  // Audio hardware emulation for decoding and playback.
  apu::AudioSystem* audio_system() const { return audio_system_.get(); }

  // Xbox media player (XMP) emulation for WMA and MP3 playback.
  apu::AudioMediaPlayer* audio_media_player() const {
    return audio_media_player_.get();
  }

  // GPU emulation for command list processing.
  gpu::GraphicsSystem* graphics_system() const {
    return graphics_system_.get();
  }

  // Human-interface Device (HID) adapters for controllers.
  hid::InputSystem* input_system() const { return input_system_.get(); }

  // Kernel function export table used to resolve exports when JITing code.
  cpu::ExportResolver* export_resolver() const {
    return export_resolver_.get();
  }

  // File systems mapped to disc images, folders, etc for games and save data.
  vfs::VirtualFileSystem* file_system() const { return file_system_.get(); }

  // The 'kernel', tracking all kernel objects and other state.
  // This is effectively the guest operating system.
  kernel::KernelState* kernel_state() const { return kernel_state_.get(); }

  patcher::Patcher* patcher() const { return patcher_.get(); }
  const std::optional<uint64_t>& title_module_hash() const {
    return title_module_hash_;
  }
  void set_title_module_hash(uint64_t hash) { title_module_hash_ = hash; }

  patcher::PluginLoader* plugin_loader() const { return plugin_loader_.get(); }

  kernel::util::GameInfoDatabase* game_info_database() const {
    return game_info_database_.get();
  }
  // Initializes the emulator and configures all components.
  // The given window is used for display and the provided functions are used
  // to create subsystems as required.
  // Once this function returns a game can be launched using one of the Launch
  // functions.
  X_STATUS Setup(
      ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
      bool require_cpu_backend,
      std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
          audio_system_factory,
      std::function<std::unique_ptr<gpu::GraphicsSystem>()>
          graphics_system_factory,
      std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
          input_driver_factory);

  // Terminates the currently running title.
  X_STATUS TerminateTitle();

  const std::unique_ptr<vfs::Device> CreateVfsDevice(
      const std::filesystem::path& path, const std::string_view mount_path);

  X_STATUS MountPath(const std::filesystem::path& path,
                     const std::string_view mount_path);

  enum class FileSignatureType {
    XEX0,
    XEXQ,
    XEXH,
    XEX25,
    XEX1,
    XEX2,
    ELF,
    CON,
    LIVE,
    PIRS,
    XISO,
    XBE,
    ZAR,
    EXE,
    Unknown
  };

  // Determine the executable signature
  FileSignatureType GetFileSignature(const std::filesystem::path& path);

  // Launches a game from the given file path.
  // This will attempt to infer the type of the given file (such as an iso, etc)
  // using heuristics.
  X_STATUS LaunchPath(const std::filesystem::path& path);

  // Launches a game from a .xex file by mounting the containing folder as if it
  // was an extracted STFS container.
  X_STATUS LaunchXexFile(const std::filesystem::path& path);

  // Launches a game from a disc image file (.iso, etc).
  X_STATUS LaunchDiscImage(const std::filesystem::path& path);

  // Launches a game from a disc archive file (.zar, etc).
  X_STATUS LaunchDiscArchive(const std::filesystem::path& path);

  // Launches a game from an STFS container file.
  X_STATUS LaunchStfsContainer(const std::filesystem::path& path);

  X_STATUS LaunchDefaultModule(const std::filesystem::path& path);

  enum class InstallState : uint8_t {
    preparing,
    pending,
    installing,
    installed,
    failed
  };

  constexpr static std::string_view installStateStringName[5] = {
      "Preparing", "Pending", "Installing", "Success", "Failed"};

  struct ContentInstallEntry {
    ContentInstallEntry(std::filesystem::path path) : path_(path) {};

    std::string name_{};
    std::filesystem::path path_;
    std::filesystem::path filename_;
    std::filesystem::path data_installation_path_;
    std::filesystem::path header_installation_path_;

    uint64_t content_size_ = 0;
    uint64_t currently_installed_size_ = 0;
    XContentType content_type_{};

    InstallState installation_state_{};
    X_STATUS installation_result_{};
    std::string installation_error_message_{};

    std::unique_ptr<ui::ImmediateTexture> icon_;
  };

  // Migrates data from content to content/xuid with respect to common data.
  X_STATUS DataMigration(const uint64_t xuid);

  X_STATUS ProcessContentPackageHeader(const std::filesystem::path& path,
                                       ContentInstallEntry& installation_info);

  // Extract content of package to content specific directory.
  X_STATUS InstallContentPackage(const std::filesystem::path& path,
                                 ContentInstallEntry& installation_info);

  X_STATUS ExtractContentPackage(const std::filesystem::path& path,
                                 ContentInstallEntry& installation_info);

  // Extract content of zar package to desired directory.
  X_STATUS ExtractZarchivePackage(const std::filesystem::path& path,
                                  const std::filesystem::path& extract_dir);

  // Pack contents of a folder into a zar package.
  X_STATUS CreateZarchivePackage(const std::filesystem::path& inputDirectory,
                                 const std::filesystem::path& outputFile);

  struct PackContext {
    std::filesystem::path outputFilePath;
    std::ofstream currentOutputFile;
    bool hasError{false};
  };

  // capture_edram: read the EDRAM contents back for a save state.
  void Pause(bool capture_edram = false);
  void Resume();

  // Why the last SaveToFile refused before pausing ("" when it did not).
  const std::string& last_save_error() const { return last_save_error_; }
  // A (guest address, delta) to add to an int32 in the saved memory image
  // only; recorded by the threads during a save, cleared after it.
  void AddSaveMemoryFixup(uint32_t address, int32_t delta) {
    save_memory_fixups_.emplace_back(address, delta);
  }
  // Notes a restore left for the user (a different profile is signed in,
  // ...); cleared at the start of every RestoreFromFile.
  const std::vector<std::string>& restore_warnings() const {
    return restore_warnings_;
  }
  void AddRestoreWarning(std::string text) {
    restore_warnings_.push_back(std::move(text));
  }
  bool is_paused() const { return paused_; }
  // Pauses, serialises the state into memory, resumes (calling on_resumed),
  // then compresses and writes the file. The game is stopped only for the
  // serialisation.
  bool SaveToFile(const std::filesystem::path& path,
                  std::function<void()> on_resumed = nullptr);
  // Pause, step every running guest thread to a safe point, Resume. No file.
  bool StepAllGuestThreads();
  bool RestoreFromFile(const std::filesystem::path& path);
  // Reads a save-state file's header. False if it is not a save state.
  static bool ReadSaveStateInfo(const std::filesystem::path& path,
                                SaveStateFileInfo* out_info);
  // Why a file cannot be loaded into the running title, or "" if it can
  // (title and disc checks only; the content is not validated).
  std::string SaveStateMismatch(const SaveStateFileInfo& info) const;
  // Format version of the file being restored (1 = uncompressed legacy).
  uint32_t save_state_version() const { return save_state_version_; }

  // The game can request another title to be loaded.
  const std::filesystem::path GetNewDiscPath(std::string window_message = "");

  void WaitUntilExit();

 public:
  xe::Delegate<uint32_t, const std::string_view> on_launch;
  xe::Delegate<bool> on_shader_storage_initialization;
  xe::Delegate<> on_patch_apply;
  xe::Delegate<> on_terminate;
  xe::Delegate<> on_exit;
  // Fired at the end of Pause() (true) and Resume() (false), on the thread
  // that called them.
  xe::Delegate<bool> on_pause_state_changed;

 private:
  enum : uint64_t { EmulatorFlagDisclaimerAcknowledged = 1ULL << 0 };
  static uint64_t GetPersistentEmulatorFlags();
  static void SetPersistentEmulatorFlags(uint64_t new_flags);
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);

  void AddGameConfigLoadCallback(GameConfigLoadCallback* callback);
  void RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback);

  std::string FindLaunchModule();

  X_STATUS CompleteLaunch(const std::filesystem::path& path,
                          const std::string_view module_path);

  std::filesystem::path command_line_;
  std::filesystem::path storage_root_;
  std::filesystem::path content_root_;
  std::filesystem::path cache_root_;

  std::string title_name_;
  std::string title_version_;
  uint8_t disc_number_ = 0;
  uint8_t disc_count_ = 0;
  uint32_t media_id_ = 0;

  ui::Window* display_window_ = nullptr;
  ui::ImGuiDrawer* imgui_drawer_ = nullptr;

  std::unique_ptr<Memory> memory_;

  std::unique_ptr<cpu::Processor> processor_;
  std::unique_ptr<apu::AudioSystem> audio_system_;
  std::unique_ptr<apu::AudioMediaPlayer> audio_media_player_;
  std::unique_ptr<gpu::GraphicsSystem> graphics_system_;
  std::unique_ptr<hid::InputSystem> input_system_;

  std::unique_ptr<cpu::ExportResolver> export_resolver_;
  std::unique_ptr<vfs::VirtualFileSystem> file_system_;
  std::unique_ptr<patcher::Patcher> patcher_;
  std::unique_ptr<patcher::PluginLoader> plugin_loader_;

  std::unique_ptr<kernel::KernelState> kernel_state_;

  // Accessible only from the thread that invokes those callbacks (the UI thread
  // if the UI is available).
  std::vector<GameConfigLoadCallback*> game_config_load_callbacks_;
  // Using an index, not an iterator, because after the erasure, the adjustment
  // must be done for the vector element indices that would be in the iterator
  // range that would be invalidated.
  // SIZE_MAX if not currently in the game config load callback loop.
  size_t game_config_load_callback_loop_next_index_ = SIZE_MAX;

  kernel::object_ref<kernel::XThread> main_thread_;
  kernel::object_ref<kernel::XHostThread> plugin_loader_thread_;
  std::optional<uint32_t> title_id_;  // Currently running title ID
  // The executable module's hash from its load, before any patch changed
  // the code section; a save-state restore re-applies patches with it.
  std::optional<uint64_t> title_module_hash_;
  std::unique_ptr<kernel::util::GameInfoDatabase> game_info_database_;

  bool paused_;
  // The guest clock at Pause(); Resume() sets it back (pause_rewinds_guest_clock).
  uint64_t pause_guest_tick_count_ = 0;
  // Guest threads Pause() suspended, so Resume() undoes exactly that.
  std::vector<kernel::object_ref<kernel::XThread>> paused_threads_;
  bool restoring_;
  threading::Fence restore_fence_;  // Fired on restore finish.

  // Staging buffer for save/restore: a large anonymous reservation, kept for
  // the life of the emulator so later saves take no page faults while paused.
  uint8_t* AcquireStateBuffer();
  uint8_t* state_buffer_ = nullptr;
  size_t state_buffer_size_ = 0;
  std::mutex state_buffer_mutex_;
  uint32_t save_state_version_ = kSaveStateFormatVersion;
  std::string last_save_error_;
  std::vector<std::pair<uint32_t, int32_t>> save_memory_fixups_;
  std::vector<std::string> restore_warnings_;
};

}  // namespace xe

#endif  // XENIA_EMULATOR_H_
