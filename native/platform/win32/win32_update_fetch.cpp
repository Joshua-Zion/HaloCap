#include "win32_update_fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <vector>

namespace halocap::platform::win32 {

namespace {

constexpr std::size_t kMaxBody = 256 * 1024;

}  // namespace

bool HttpGetUtf8(const std::wstring& url, std::string* body, std::string* error) {
    if (body == nullptr) {
        return false;
    }
    body->clear();

    const auto fail = [&](const char* reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    HINTERNET session = WinHttpOpen(L"HaloCap/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return fail("open");
    }
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        WinHttpCloseHandle(session);
        return fail("url");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET connection =
        WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return fail("connect");
    }

    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return fail("request");
    }

    bool ok = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                           0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            if (status == 200) {
                ok = true;
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                        break;
                    }
                    if (body->size() + available > kMaxBody) {
                        ok = false;
                        fail("too_large");
                        break;
                    }
                    const std::size_t offset = body->size();
                    body->resize(offset + available);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, body->data() + offset, available, &read)) {
                        ok = false;
                        fail("read");
                        break;
                    }
                    body->resize(offset + read);
                }
            } else {
                char reason[32] = {};
                std::snprintf(reason, sizeof(reason), "status:%lu",
                              static_cast<unsigned long>(status));
                fail(reason);
            }
        } else {
            fail("status");
        }
    } else {
        fail("send");
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool HttpDownloadToFile(
    const std::wstring& url, const std::wstring& path,
    const std::function<void(std::int64_t received, std::int64_t total)>& progress,
    std::string* error) {
    const auto fail = [&](const char* reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    HINTERNET session = WinHttpOpen(L"HaloCap/2.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return fail("open");
    }
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 60000);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        WinHttpCloseHandle(session);
        return fail("url");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring requestPath(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        requestPath.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        return fail("connect");
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", requestPath.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return fail("request");
    }

    bool ok = false;
    std::int64_t received = 0;
    std::int64_t total = 0;
    HANDLE file = INVALID_HANDLE_VALUE;

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                           0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            fail("status");
        } else if (status != 200) {
            char reason[32] = {};
            std::snprintf(reason, sizeof(reason), "status:%lu", static_cast<unsigned long>(status));
            fail(reason);
        } else {
            DWORD contentLength = 0;
            DWORD lengthSize = sizeof(contentLength);
            if (WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &lengthSize,
                                    WINHTTP_NO_HEADER_INDEX)) {
                total = static_cast<std::int64_t>(contentLength);
            }

            file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                fail("file");
            } else {
                ok = true;
                std::vector<char> buffer(64 * 1024);
                if (progress) {
                    progress(0, total);
                }
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
                        break;
                    }
                    while (available > 0) {
                        const DWORD chunk =
                            std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                        DWORD read = 0;
                        if (!WinHttpReadData(request, buffer.data(), chunk, &read) || read == 0) {
                            ok = false;
                            fail("read");
                            break;
                        }
                        DWORD written = 0;
                        if (!WriteFile(file, buffer.data(), read, &written, nullptr) ||
                            written != read) {
                            ok = false;
                            fail("write");
                            break;
                        }
                        received += read;
                        available -= read;
                        if (progress) {
                            progress(received, total);
                        }
                    }
                    if (!ok) {
                        break;
                    }
                }
                CloseHandle(file);
                if (!ok) {
                    DeleteFileW(path.c_str());
                }
            }
        }
    } else {
        fail("send");
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (ok && progress) {
        progress(received, total);
    }
    return ok;
}

}
