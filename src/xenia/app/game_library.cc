/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <span>
#include <system_error>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace app {

void GameLibrary::Load(const std::filesystem::path& path) {
  titles.clear();
  std::ifstream file(path);
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
  auto* list = parsed["titles"].as_array();
  if (!list) {
    return;
  }
  for (auto& node : *list) {
    auto* t = node.as_table();
    if (!t) {
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
    Title title;
    std::string path_utf8 = str("path");
    if (path_utf8.empty()) {
      continue;
    }
    title.path = xe::to_path(path_utf8);
    title.type = str("type");
    title.title_id =
        uint32_t(std::strtoul(str("title_id").c_str(), nullptr, 16));
    title.title_name = str("title_name");
    title.disc_number = uint8_t(num("disc_number"));
    title.disc_count = uint8_t(num("disc_count"));
    title.media_id =
        uint32_t(std::strtoul(str("media_id").c_str(), nullptr, 16));
    title.region = uint32_t(std::strtoul(str("region").c_str(), nullptr, 16));
    title.size = uint64_t(num("size"));
    title.seconds_played = num("seconds_played");
    title.last_played = num("last_played");
    title.rating = int(std::clamp<int64_t>(num("rating"), 0, 5));
    titles.push_back(std::move(title));
  }
  XELOGI("Library: {} title(s) loaded", titles.size());
}

void GameLibrary::Save(const std::filesystem::path& path) const {
  toml::array list;
  for (const auto& title : titles) {
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
    list.push_back(std::move(t));
  }
  toml::table root;
  root.insert("titles", std::move(list));
  std::ofstream file(path, std::ofstream::trunc);
  file << root;
}

std::string GameLibrary::TypeOf(const std::filesystem::path& path) {
  std::string ext = xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  return ext == ".iso"   ? "ISO"
         : ext == ".xex" ? "XEX"
         : ext == ".zar" ? "ZAR"
                         : "";
}

// The name lives in the compressed part of the XEX and is filled in at first
// launch.
bool GameLibrary::ReadTitleInfo(Title& title) {
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
      device = std::make_unique<vfs::DiscZarchiveDevice>(
          "\\Device\\LibraryScan", title.path);
    }
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
        title.title_id = info->title_id;
        title.media_id = info->media_id;
        title.disc_number = info->disc_number;
        title.disc_count = info->disc_count;
      }
    }
  }
  uint32_t security = xex->security_offset;
  if (security + 0x180 <= header.size()) {
    title.region =
        xe::load_and_swap<uint32_t>(header.data() + security + 0x178);
  }
  return title.title_id != 0;
}

size_t GameLibrary::Scan(const std::filesystem::path& root,
                         const std::vector<Recent>& recent) {
  std::error_code ec;
  if (root.empty()) {
    XELOGW("Library: no games folder is set, nothing to scan");
    return 0;
  }
  if (!std::filesystem::is_directory(root, ec)) {
    XELOGW("Library: the games folder {} is not a readable directory",
           xe::path_to_utf8(root));
    return 0;
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
    std::string type = TypeOf(entry.path());
    if (type.empty()) {
      continue;
    }
    Title* existing = EntryFor(entry.path());
    if (existing) {
      existing->size = entry.file_size(ec);
      if (!existing->title_id) {
        ReadTitleInfo(*existing);
      }
      continue;
    }
    Title title;
    title.path = entry.path();
    title.type = type;
    title.size = entry.file_size(ec);
    if (!ReadTitleInfo(title)) {
      ++unreadable;
    }
    for (const auto& r : recent) {
      if (r.path == entry.path()) {
        title.title_name = r.title_name;
        title.last_played = r.last_run_time;
      }
    }
    titles.push_back(std::move(title));
    ++added;
  }
  // Drop entries whose file is gone, and playlists an older build recorded
  // as titles.
  std::erase_if(titles, [&](const Title& t) {
    return !std::filesystem::exists(t.path, ec) ||
           xe::utf8::lower_ascii(xe::path_to_utf8(t.path.extension())) ==
               ".m3u";
  });
  XELOGI(
      "Library: {} scanned, {} new, {} without a readable XEX header, {} total",
      xe::path_to_utf8(root), added, unreadable, titles.size());
  return added;
}

