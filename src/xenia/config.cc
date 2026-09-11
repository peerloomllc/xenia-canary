/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "config.h"

#include <fstream>
#include <sstream>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/system.h"

toml::parse_result ParseFile(const std::filesystem::path& filename) {
  return toml::parse_file(xe::path_to_utf8(filename));
}

CmdVar(config, "", "Specifies the target config to load.");

DEFINE_uint32(
    defaults_date, 0,
    "Do not modify - internal version of the default values in the config, for "
    "seamless updates if default value of any option is changed.",
    "Config");

namespace config {
std::string config_name = "xenia-canary.config.toml";
std::filesystem::path config_folder;
std::filesystem::path config_path;
std::string game_config_suffix = ".config.toml";

bool sortCvar(cvar::IConfigVar* a, cvar::IConfigVar* b) {
  if (a->category() < b->category()) {
    return true;
  }
  if (a->category() > b->category()) {
    return false;
  }
  if (a->name() < b->name()) {
    return true;
  }
  return false;
}

toml::parse_result ParseConfig(const std::filesystem::path& config_path) {
  try {
    return ParseFile(config_path);
  } catch (toml::parse_error& e) {
    xe::FatalError(fmt::format("Failed to parse config file '{}':\n\n{}",
                               config_path, e.what()));
    return toml::parse_result();
  }
}

void PrintConfigToLog(const std::filesystem::path& file_path) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return;
  }

  std::string config_dump = "----------- CONFIG DUMP -----------\n";
  std::string config_line = "";
  while (std::getline(file, config_line)) {
    if (config_line.empty()) {
      continue;
    }

    // Find place where comment begins and cut that part.
    const size_t comment_mark_position = config_line.find_first_of("#");
    if (comment_mark_position != std::string::npos) {
      config_line.erase(comment_mark_position, config_line.length());
    }

    // Check if remaining part of line is empty.
    if (std::ranges::all_of(std::as_const(config_line), isspace)) {
      continue;
    }
    // Check if line is a category mark. If it is add new line on start for
    // improved visibility.
    const bool category_mark = config_line.at(0) == '[';
    config_dump += (category_mark ? "\n" : "") + config_line + "\n";
  }
  config_dump += "----------- END OF CONFIG DUMP ----";
  XELOGI("{}", config_dump);

  file.close();
}

void ReadConfig(const std::filesystem::path& file_path,
                bool update_if_no_version_stored) {
  if (!cvar::ConfigVars) {
    return;
  }

  const auto config = ParseConfig(file_path);

  PrintConfigToLog(file_path);

  // Loading an actual global config file that exists - if there's no
  // defaults_date in it, it's very old (before updating was added at all, thus
  // all defaults need to be updated).
  auto defaults_date_cvar =
      dynamic_cast<cvar::ConfigVar<uint32_t>*>(cv::cv_defaults_date);
  assert_not_null(defaults_date_cvar);
  defaults_date_cvar->SetConfigValue(0);
  for (auto& it : *cvar::ConfigVars) {
    auto config_var = static_cast<cvar::IConfigVar*>(it.second);
    toml::path config_key =
        toml::path(config_var->category() + "." + config_var->name());

    const auto config_key_node = config.at_path(config_key);
    if (config_key_node) {
      config_var->LoadConfigValue(config_key_node.node());
    }
  }
  uint32_t config_defaults_date = defaults_date_cvar->GetTypedConfigValue();
  if (update_if_no_version_stored || config_defaults_date) {
    cvar::IConfigVarUpdate::ApplyUpdates(config_defaults_date);
  }

  // Check for type mismatch warnings
  if (cvar::config_type_mismatch_warnings &&
      !cvar::config_type_mismatch_warnings->empty()) {
    std::string warning_message =
        "The following config values had invalid types and have been reset to "
        "defaults:\n\n";
    for (const auto& name : *cvar::config_type_mismatch_warnings) {
      warning_message += "  - " + name + "\n";
    }
    warning_message +=
        "\nPlease check your config file. The config will be saved with the "
        "correct types.";

    xe::ShowSimpleMessageBox(xe::SimpleMessageBoxType::Warning,
                             warning_message);

    // Clear warnings
    cvar::config_type_mismatch_warnings->clear();
  }

  XELOGI("Loaded config: {}", file_path);
}

