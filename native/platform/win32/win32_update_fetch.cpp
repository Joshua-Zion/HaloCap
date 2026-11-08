#include "win32_update_fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstddef>

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

}
