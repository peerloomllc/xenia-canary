/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/http_fetch.h"

#include <sys/wait.h>
#include <cstdio>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"

namespace xe {
namespace app {

namespace {

std::string ShellQuote(const std::string& text) {
  std::string quoted = "'";
  for (char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

// Runs curl with `extra` arguments; stdout (and stderr, for the error
// message) in *output.
bool RunCurl(const std::string& url, const std::vector<std::string>& headers,
             int timeout_seconds, const std::string& extra, std::string* output,
             std::string* error) {
  std::string command =
      fmt::format("curl -sSfL --max-time {}", timeout_seconds);
  for (const std::string& header : headers) {
    command += " -H " + ShellQuote(header);
  }
  command += " " + extra + " " + ShellQuote(url) + " 2>&1";
  output->clear();
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    if (error) {
      *error = "cannot run curl";
    }
    return false;
  }
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    output->append(buffer, n);
  }
  int status = pclose(pipe);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (code != 0) {
    if (error) {
      *error =
          fmt::format("curl failed ({}): {}", code, output->substr(0, 200));
    }
    return false;
  }
  return true;
}

}  // namespace

bool HttpGet(const std::string& url, const std::vector<std::string>& headers,
             int timeout_seconds, std::string* body, std::string* error) {
  return RunCurl(url, headers, timeout_seconds, "", body, error);
}

bool HttpDownload(const std::string& url,
                  const std::vector<std::string>& headers, int timeout_seconds,
                  const std::filesystem::path& path, std::string* error) {
  std::string output;
  return RunCurl(url, headers, timeout_seconds,
                 "-o " + ShellQuote(xe::path_to_utf8(path)), &output, error);
}

}  // namespace app
}  // namespace xe