void ReadGameConfig(const std::filesystem::path& file_path) {
  if (!cvar::ConfigVars) {
    return;
  }
  const auto config = ParseConfig(file_path);
  for (auto& it : *cvar::ConfigVars) {
    auto config_var = static_cast<cvar::IConfigVar*>(it.second);
    toml::path config_key =
        toml::path(config_var->category() + "." + config_var->name());

    const auto config_key_node = config.at_path(config_key);
    if (config_key_node) {
      // LoadConfigValue would overwrite the value the main config holds, and
      // that is what SaveConfig writes back out, so this title's settings
      // would become everyone's on the way out. The game config value is a
      // separate slot that takes priority while the title runs.
      config_var->LoadGameConfigValue(config_key_node.node());
    }
  }
  XELOGI("Loaded game config: {}", file_path);
}

void SaveConfig() {
  if (config_path.empty()) {
    return;
  }

  // All cvar defaults have been updated on loading - store the current date.
  auto defaults_date_cvar =
      dynamic_cast<cvar::ConfigVar<uint32_t>*>(cv::cv_defaults_date);
  assert_not_null(defaults_date_cvar);
  defaults_date_cvar->SetConfigValue(
      cvar::IConfigVarUpdate::GetLastUpdateDate());

  std::vector<cvar::IConfigVar*> vars;
  if (cvar::ConfigVars) {
    for (const auto& s : *cvar::ConfigVars) {
      vars.push_back(s.second);
    }
  }
  std::ranges::sort(vars, [](auto a, auto b) {
    if (a->category() < b->category()) {
      return true;
    }
    if (a->category() > b->category()) {
      return false;
    }
    if (a->name() < b->name()) {
      return true;
    }
    return false;
  });

  // we use our own write logic because cpptoml doesn't
  // allow us to specify comments :(
  std::string last_category;
  bool last_multiline_description = false;
  xe::StringBuffer sb;
  for (auto config_var : vars) {
    if (config_var->is_transient()) {
      continue;
    }

    if (last_category != config_var->category()) {
      if (!last_category.empty()) {
        sb.Append('\n', 2);
      }
      last_category = config_var->category();
      last_multiline_description = false;
      sb.AppendFormat("[{}]\n", last_category);
    } else if (last_multiline_description) {
      last_multiline_description = false;
      sb.Append('\n');
    }

    auto value = config_var->config_value();
    size_t line_count;
    if (xe::utf8::find_any_of(value, "\n") == std::string_view::npos) {
      auto line = fmt::format("{} = {}", config_var->name(),
                              config_var->config_value());
      sb.Append(line);
      line_count = xe::utf8::count(line);
    } else {
      auto lines = xe::utf8::split(value, "\n");
      auto first = lines.cbegin();
      sb.AppendFormat("{} = {}\n", config_var->name(), *first);
      auto last = std::prev(lines.cend());
      for (auto it = std::next(first); it != last; ++it) {
        sb.Append(*it);
        sb.Append('\n');
      }
      sb.Append(*last);
      line_count = xe::utf8::count(*last);
    }

    constexpr size_t value_alignment = 50;
    const auto& description = config_var->description();
    if (!description.empty()) {
      if (line_count < value_alignment) {
        sb.Append(' ', value_alignment - line_count);
      }
      if (xe::utf8::find_any_of(description, "\n") == std::string_view::npos) {
        sb.AppendFormat("\t# {}\n", config_var->description());
      } else {
        auto lines = xe::utf8::split(description, "\n");
        auto first = lines.cbegin();
        sb.Append("\t# ");
        sb.Append(*first);
        sb.Append('\n');
        for (auto it = std::next(first); it != lines.cend(); ++it) {
          sb.Append(' ', value_alignment);
          sb.Append("\t# ");
          sb.Append(*it);
          sb.Append('\n');
        }
        last_multiline_description = true;
      }
    }
  }

  // save the config file
  xe::filesystem::CreateParentFolder(config_path);

  auto handle = xe::filesystem::OpenFile(config_path, "wb");
  if (!handle) {
    XELOGE("Failed to open '{}' for writing.", config_path);
  } else {
    fwrite(sb.buffer(), 1, sb.length(), handle);
    fclose(handle);
  }
}

