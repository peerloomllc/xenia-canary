/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_EMULATOR_WINDOW_H_
#define XENIA_APP_EMULATOR_WINDOW_H_

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "xenia/app/profile_dialogs.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class ConsoleSettingsDialog;
class ContentListDialog;

struct RecentTitleEntry {
  std::string title_name;
  std::filesystem::path path_to_file;
  std::time_t last_run_time;
};

class EmulatorWindow {
 public:
  // Keyboard actions the user can rebind (HID > Keyboard hotkeys).
  enum class HotkeyAction : int {
    kPauseResume = 0,
    kMute,
    kSaveState,
    kLoadState,
    kNextSlot,
    kPrevSlot,
    kCount
  };

  using steady_clock = std::chrono::steady_clock;  // stdlib steady clock

  enum : size_t {
    // The UI is on top of the game and is open in special cases, so
    // lowest-priority.
    kZOrderHidInput,
    kZOrderImGui,
    kZOrderProfiler,
    // Emulator window controls are expected to be always accessible by the
    // user, so highest-priority.
    kZOrderEmulatorWindowInput,
  };

  virtual ~EmulatorWindow();

  static std::unique_ptr<EmulatorWindow> Create(
      Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
      uint32_t height);

  std::unique_ptr<xe::threading::Thread> Gamepad_HotKeys_Listener;

  int32_t selected_title_index = -1;

  static constexpr int64_t diff_in_ms(
      const steady_clock::time_point t1,
      const steady_clock::time_point t2) noexcept {
    using ms = std::chrono::milliseconds;
    return std::chrono::duration_cast<ms>(t1 - t2).count();
  }

  steady_clock::time_point last_mouse_up = steady_clock::now();
  steady_clock::time_point last_mouse_down = steady_clock::now();

  Emulator* emulator() const { return emulator_; }
  ui::WindowedAppContext& app_context() const { return app_context_; }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

  ui::Presenter* GetGraphicsSystemPresenter() const;
  void SetupGraphicsSystemPresenterPainting();
  void ShutdownGraphicsSystemPresenterPainting();

  void OnEmulatorInitialized();

  xe::X_STATUS RunTitle(const std::filesystem::path& path_to_file);
  // Called on the UI thread once Emulator::Setup and the path registrations
  // on the emulator thread are done. A launch requested before that (a
  // double-click on the dashboard right after the window appeared) is queued
  // and started here: RunTitle blocks the UI thread waiting for the GPU
  // command processor, which does not exist until Setup has run its own
  // UI-thread calls, so launching earlier deadlocked (notes/38).
  void OnEmulatorReady();
  std::atomic<bool> emulator_ready_{false};
  std::filesystem::path pending_launch_path_;
  void UpdateTitle();
  void SetFullscreen(bool fullscreen);
  void ToggleFullscreen();
  void SetInitializingShaderStorage(bool initializing);

  void TakeScreenshot();
  void ExportScreenshot(const xe::ui::RawImage& image);
  void SaveImage(const std::filesystem::path& path,
                 const xe::ui::RawImage& image);

  void ToggleProfilesConfigDialog();
  void ToggleXMPConfigDialog();
  void ToggleSupportDialog();
  void ToggleConsoleSettingsDialog();
  void ToggleContentListDialog();

  void SetHotkeysState(bool enabled) { disable_hotkeys_ = !enabled; }

  void ExtractContent(const std::filesystem::path file = "");

  // Types of button functions for hotkeys.
  enum class ButtonFunctions {
    ToggleFullscreen,
    RunTitle,
    CpuTimeScalarSetHalf,
    CpuTimeScalarSetDouble,
    CpuTimeScalarReset,
    ClearGPUCache,
    ToggleControllerVibration,
    ClearMemoryPageState,
    ReadbackResolve,
    ToggleLogging,
    IncTitleSelect,
    DecTitleSelect,
    Unknown
  };

  class ControllerHotKey {
   public:
    // If true the hotkey can be activated while a title is running, otherwise
    // false.
    bool title_passthru;

    // If true vibrate the controller after activating the hotkey, otherwise
    // false.
    bool rumble;
    std::string pretty;
    ButtonFunctions function;

