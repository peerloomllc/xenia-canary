/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/patch_files.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/app/http_fetch.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/patcher/patch_db.h"

namespace xe {
namespace app {

namespace {

const char* kCommunityPatchesTreeUrl =
    "https://api.github.com/repos/xenia-canary/game-patches/git/trees/"
    "main?recursive=1";
const char* kCommunityPatchesRawUrl =
    "https://raw.githubusercontent.com/xenia-canary/game-patches/main/"
    "patches/";

const std::vector<std::string> kGitHubHeaders = {
    "User-Agent: xenia-canary", "Accept: application/vnd.github+json"};

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

// Blob ids of the community files downloaded through the tab, one
// "<sha> <file name>" per line in the storage root, so a file whose
// is_enabled flags were toggled still counts as up to date.
std::filesystem::path CommunityShaFile(const std::filesystem::path& root) {
  return root / "community_patch_shas.txt";
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
  static const std::regex enabled_true(
      "(^|\n)([ \t]*is_enabled[ \t]*=[ \t]*)true");
  return GitBlobSha(std::regex_replace(content, enabled_true, "$1$2false"));
}

const char* PatchCategoryKeyword(PatchCategory category) {
  switch (category) {
    case PatchCategory::kCheat:
      return "cheat";
    case PatchCategory::kExtra:
      return "extra";
    default:
      return "fix";
  }
}

std::string PatchCategoryKey(const std::filesystem::path& file,
                             const std::string& name) {
  return xe::path_to_utf8(file.filename()) + "|" + name;
}

}  // namespace

// Guess which tab a patch entry belongs on from its name. The .patch.toml
// format has no category field, so the community files mix graphics fixes,
// gameplay cheats and debug toys in one list. The name decides: descriptions
// mention cheats too often to be safe ("No clipping, collision bugs..." under
// a 60 FPS entry, "Cheat Engine address: ..." under a camera one), and only
// the two most unmistakable phrases are read out of one. Anything the words
// do not recognise is a plain patch; the user can move an entry with the
// button beside it and the choice is remembered.
PatchCategory GuessPatchCategory(const std::string& name,
                                 const std::string& desc) {
  auto lower = [](const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(::tolower(c)); });
    return out;
  };
  std::string lower_name = lower(name);
  std::string lower_desc = lower(desc);
  auto name_has = [&lower_name](const char* needle) {
    return lower_name.find(needle) != std::string::npos;
  };
  // A stability or rendering fix wins over any word below: entries like
  // "Skip infinite loop on race end" are fixes that read as something else.
  if (name_has("fix") || name_has("loop") || name_has("crash") ||
      name_has("hash check")) {
    return PatchCategory::kFix;
  }
  // The game's own toys, checked first: "Enable Debug Menu" is an extra even
  // though such menus are where the cheats usually live.
  static const char* kExtraWords[] = {
      "debug menu",         "debug settings", "developer menu",
      "developer settings", "dev menu",       "test menu",
      "level select",       "free cam",       "freecam",
      "free camera",        "helicam",        "fly around",
      "wireframe",          "skip intro",     "skip logo",
      "skip video",         "skip movie",     "camera bounding box",
  };
  for (const char* word : kExtraWords) {
    if (name_has(word)) {
      return PatchCategory::kExtra;
    }
  }
  static const char* kCheatWords[] = {
      "infinite",     "unlimited",     "god mode",   "godmode",
      "invincib",     "no clip",       "noclip",     "one hit kill",
      "one-hit kill", "instant kill",  "unlock all", "all items",
      "all weapons",  "all character", "all cars",   "bottomless",
      "max money",    "max health",    "max ammo",   "max level",
      "max stats",    "never die",     "always win", "no reload",
      "cheat",
  };
  for (const char* word : kCheatWords) {
    if (name_has(word)) {
      return PatchCategory::kCheat;
    }
  }
  if (lower_desc.find("god mode") != std::string::npos ||
      lower_desc.find("invincib") != std::string::npos) {
    return PatchCategory::kCheat;
  }
  return PatchCategory::kFix;
}

