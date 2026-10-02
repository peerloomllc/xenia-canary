/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_HTTP_FETCH_H_
#define XENIA_APP_HTTP_FETCH_H_

#include <filesystem>
#include <string>
#include <vector>

namespace xe {
namespace app {

// Blocking HTTPS GET for the host UI's few network features (the community
// patch repository). Follows redirects and fails on an HTTP error status.
// `headers` are "Name: value" lines. Run it off the UI thread. curl on
// POSIX, WinHTTP on Windows.
bool HttpGet(const std::string& url, const std::vector<std::string>& headers,
             int timeout_seconds, std::string* body, std::string* error);

// The same, written to `path` (created or truncated).
bool HttpDownload(const std::string& url,
                  const std::vector<std::string>& headers, int timeout_seconds,
                  const std::filesystem::path& path, std::string* error);

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_HTTP_FETCH_H_
