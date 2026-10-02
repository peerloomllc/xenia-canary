/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_LIBRARY_H_
#define XENIA_APP_GAME_LIBRARY_H_

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace app {

// The game library behind the dashboard: library.toml in the storage root,
// one entry per file under games_dir with what the XEX header says (title
// id, discs, media id, region), the name once the title was launched, time
// played, last played and the user's rating. No drawing here; the dashboard
// that shows it is per platform.
class GameLibrary {
 public:
  struct Title {
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
  // A title the window launched before, to name a newly found file.
  struct Recent {
    std::filesystem::path path;
    std::string title_name;
    std::time_t last_run_time;
  };

  static constexpr const char* kFilename = "library.toml";

  std::vector<Title> titles;

  void Load(const std::filesystem::path& file);
  void Save(const std::filesystem::path& file) const;
  // Adds every ISO, XEX and ZAR file up to three folders deep under `root`
  // and drops entries whose file is gone. Returns the number added.
  size_t Scan(const std::filesystem::path& root,
              const std::vector<Recent>& recent);
  // Title id, discs, media id and region from the XEX2 header of the file
  // (ISO/ZAR: default.xex at the disc root), without launching.
  static bool ReadTitleInfo(Title& title);
  // "ISO", "XEX", "ZAR" from the extension, empty for anything else.
  static std::string TypeOf(const std::filesystem::path& path);

  Title* EntryFor(const std::filesystem::path& path);
  // Indices of every entry that belongs to the same multi-disc title as
  // entry `index` (same title id and folder), including itself, in disc
  // order. A single-disc title yields just itself.
  std::vector<size_t> DiscGroup(size_t index) const;
  // Writes <dir>/<title id>.m3u listing the discs of entry `index`, the one
  // played most recently first. Empty for a single-disc title.
  std::filesystem::path WritePlaylist(size_t index,
                                      const std::filesystem::path& dir) const;
};

// Text for the library's columns.
std::string RegionText(uint32_t region);
std::string TimePlayedText(int64_t seconds);
std::string DateText(int64_t ts);
std::string RatingText(int rating);
// What to call a title the emulator has never run: the name of its folder
// under `games_root` if it has one, otherwise its file name.
std::string NameFromPath(const std::filesystem::path& path,
                         const std::filesystem::path& games_root);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_LIBRARY_H_