const char* PatchCategoryTabName(PatchCategory category) {
  switch (category) {
    case PatchCategory::kCheat:
      return "Cheats";
    case PatchCategory::kExtra:
      return "Extras";
    default:
      return "Patches";
  }
}

const char* PatchCategoryIntro(PatchCategory category) {
  switch (category) {
    case PatchCategory::kCheat:
      return "Cheats give an advantage in the game: infinite ammo or health, "
             "god mode, everything unlocked. Most games have none.";
    case PatchCategory::kExtra:
      return "Extras are the game's own toys: debug menus, a free camera, "
             "wireframe drawing, skipped intro videos.";
    default:
      return "Patches change how the game runs: frame rate, resolution, "
             "filtering, broken effects.";
  }
}

// The user's own placements, one per line: <category> <file> <patch>, tab
// separated. Names never contain a tab; the file is rewritten whole.
void PatchCategories::Load() {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  std::ifstream in(path_);
  if (!in) {
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    size_t first = line.find('\t');
    if (first == std::string::npos) {
      continue;
    }
    size_t second = line.find('\t', first + 1);
    if (second == std::string::npos) {
      continue;
    }
    std::string keyword = line.substr(0, first);
    std::string file = line.substr(first + 1, second - first - 1);
    std::string name = line.substr(second + 1);
    PatchCategory category = PatchCategory::kFix;
    if (keyword == "cheat") {
      category = PatchCategory::kCheat;
    } else if (keyword == "extra") {
      category = PatchCategory::kExtra;
    }
    categories_[file + "|" + name] = category;
  }
}

void PatchCategories::Save() const {
  std::ofstream out(path_, std::ios::trunc);
  if (!out) {
    XELOGE("Patches: cannot write {}", xe::path_to_utf8(path_));
    return;
  }
  out << "# Where each patch entry appears in the Preferences window.\n";
  out << "# <category>\\t<patch file>\\t<patch name>\n";
  for (const auto& [key, category] : categories_) {
    size_t bar = key.find('|');
    if (bar == std::string::npos) {
      continue;
    }
    out << PatchCategoryKeyword(category) << '\t' << key.substr(0, bar) << '\t'
        << key.substr(bar + 1) << '\n';
  }
}

PatchCategory PatchCategories::Of(const std::filesystem::path& file,
                                  const std::string& name,
                                  const std::string& desc) {
  Load();
  auto it = categories_.find(PatchCategoryKey(file, name));
  if (it != categories_.end()) {
    return it->second;
  }
  return GuessPatchCategory(name, desc);
}

void PatchCategories::Set(const std::filesystem::path& file,
                          const std::string& name, PatchCategory category) {
  Load();
  categories_[PatchCategoryKey(file, name)] = category;
  Save();
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
    return q2 == std::string::npos ? std::string()
                                   : t.substr(q1 + 1, q2 - q1 - 1);
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
      lines[i] =
          prefix + "is_enabled = " + (enabled ? "true" : "false") + comment;
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

// The guest addresses a patch entry writes, as [first, last) pairs.
static std::vector<std::pair<uint32_t, uint32_t>> PatchWriteRanges(
    const xe::patcher::PatchInfoEntry& patch) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  for (const auto& data : patch.patch_data) {
    uint32_t size = uint32_t(std::max<size_t>(1, data.data.alloc_size));
    ranges.emplace_back(data.address, data.address + size);
  }
  return ranges;
}

static bool PatchRangesOverlap(
    const std::vector<std::pair<uint32_t, uint32_t>>& a,
    const std::vector<std::pair<uint32_t, uint32_t>>& b) {
  for (const auto& x : a) {
    for (const auto& y : b) {
      if (x.first < y.second && y.first < x.second) {
        return true;
      }
    }
  }
  return false;
}

