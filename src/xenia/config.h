/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CONFIG_H_
#define XENIA_CONFIG_H_

#include <filesystem>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include <map>

#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

toml::parse_result ParseFile(const std::filesystem::path& filename);

namespace config {
void SetupConfig(const std::filesystem::path& config_folder);
void LoadGameConfig(const std::string_view title_id);
// Per-game overrides, in <config folder>/config/<title id>.config.toml. These
// are read at launch by LoadGameConfig; writing one does not touch the value
// in memory, so it applies the next time that title runs.
std::filesystem::path GameConfigPath(const std::string_view title_id);
// toml_value is the value as it appears in the file ("true", "2", "\"fsi\"").
void SetGameConfigValue(const std::string_view title_id,
                        const std::string& category, const std::string& name,
                        const std::string& toml_value);
// "category.name" -> value, as written.
std::map<std::string, std::string> GameConfigValues(
    const std::string_view title_id);
void ClearGameConfig(const std::string_view title_id);
void SaveConfig();
}  // namespace config

#endif  // XENIA_CONFIG_H_