    ControllerHotKey(ButtonFunctions fn = ButtonFunctions::Unknown,
                     std::string pretty = "", bool rumble = false,
                     bool active = true) {
      function = fn;
      this->pretty = pretty;
      title_passthru = active;
      this->rumble = rumble;
    }
  };

 private:
  class EmulatorWindowListener final : public ui::WindowListener,
                                       public ui::WindowInputListener {
   public:
    explicit EmulatorWindowListener(EmulatorWindow& emulator_window)
        : emulator_window_(emulator_window) {}

    void OnClosing(ui::UIEvent& e) override;
    void OnFileDrop(ui::FileDropEvent& e) override;

    void OnKeyDown(ui::KeyEvent& e) override;
    void OnKeyChar(ui::KeyEvent& e) override;

    void OnMouseDown(ui::MouseEvent& e) override;
    void OnMouseUp(ui::MouseEvent& e) override;

    void OnUsbDeviceChanged(bool is_arrival) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class DisplayConfigGameConfigLoadCallback
      : public Emulator::GameConfigLoadCallback {
   public:
    DisplayConfigGameConfigLoadCallback(Emulator& emulator,
                                        EmulatorWindow& emulator_window)
        : Emulator::GameConfigLoadCallback(emulator),
          emulator_window_(emulator_window) {}

    void PostGameConfigLoad() override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class ContentInstallDialog final : public ui::ImGuiDialog {
   public:
    ContentInstallDialog(
        ui::ImGuiDrawer* imgui_drawer, EmulatorWindow& emulator_window,
        std::shared_ptr<std::vector<Emulator::ContentInstallEntry>> entries)
        : ui::ImGuiDialog(imgui_drawer),
          emulator_window_(emulator_window),
          installation_entries_(entries) {
      window_id_ = GetWindowId();
    }

    ~ContentInstallDialog() {
      for (auto& entry : *installation_entries_) {
        entry.icon_.release();
      }
    }

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    uint64_t window_id_;

    EmulatorWindow& emulator_window_;
    std::shared_ptr<std::vector<Emulator::ContentInstallEntry>>
        installation_entries_;
  };

  class DisplayConfigDialog final : public ui::ImGuiDialog {
   public:
    DisplayConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  // Display > Advanced GPU options...: the GPU cvars worth a control,
  // written to the config as they change; the ones the graphics system
  // reads only at start-up are grouped under "needs a relaunch".
  class GpuOptionsDialog final : public ui::ImGuiDialog {
   public:
    GpuOptionsDialog(ui::ImGuiDrawer* imgui_drawer,
                     EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  void ToggleGpuOptionsDialog();
  std::unique_ptr<GpuOptionsDialog> gpu_options_dialog_;
  // Display > Dialog size: cvar ui_scale, applied to the ImGui drawer.
  void SetUIScale(float scale);
#if XE_PLATFORM_LINUX
  // Display > Settings window...: a GTK window (Graphics, Folders, Hotkeys
  // tabs) over the same config variables as the ImGui dialogs; a real
  // window that can sit next to or outside the game.
  void ToggleSettingsWindow();
  void RefreshSettingsWindow();
  void* settings_window_ = nullptr;  // GtkWidget*
  std::vector<std::pair<std::string, void*>> settings_refresh_labels_;
  // Re-evaluated by RefreshSettingsWindow: a setting whose availability or
  // wording depends on another one registers here, so changing the other
  // updates it without rebuilding the window. Cleared with the labels when
  // the window is built or destroyed.
  std::vector<std::function<void()>> settings_refresh_hooks_;
  int settings_capture_action_ = -1;
  // Profiles and Console tabs of the Preferences window.
  void BuildProfilesTab(void* notebook);
  void RefreshProfilesTab();
  void BuildConsoleTab(void* notebook);
  // Patches tab: the title's .patch.toml entries with checkboxes that
  // edit is_enabled in the file, and the community repository lookup.
  void BuildPatchesTab(void* notebook);
  void RefreshPatchesTab();
  void RefreshCommunityPatchList();
  void LookupCommunityPatches();
  void DownloadCommunityPatch(const std::string& name);
  std::map<uint32_t, std::string> PatchTitles();
  void* patches_status_ = nullptr;      // GtkLabel*
  void* patches_combo_ = nullptr;       // GtkComboBoxText*
  void* patches_box_ = nullptr;         // GtkBox*: the title's patches
  void* community_status_ = nullptr;    // GtkLabel*
  void* community_box_ = nullptr;       // GtkBox*
  void* community_show_all_ = nullptr;  // GtkCheckButton*
  void* community_filter_ = nullptr;    // GtkEntry*
  std::vector<uint32_t> patches_combo_title_ids_;
  uint32_t patches_selected_title_ = 0;
  bool patches_refreshing_ = false;
  struct CommunityPatchFile {
    std::string name;  // file name in the repository's patches/ folder
    std::string sha;   // git blob id
    uint32_t title_id;
  };
  std::vector<CommunityPatchFile> community_patch_files_;
  bool community_looked_up_ = false;
  bool community_lookup_running_ = false;
  int community_downloads_running_ = 0;
  void* profiles_list_ = nullptr;   // GtkListBox*
  void* profiles_status_ = nullptr;  // GtkLabel*
  std::unique_ptr<kernel::XConfigData> console_data_;  // edited copy
  std::vector<std::function<void()>> console_refreshers_;
  void* console_status_ = nullptr;  // GtkLabel*
#endif

  // Full-window dimming overlay with "PAUSED" while Emulator::is_paused().
  class PausedOverlayDialog final : public ui::ImGuiDialog {
   public:
    PausedOverlayDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  // Top-left status lines: "FAST-FORWARD 2.00x" / "SLOW-MOTION 0.50x" while
  // the guest time scalar is not 1, "MUTED" while audio is muted.
  class StatusOverlayDialog final : public ui::ImGuiDialog {
   public:
    StatusOverlayDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
    // Frame rate from the command processor's swap count, over ~0.5 s.
    uint64_t fps_last_swaps_ = 0;
    std::chrono::steady_clock::time_point fps_last_time_{};
    double fps_ = 0.0;
  };
  void ToggleFpsOverlay();



  class KeyboardHotkeysDialog final : public ui::ImGuiDialog {
   public:
    KeyboardHotkeysDialog(ui::ImGuiDrawer* imgui_drawer,
                          EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class SupportDialog final : public ui::ImGuiDialog {
   public:
    SupportDialog(ui::ImGuiDrawer* imgui_drawer,
                  EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  class XMPConfigDialog final : public ui::ImGuiDialog {
   public:
    XMPConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
      if (emulator_window_.emulator_->audio_media_player()) {
        volume_ = emulator_window_.emulator_->audio_media_player()
                      ->GetVolume()
                      ->load();
      }
    }

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
    float volume_ = 0.0f;
  };

  explicit EmulatorWindow(Emulator* emulator,
                          ui::WindowedAppContext& app_context, uint32_t width,
                          uint32_t height);

  bool Initialize();

  // For comparisons, use GetSwapPostEffectForCvarValue instead as the default
  // fallback may be used for multiple values.
  static const char* GetCvarValueForSwapPostEffect(
      gpu::CommandProcessor::SwapPostEffect effect);
  static gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
      const std::string& cvar_value);
  // For comparisons, use GetGuestOutputPaintEffectForCvarValue instead as the
  // default fallback may be used for multiple values.
  static const char* GetCvarValueForGuestOutputPaintEffect(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  static ui::Presenter::GuestOutputPaintConfig::Effect
  GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
  static ui::Presenter::GuestOutputPaintConfig
  GetGuestOutputPaintConfigForCvars();
  void ApplyDisplayConfigForCvars();

  void OnKeyDown(ui::KeyEvent& e);
  // Printable keys (letters, digits, space) arrive here on GTK, not in
  // OnKeyDown; the pause hotkey and key capture have to see both.
  void OnKeyChar(ui::KeyEvent& e);
  bool HandleAssignableHotkeys(ui::VirtualKey key, ui::KeyEvent& e);
  void OnMouseDown(const ui::MouseEvent& e);
  void ToggleFullscreenOnDoubleClick();
  void FileDrop(const std::filesystem::path& filename);
  void OnMouseUp(const ui::MouseEvent& e);
  void FileOpen();
  void FileClose();
  void InstallContent();
  void ExtractZarchive();
  void CreateZarchive();
  void ShowContentDirectory();
  void CpuTimeScalarReset();
  void CpuTimeScalarSetHalf();
  void CpuTimeScalarSetDouble();
  void CpuBreakIntoDebugger();
  void CpuBreakIntoHostDebugger();
  void TogglePauseEmulation();
  // Save states (experimental): one slot per title under --save_state_dir.
  // Both run off the UI thread and report through a notification.
  void SaveState();
  void LoadState();
  std::filesystem::path SaveStatePath();
  static constexpr int kSaveStateSlots = 9;
  // Slot 1..kSaveStateSlots; remembered in the config. delta cycles.
  // announce: show the slot table overlay (not from the slot dialog).
  void SelectSaveStateSlot(int slot, bool announce = true);
  void CycleSaveStateSlot(int delta);
  std::string SaveStateSlotSummary(int slot);
  // Slot file for the running title (and disc, for a multi-disc title).
  // Adopts older per-title files into the running disc's slot on first sight.
  std::filesystem::path SaveStateSlotPath(int slot);
  std::filesystem::path SaveStateDir() const;
  // Folder setting (cvar save_state_dir, "" = <storage root>/savestates).
  void PickSaveStateDir();  // folder picker; call from the UI loop
  void SetSaveStateDir(const std::filesystem::path& dir);
  static size_t CountSaveStateFiles(const std::filesystem::path& dir);
  void MoveSaveStates(const std::filesystem::path& from,
                      const std::filesystem::path& to);
  // Folder in use before the last change, while it still holds files.
  std::filesystem::path previous_save_state_dir_;

  // Game library: the games folder (cvar games_dir) scanned for
  // .iso/.xex/.zar, listed with a Launch button; Open starts there too.
  struct LibraryEntry {
    std::string label;  // path relative to the games folder
    std::filesystem::path path;
    uint64_t size;
    std::string title_name;  // from the recent list, if launched before
  };
  class GameLibraryDialog final : public ui::ImGuiDialog {
   public:
    GameLibraryDialog(ui::ImGuiDrawer* imgui_drawer,
                      EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  void ToggleGameLibraryDialog();
#if XE_PLATFORM_LINUX
  // Game library dashboard: a native list over the game view while no
  // title runs (File > Game Library toggles it). Backed by library.toml
  // in the storage root: one entry per file under games_dir with what the
  // XEX header says (title id, discs, media id, region), the name once
  // the title was launched, time played, last played and Tim's rating.
  struct LibraryTitle {
    std::filesystem::path path;
    std::string type;  // ISO, XEX, ZAR
    uint32_t title_id = 0;
    std::string title_name;
    uint8_t disc_number = 0;
    uint8_t disc_count = 0;
    uint32_t media_id = 0;
    uint32_t region = 0;
    uint64_t size = 0;
    int64_t seconds_played = 0;
    int64_t last_played = 0;
    int rating = 0;  // 0 none, 1-5 stars
  };
  void LoadLibrary();
  void SaveLibrary();
  void ScanLibrary();
  static bool ReadTitleInfo(LibraryTitle& title);
  void BuildDashboard();
  void RefreshDashboard();
  void ShowDashboard(bool show);
  void ToggleDashboard();
  void OnDashboardTitleLaunched();
  void AddPlayTime();
  LibraryTitle* LibraryEntryFor(const std::filesystem::path& path);

 public:
  bool DashboardRowVisible(void* model, void* iter);  // GTK filter callback

 private:
  std::vector<LibraryTitle> library_titles_;
  void* dashboard_widget_ = nullptr;  // GtkWidget*
  void* dashboard_store_ = nullptr;   // GtkListStore*
  void* dashboard_filter_ = nullptr;  // GtkTreeModelFilter*
  // Icons: the XDBF icon of a title, written to <storage>/library/icons/
  // <id>.png when it is launched, shown in the list's first column and in
  // the grid view (a GtkIconView over the same filtered rows).
  void* dashboard_stack_ = nullptr;       // GtkStack*: "list" / "grid"
  void* dashboard_grid_ = nullptr;        // GtkIconView*
  void* dashboard_grid_store_ = nullptr;  // GtkListStore*
  std::map<uint64_t, void*> dashboard_icons_;  // (id << 8 | size) -> GdkPixbuf*
  std::filesystem::path TitleIconPath(uint32_t title_id) const;
  void SaveTitleIcon();
  void* TitleIconPixbuf(uint32_t title_id, int size);
  void RefreshDashboardGrid();
  void LaunchLibraryIndex(int index);
  void* dashboard_search_ = nullptr;  // GtkEntry*
  void* dashboard_type_ = nullptr;    // GtkComboBoxText*
  void* dashboard_region_ = nullptr;  // GtkComboBoxText*
  void* dashboard_status_ = nullptr;  // GtkLabel*
  void* dashboard_back_ = nullptr;    // GtkButton*, only while a title runs
  void* dashboard_banner_ = nullptr;  // GtkBox* holding it, with the notice
  void* dashboard_banner_label_ = nullptr;  // GtkLabel*
  int dashboard_menu_index_ = -1;
  std::chrono::steady_clock::time_point session_start_;
  bool session_running_ = false;
  std::filesystem::path session_path_;
#endif
  // File > Reset Game / Close Game. RunTitle closes a running title first.
  void ResetGame();
  void CloseGame();
  // Starts a new emulator process with this one's arguments and `path` as
  // the title (none if empty), then closes this window. Linux only.
  bool RelaunchProcess(const std::filesystem::path& path);
  std::filesystem::path last_launched_path_;
  void PickGamesDir();  // folder picker; call from the UI loop
  void SetGamesDir(const std::filesystem::path& dir);
  void ScanGamesDir();
  std::unique_ptr<GameLibraryDialog> game_library_dialog_;
  std::vector<LibraryEntry> library_entries_;
  std::string library_status_;

  // Content folder (cvar content_root, "" = <storage root>/content): where
  // DLC, saves and installed titles are stored as <profile>/<title id>/
  // <content type>/<package>. Content > Content Folder... shows the setting
  // and what is installed, grouped by title.
  struct ContentItem {
    std::string type;  // content type name, plus the profile for saves
    std::string name;  // display name from the package header
    std::string file;  // package file or folder name
    uint64_t size;
    std::filesystem::path path;
  };
  struct ContentTitle {
    uint32_t title_id;
    std::string title_name;  // from the packages' headers
    uint64_t size;
    std::vector<ContentItem> items;
  };
  class ContentFolderDialog final : public ui::ImGuiDialog {
   public:
    ContentFolderDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  void ToggleContentFolderDialog();
  // The folder the config names, resolved the way xenia_main does at launch.
  std::filesystem::path ContentRootFromConfig() const;
  void PickContentRoot();  // folder picker; call from the UI loop
  void SetContentRoot(const std::filesystem::path& dir);
  // A change made while a title ran is applied once no title is running.
  void ApplyPendingContentRoot();
  void ScanContentRoot();
  static size_t CountContentTitles(const std::filesystem::path& dir);
  void MoveContent(const std::filesystem::path& from,
                   const std::filesystem::path& to);
  std::unique_ptr<ContentFolderDialog> content_folder_dialog_;
  std::vector<ContentTitle> content_titles_;
  std::string content_status_;
  // Folder in use before the last change, while it still holds titles.
  std::filesystem::path previous_content_root_;
  // Folder chosen while a title was running; empty when none is waiting.
  std::filesystem::path pending_content_root_;
  // Slot browser: thumbnail, time, size, Select/Save/Load per slot.
  class SaveStatesDialog final : public ui::ImGuiDialog {
   public:
    SaveStatesDialog(ui::ImGuiDrawer* imgui_drawer,
                     EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  void ToggleSaveStatesDialog();
  std::unique_ptr<SaveStatesDialog> save_states_dialog_;
  // PageUp/PageDown: the slot table over the game for a few seconds with
  // the selected slot highlighted; each press restarts the timer, Escape
  // closes it at once.
  class SlotOverlayDialog final : public ui::ImGuiDialog {
   public:
    SlotOverlayDialog(ui::ImGuiDrawer* imgui_drawer,
                      EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  std::unique_ptr<SlotOverlayDialog> slot_overlay_;
  std::chrono::steady_clock::time_point slot_overlay_deadline_;
  void ShowSlotOverlay();
  void HideSlotOverlay();
  struct SlotThumbnail {
    std::filesystem::file_time_type mtime;
    std::unique_ptr<ui::ImmediateTexture> texture;
  };
  std::map<int, SlotThumbnail> slot_thumbnails_;
  ui::ImmediateTexture* SlotThumbnailTexture(int slot);
  std::atomic<bool> state_op_in_progress_{false};
  void GpuTraceFrame();
  void GpuClearCaches();
  void ToggleDisplayConfigDialog();
  void ToggleKeyboardHotkeysDialog();
  void SetPausedOverlay(bool shown);
  // Shows or hides the status overlay to match the time scalar and mute
  // state; posts a "Speed" or "Audio" notification when asked.
  void UpdateStatusOverlay(const char* notify_title);
  void ToggleMute();
  // Assigns an action's key at runtime and persists it to the config.
  // Returns false (and leaves things unchanged) if the key is taken by a
  // fixed hotkey or another action, or cannot be represented.
  bool SetActionHotkey(HotkeyAction action, ui::VirtualKey key);
  void ClearActionHotkey(HotkeyAction action);
  std::optional<ui::VirtualKey> action_key(HotkeyAction action) const {
    return action_keys_[int(action)];
  }
  int capturing_action_ = -1;  // HotkeyAction being captured, or -1.
  std::string hotkey_status_;
  void ToggleControllerVibration();
  void ShowCompatibility();
  void ShowFAQ();
  void ShowBuildCommit();

  EmulatorWindow::ControllerHotKey ProcessControllerHotkey(int buttons);
  void VibrateController(xe::hid::InputSystem* input_sys, uint32_t user_index,
                         bool vibrate = true);
  void GamepadHotKeys();
  void ToggleGPUSetting(gpu::GPUSetting setting);
  void CycleReadbackResolve();
  void DisplayHotKeysConfig();

  static std::string CanonicalizeFileExtension(
      const std::filesystem::path& path);

  void RunPreviouslyPlayedTitle();
  void FillRecentlyLaunchedTitlesMenu(xe::ui::MenuItem* recent_menu);
  void LoadRecentlyLaunchedTitles();
  void AddRecentlyLaunchedTitle(std::filesystem::path path_to_file,
                                std::string title_name);

  void ClearDialogs();

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  EmulatorWindowListener window_listener_;
  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  std::unique_ptr<DisplayConfigGameConfigLoadCallback>
      display_config_game_config_load_callback_;
  // Creation may fail, in this case immediate drawer UI must not be drawn.
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;

  bool emulator_initialized_ = false;
  std::optional<ui::VirtualKey> action_keys_[int(HotkeyAction::kCount)];
  std::atomic<bool> disable_hotkeys_ = false;

  std::string base_title_;
  bool initializing_shader_storage_ = false;

  std::unique_ptr<DisplayConfigDialog> display_config_dialog_;
  std::unique_ptr<KeyboardHotkeysDialog> keyboard_hotkeys_dialog_;
  std::unique_ptr<PausedOverlayDialog> paused_overlay_;
  // Full-window "SAVING STATE..." / "LOADING STATE..." while a save-state
  // operation runs (they pause the game for its duration).
  class StateOverlayDialog final : public ui::ImGuiDialog {
   public:
    StateOverlayDialog(ui::ImGuiDrawer* imgui_drawer,
                       EmulatorWindow& emulator_window)
        : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

   protected:
    void OnDraw(ImGuiIO& io) override;

   private:
    EmulatorWindow& emulator_window_;
  };
  // UI thread only. nullptr hides it.
  void SetStateOverlay(const char* title, const char* hint);
  std::unique_ptr<StateOverlayDialog> state_overlay_;
  std::string state_overlay_title_;
  std::string state_overlay_hint_;
  std::unique_ptr<StatusOverlayDialog> status_overlay_;
  std::unique_ptr<ConsoleSettingsDialog> console_settings_dialog_;
  std::unique_ptr<ContentListDialog> content_list_dialog_;
  // Storing pointers and toggling dialog state is useful for broadcasting
  // messages back to guest.
  std::unique_ptr<ProfileConfigDialog> profile_config_dialog_;

  std::unique_ptr<XMPConfigDialog> xmp_config_dialog_;
  std::unique_ptr<SupportDialog> support_dialog_;

  std::vector<RecentTitleEntry> recently_launched_titles_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_WINDOW_H_
