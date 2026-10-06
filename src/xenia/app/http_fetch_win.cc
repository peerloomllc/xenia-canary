/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/http_fetch.h"

#include <fstream>
#include <functional>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace xe {
namespace app {

namespace {

std::wstring Wide(const std::string& text) {
  std::u16string utf16 = xe::to_utf16(text);
  return std::wstring(utf16.begin(), utf16.end());
}

struct InternetHandle {
  HINTERNET handle = nullptr;
  explicit InternetHandle(HINTERNET h) : handle(h) {}
  ~InternetHandle() {
    if (handle) {
      WinHttpCloseHandle(handle);
    }
  }
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
};

std::string LastError(const std::string& what) {
  return fmt::format("{} failed (error {})", what, GetLastError());
}

// GETs `url` (redirects are followed by WinHTTP) and hands each chunk of the
// body to `sink`.
bool Fetch(const std::string& url, const std::vector<std::string>& headers,
           int timeout_seconds,
           const std::function<bool(const char*, size_t)>& sink,
           std::string* error) {
  auto fail = [error](std::string message) {
    if (error) {
      *error = std::move(message);
    }
    return false;
  };
  std::wstring wide_url = Wide(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = DWORD(-1);
  parts.dwUrlPathLength = DWORD(-1);
  parts.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
    return fail(LastError("WinHttpCrackUrl"));
  }
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }

  InternetHandle session(
      WinHttpOpen(L"xenia-canary", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.handle) {
    // Before Windows 8.1 there is no automatic proxy type.
    session.handle =
        WinHttpOpen(L"xenia-canary", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session.handle) {
    return fail(LastError("WinHttpOpen"));
  }
  int timeout_ms = timeout_seconds * 1000;
  WinHttpSetTimeouts(session.handle, timeout_ms, timeout_ms, timeout_ms,
                     timeout_ms);
  InternetHandle connection(
      WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
  if (!connection.handle) {
    return fail(LastError("WinHttpConnect"));
  }
  DWORD flags =
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  InternetHandle request(WinHttpOpenRequest(
      connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
  if (!request.handle) {
    return fail(LastError("WinHttpOpenRequest"));
  }
  std::wstring header_block;
  for (const std::string& header : headers) {
    header_block += Wide(header) + L"\r\n";
  }
  if (!WinHttpSendRequest(request.handle,
                          header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                               : header_block.c_str(),
                          header_block.empty() ? 0 : DWORD(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.handle, nullptr)) {
    return fail(LastError("Request to " + url));
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(
          request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX)) {
    return fail(LastError("WinHttpQueryHeaders"));
  }
  if (status >= 400) {
    return fail(fmt::format("HTTP {} from {}", status, url));
  }
  char buffer[16384];
  for (;;) {
    DWORD read = 0;
    if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &read)) {
      return fail(LastError("WinHttpReadData"));
    }
    if (!read) {
      break;
    }
    if (!sink(buffer, read)) {
      return fail("Cannot write the download");
    }
  }
  return true;
}

}  // namespace

bool HttpGet(const std::string& url, const std::vector<std::string>& headers,
             int timeout_seconds, std::string* body, std::string* error) {
  body->clear();
  return Fetch(
      url, headers, timeout_seconds,
      [body](const char* data, size_t size) {
        body->append(data, size);
        return true;
      },
      error);
}

bool HttpDownload(const std::string& url,
                  const std::vector<std::string>& headers, int timeout_seconds,
                  const std::filesystem::path& path, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) {
      *error = "Cannot create " + xe::path_to_utf8(path);
    }
    return false;
  }
  return Fetch(
      url, headers, timeout_seconds,
      [&out](const char* data, size_t size) {
        out.write(data, std::streamsize(size));
        return bool(out);
      },
      error);
}

}  // namespace app
}  // namespace xe
