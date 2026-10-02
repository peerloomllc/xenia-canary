/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_PATCH_FILES_H_
#define XENIA_APP_PATCH_FILES_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace patcher {
class PatchDB;
}  // namespace patcher

namespace app {

// What the Patches, Cheats and Extras tabs of the Preferences window do to
// .patch.toml files and the community repository, with no drawing in it.

// Which tab a .patch.toml entry appears on. The file format has no category
// field, so this is guessed from the entry's name and description unless the
// user has moved it (patch_categories.txt in the storage root).
enum class PatchCategory {
  kFix = 0,    // fixes, graphics and speed
  kCheat = 1,  // gameplay advantages
  kExtra = 2,  // debug menus, free camera, skipped intros
};
constexpr int kPatchCategoryCount = 3;

PatchCategory GuessPatchCategory(const std::string& name,
                                 const std::string& desc);
const char* PatchCategoryTabName(PatchCategory category);
const char* PatchCategoryIntro(PatchCategory category);

// The user's placements, read from and written to `path` on first use.
class PatchCategories {
 public:
  explicit PatchCategories(std::filesystem::path path)
      : path_(std::move(path)) {}
  // The user's choice if there is one, otherwise the guess.
  PatchCategory Of(const std::filesystem::path& file, const std::string& name,
                   const std::string& desc);
  void Set(const std::filesystem::path& file, const std::string& name,
           PatchCategory category);

 private:
  void Load();
  void Save() const;
  std::filesystem::path path_;
  std::map<std::string, PatchCategory> categories_;  // "file|patch"
  bool loaded_ = false;
};

// Flips is_enabled for the [[patch]] entry named `patch_name`, keeping every
// other byte of the file (community files carry comments).
bool SetPatchEnabledInFile(const std::filesystem::path& path,
                           const std::string& patch_name, bool enabled);

// Switches off any enabled patch of the same title that writes a guest
// address the entry `name` in `file` also writes; returns their names and
// reloads the database if any changed.
std::vector<std::string> DisableConflictingPatches(
    patcher::PatchDB& db, const std::filesystem::path& file,
    const std::string& name);

// ---- The community repository (xenia-canary/game-patches) ----

struct CommunityPatchFile {
  std::string name;  // file name in the repository's patches/ folder
  std::string sha;   // git blob id
  uint32_t title_id;
};

// The repository's patch files, sorted by name. Blocking, off the UI thread.
bool FetchCommunityPatchList(std::vector<CommunityPatchFile>* files,
                             std::string* error);
// Downloads one file to `temp`. Blocking, off the UI thread.
bool DownloadCommunityPatch(const std::string& name,
                            const std::filesystem::path& temp,
                            std::string* error);
// Moves a downloaded `temp` over `target`, keeping the entries that were
// enabled in the old copy, and records its blob id under `storage_root`.
// Returns how many enabled entries were carried over, -1 on failure.
int InstallCommunityPatch(patcher::PatchDB* db,
                          const std::filesystem::path& temp,
                          const std::filesystem::path& target,
                          const std::filesystem::path& storage_root,
                          const std::string& name, const std::string& sha);

enum class CommunityFileState { kMissing, kOutdated, kCurrent };
// Blob ids of the files downloaded through the tab, by file name.
std::map<std::string, std::string> LoadCommunityShas(
    const std::filesystem::path& storage_root);
CommunityFileState StateOfCommunityFile(
    const std::filesystem::path& storage_root, const std::string& name,
    const std::string& tree_sha,
    const std::map<std::string, std::string>& recorded);

// GitHub's blob id of some content: sha1 of "blob <size>\0" + content.
std::string GitBlobSha(std::string_view content);
// Unescapes a JSON string body (\" \\ \/ \n \t \uXXXX with surrogate pairs).
std::string JsonUnescape(const std::string& in);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_PATCH_FILES_H_