void SetupConfig(const std::filesystem::path& config_folder) {
  config::config_folder = config_folder;
  // check if the user specified a specific config to load
  if (!cvars::config.empty()) {
    config_path = xe::to_path(cvars::config);
    if (std::filesystem::exists(config_path)) {
      // An external config file may contain only explicit overrides - in this
      // case, it will likely not contain the defaults version; don't update
      // from the version 0 in this case. Or, it may be a full config - in this
      // case, if it's recent enough (created at least in 2021), it will contain
      // the version number - updates the defaults in it.
      ReadConfig(config_path, false);
      return;
    }
  }
  // if the user specified a --config argument, but the file doesn't exist,
  // let's also load the default config
  if (!config_folder.empty()) {
    config_path = config_folder / config_name;
    if (std::filesystem::exists(config_path)) {
      ReadConfig(config_path, true);
    }
    // Re-save the loaded config to present the most up-to-date list of
    // parameters to the user, if new options were added, descriptions were
    // updated, or default values were changed.
    SaveConfig();
  }
}

std::filesystem::path GameConfigPath(const std::string_view title_id) {
  return config_folder / "config" / (std::string(title_id) + game_config_suffix);
}

namespace {
// The file as toml++ sees it, or an empty table when there is none yet.
toml::table ReadGameConfigTable(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return toml::table();
  }
  try {
    return toml::parse_file(xe::path_to_utf8(path));
  } catch (const toml::parse_error& e) {
    XELOGE("Cannot parse the per-game config {}: {}", path,
           std::string(e.description()));
    return toml::table();
  }
}
}  // namespace

void SetGameConfigValue(const std::string_view title_id,
                        const std::string& category, const std::string& name,
                        const std::string& toml_value) {
  const auto path = GameConfigPath(title_id);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  toml::table table = ReadGameConfigTable(path);

  // Parse the one line so the value keeps the type it has in the file.
  toml::table parsed;
  try {
    parsed = toml::parse(name + " = " + toml_value);
  } catch (const toml::parse_error& e) {
    XELOGE("Per-game config: cannot write {} = {}: {}", name, toml_value,
           std::string(e.description()));
    return;
  }
  if (!table.contains(category)) {
    table.insert(category, toml::table());
  }
  auto* cat = table.get_as<toml::table>(category);
  if (!cat) {
    XELOGE("Per-game config: [{}] is not a table in {}", category, path);
    return;
  }
  if (auto* v = parsed.get_as<bool>(name)) {
    cat->insert_or_assign(name, v->get());
  } else if (auto* v = parsed.get_as<int64_t>(name)) {
    cat->insert_or_assign(name, v->get());
  } else if (auto* v = parsed.get_as<double>(name)) {
    cat->insert_or_assign(name, v->get());
  } else if (auto* v = parsed.get_as<std::string>(name)) {
    cat->insert_or_assign(name, v->get());
  } else {
    XELOGE("Per-game config: {} has a type this cannot write", name);
    return;
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    XELOGE("Per-game config: cannot write {}", path);
    return;
  }
  out << "# Settings for this title only. Read when it launches, over the\n"
         "# main configuration file.\n"
      << table << "\n";
  XELOGI("Per-game config: {}.{} = {} in {}", category, name, toml_value, path);
}

std::map<std::string, std::string> GameConfigValues(
    const std::string_view title_id) {
  std::map<std::string, std::string> out;
  const toml::table table = ReadGameConfigTable(GameConfigPath(title_id));
  for (const auto& [category, node] : table) {
    const auto* cat = node.as_table();
    if (!cat) {
      continue;
    }
    for (const auto& [name, value] : *cat) {
      std::string text;
      if (const auto* v = value.as_boolean()) {
        text = v->get() ? "true" : "false";
      } else if (const auto* v = value.as_integer()) {
        text = std::to_string(v->get());
      } else if (const auto* v = value.as_floating_point()) {
        text = fmt::format("{}", v->get());
      } else if (const auto* v = value.as_string()) {
        text = v->get();
      } else {
        continue;
      }
      out[std::string(category.str()) + "." + std::string(name.str())] = text;
    }
  }
  return out;
}

void ClearGameConfig(const std::string_view title_id) {
  std::error_code ec;
  const auto path = GameConfigPath(title_id);
  if (std::filesystem::remove(path, ec)) {
    XELOGI("Per-game config: removed {}", path);
  }
}

void LoadGameConfig(const std::string_view title_id) {
  const auto game_config_folder = config_folder / "config";
  const auto game_config_path =
      game_config_folder / (std::string(title_id) + game_config_suffix);
  if (std::filesystem::exists(game_config_path)) {
    ReadGameConfig(game_config_path);
  }
}

}  // namespace config