// Two enabled patches that write the same address fight, and whichever the
// file loads last wins with nothing on screen to say so. Switching one on
// switches those off and says which.
std::vector<std::string> DisableConflictingPatches(
    patcher::PatchDB& db, const std::filesystem::path& file,
    const std::string& name) {
  std::vector<std::string> turned_off;
  const xe::patcher::PatchInfoEntry* self = nullptr;
  uint32_t title = 0;
  for (const auto& f : db.GetAllPatches()) {
    if (f.file_path != file) {
      continue;
    }
    for (const auto& patch : f.patch_info) {
      if (patch.patch_name == name) {
        self = &patch;
        title = f.title_id;
      }
    }
  }
  if (!self) {
    return turned_off;
  }
  auto mine = PatchWriteRanges(*self);
  for (const auto& f : db.GetAllPatches()) {
    if (f.title_id != title) {
      continue;
    }
    for (const auto& patch : f.patch_info) {
      if (!patch.is_enabled ||
          (f.file_path == file && patch.patch_name == name)) {
        continue;
      }
      if (!PatchRangesOverlap(mine, PatchWriteRanges(patch))) {
        continue;
      }
      if (SetPatchEnabledInFile(f.file_path, patch.patch_name, false)) {
        turned_off.push_back(patch.patch_name);
        XELOGI(
            "Patches: switched off '{}' in {}, it writes the same "
            "address as '{}'",
            patch.patch_name, xe::path_to_utf8(f.file_path.filename()), name);
      }
    }
  }
  if (!turned_off.empty()) {
    db.Reload(true);
  }
  return turned_off;
}

// ---- Community repository ----

bool FetchCommunityPatchList(std::vector<CommunityPatchFile>* files,
                             std::string* error) {
  files->clear();
  std::string json;
  if (!HttpGet(kCommunityPatchesTreeUrl, kGitHubHeaders, 60, &json, error)) {
    return false;
  }
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
    std::string sha =
        sha_pos == std::string::npos ? "" : json.substr(sha_pos + 7, 40);
    pos = end;
    const std::string prefix = "patches/";
    const std::string suffix = ".patch.toml";
    if (path.rfind(prefix, 0) != 0 || path.size() < suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
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
    files->push_back(std::move(file));
  }
  if (files->empty()) {
    if (error) {
      *error = "No patch files in the reply: " + json.substr(0, 200);
    }
    return false;
  }
  std::sort(files->begin(), files->end(),
            [](const CommunityPatchFile& a, const CommunityPatchFile& b) {
              return a.name < b.name;
            });
  return true;
}

bool DownloadCommunityPatch(const std::string& name,
                            const std::filesystem::path& temp,
                            std::string* error) {
  return HttpDownload(kCommunityPatchesRawUrl + UrlEncodeComponent(name),
                      {"User-Agent: xenia-canary"}, 60, temp, error);
}

int InstallCommunityPatch(patcher::PatchDB* db,
                          const std::filesystem::path& temp,
                          const std::filesystem::path& target,
                          const std::filesystem::path& storage_root,
                          const std::string& name, const std::string& sha) {
  // Keep what was enabled in the old copy.
  std::vector<std::string> enabled;
  if (db && std::filesystem::exists(target)) {
    auto old = db->ReadPatchFile(target);
    for (const auto& patch : old.patch_info) {
      if (patch.is_enabled) {
        enabled.push_back(patch.patch_name);
      }
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, target, ec);
  if (ec) {
    XELOGE("Patches: cannot move {} into place: {}", name, ec.message());
    return -1;
  }
  for (const std::string& patch_name : enabled) {
    SetPatchEnabledInFile(target, patch_name, true);
  }
  if (!sha.empty()) {
    RecordCommunitySha(storage_root, name, sha);
  }
  return int(enabled.size());
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

std::string GitBlobSha(std::string_view content) {
  std::string header = fmt::format("blob {}", content.size());
  sha1::SHA1 sha;
  sha.processBytes(header.data(), header.size() + 1);  // with the NUL
  sha.processBytes(content.data(), content.size());
  sha1::SHA1::digest8_t digest;
  sha.finalize(digest);
  std::string hex;
  for (uint8_t byte : digest) {
    hex += fmt::format("{:02x}", byte);
  }
  return hex;
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
        uint32_t cp =
            uint32_t(strtoul(in.substr(i + 1, 4).c_str(), nullptr, 16));
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

}  // namespace app
}  // namespace xe