GameLibrary::Title* GameLibrary::EntryFor(const std::filesystem::path& path) {
  std::error_code ec;
  for (auto& title : titles) {
    if (title.path == path ||
        std::filesystem::equivalent(title.path, path, ec)) {
      return &title;
    }
  }
  return nullptr;
}

std::vector<size_t> GameLibrary::DiscGroup(size_t index) const {
  std::vector<size_t> group;
  if (index >= titles.size()) {
    return group;
  }
  const Title& t = titles[index];
  if (t.disc_count > 1 && t.title_id) {
    for (size_t i = 0; i < titles.size(); ++i) {
      const Title& o = titles[i];
      if (o.title_id == t.title_id && o.disc_count > 1 &&
          o.path.parent_path() == t.path.parent_path()) {
        group.push_back(i);
      }
    }
    std::sort(group.begin(), group.end(), [this](size_t a, size_t b) {
      return titles[a].disc_number < titles[b].disc_number;
    });
  } else {
    group.push_back(index);
  }
  return group;
}

std::filesystem::path GameLibrary::WritePlaylist(
    size_t index, const std::filesystem::path& dir) const {
  std::vector<size_t> group = DiscGroup(index);
  if (group.size() < 2) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path playlist =
      dir / fmt::format("{:08X}.m3u", titles[index].title_id);
  std::ofstream out(playlist, std::ios::trunc);
  if (!out) {
    XELOGE("Library: cannot write the playlist {}", xe::path_to_utf8(playlist));
    return {};
  }
  // The title boots the first entry, and swaps look entries up by disc
  // number, so put the disc that was played most recently first: launching a
  // multi-disc title from the library otherwise always started at disc 1,
  // whichever disc the last session ended on (and the save state slots shown
  // are the booted disc's).
  size_t first = group.front();
  int64_t newest = 0;
  for (size_t i : group) {
    if (titles[i].last_played > newest) {
      newest = titles[i].last_played;
      first = i;
    }
  }
  out << "# Written by the game library; the discs of this title in order,\n";
  out << "# starting with the one played most recently.\n";
  out << xe::path_to_utf8(titles[first].path) << '\n';
  for (size_t i : group) {
    if (i != first) {
      out << xe::path_to_utf8(titles[i].path) << '\n';
    }
  }
  if (first != group.front()) {
    XELOGI("Library: starting {} at disc {}, played most recently",
           titles[first].title_name.empty()
               ? xe::path_to_utf8(titles[first].path.filename())
               : titles[first].title_name,
           titles[first].disc_number);
  }
  return playlist;
}

std::string RegionText(uint32_t region) {
  if (region == 0) {
    return "";
  }
  if (region == 0xFFFFFFFFu) {
    return "All";
  }
  std::vector<std::string> parts;
  if (region & 0x000000FF) {
    parts.push_back("NTSC-U");
  }
  if (region & 0x0000FF00) {
    parts.push_back("NTSC-J");
  }
  if (region & 0x00FF0000) {
    parts.push_back("PAL");
  }
  if (region & 0xFF000000) {
    parts.push_back("Other");
  }
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

// A disc in its own folder takes the folder's name, which is how a multi-disc
// set is usually kept; anything else takes its file name.
std::string NameFromPath(const std::filesystem::path& path,
                         const std::filesystem::path& games_root) {
  std::error_code ec;
  if (!games_root.empty()) {
    std::filesystem::path rel =
        std::filesystem::relative(path.parent_path(), games_root, ec);
    if (!ec && !rel.empty() && rel != "." &&
        xe::path_to_utf8(rel).find("..") == std::string::npos) {
      std::string folder = xe::path_to_utf8(rel.filename());
      if (!folder.empty()) {
        return folder;
      }
    }
  }
  return xe::path_to_utf8(path.stem());
}

}  // namespace app
}  // namespace xe
