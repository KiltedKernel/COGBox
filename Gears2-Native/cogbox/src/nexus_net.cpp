// CogBox - the Fabricator's requests to Nexus Mods (see nexus_net.h). COGBOX_FAB_NEXUS builds only.
#ifdef COGBOX_FAB_NEXUS
#include "nexus_net.h"

#ifdef COGBOX_NEXUS_TESTS
#define NEXUS_APP_VERSION "0.0.0-test"   // (the test is built without the launcher's SDL / imgui headers)
#else
#include "app.h"   // COGBOX_VERSION
#define NEXUS_APP_VERSION COGBOX_VERSION
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
constexpr size_t kMaxAnswer = 8u << 20;   // 8 MiB: a mods page is tens of KB; a description at most a few hundred

std::wstring wide_of(const std::string &s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool ends_with(const std::string &s, const char *tail)
{
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

struct Target {
    std::string host;   // lower case
    int port = 0;
    bool secure = false;
    std::string path;   // path and query, never empty ("/")
};

// scheme://host[:port][/path][?query][#fragment]. False for anything odd: user info, characters outside a host name,
// a bad port, spaces or control characters anywhere.
bool split_url(const std::string &url, Target *t)
{
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f) return false;
    std::string lower;
    for (char c : url.substr(0, 8)) lower += (char)tolower((unsigned char)c);
    size_t at;
    if (lower.rfind("https://", 0) == 0) { t->secure = true; t->port = 443; at = 8; }
    else if (lower.rfind("http://", 0) == 0) { t->secure = false; t->port = 80; at = 7; }
    else return false;
    const size_t end = url.find_first_of("/?#\\", at);
    const std::string auth = url.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (end != std::string::npos && url[end] == '\\') return false;
    if (auth.empty() || auth.find('@') != std::string::npos) return false;
    const size_t colon = auth.find(':');
    std::string host = auth.substr(0, colon);
    if (colon != std::string::npos) {
        const std::string port = auth.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
        t->port = atoi(port.c_str());
        if (t->port <= 0 || t->port > 65535) return false;
    }
    if (host.empty() || host.size() > 253) return false;
    for (char &c : host) {
        c = (char)tolower((unsigned char)c);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos) return false;
    t->host = host;
    std::string path = end == std::string::npos ? std::string("/") : url.substr(end);
    const size_t hash = path.find('#');
    if (hash != std::string::npos) path.resize(hash);
    if (path.empty() || path[0] != '/') path = "/" + path;   // "?q" -> "/?q"
    t->path = path;
    return true;
}

#ifdef COGBOX_NEXUS_TESTS
std::mutex g_test_mu;
std::string g_test_base;
#endif
}  // namespace

bool nexus_host_allowed(const std::string &url)
{
    Target t;
    if (!split_url(url, &t) || !t.secure || t.port != 443) return false;
    return t.host == "api.nexusmods.com" || t.host == "users.nexusmods.com" || ends_with(t.host, ".nexusmods.com") ||
           ends_with(t.host, ".nexus-cdn.com");
}

#ifdef COGBOX_NEXUS_TESTS
void nexus_set_test_base(const std::string &http_loopback_base)
{
    std::lock_guard<std::mutex> lk(g_test_mu);
    g_test_base = http_loopback_base;
}
#endif

NexusResponse nexus_request(const std::string &method, const std::string &url, const std::string &body,
                            const std::vector<std::pair<std::string, std::string>> &headers, int timeout_ms)
{
    NexusResponse res;
    if (method.empty() || method.size() > 8 || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) {
        res.error = "Bad request method.";
        return res;
    }
    if (!nexus_host_allowed(url)) {
        res.error = "Refused: not a Nexus Mods https address.";
        return res;
    }
    Target t;
    split_url(url, &t);
#ifdef COGBOX_NEXUS_TESTS
    {
        std::lock_guard<std::mutex> lk(g_test_mu);
        Target b;
        if (!g_test_base.empty()) {
            if (!split_url(g_test_base, &b) || b.secure || b.host != "127.0.0.1") {
                res.error = "Bad test base.";
                return res;
            }
            t.host = b.host;
            t.port = b.port;
            t.secure = false;
        }
    }
#endif
    std::wstring hdr = L"Application-Name: COGBox\r\nApplication-Version: " + wide_of(NEXUS_APP_VERSION) + L"\r\n";
    bool has_type = false;
    for (const auto &h : headers) {
        if (h.first.empty() || h.first.find_first_of("\r\n:") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos) {
            res.error = "Bad request header.";
            return res;
        }
        if (_stricmp(h.first.c_str(), "Application-Name") == 0 || _stricmp(h.first.c_str(), "Application-Version") == 0) continue;   // (ours only)
        if (_stricmp(h.first.c_str(), "Content-Type") == 0) has_type = true;
        hdr += wide_of(h.first) + L": " + wide_of(h.second) + L"\r\n";
    }
    if (!body.empty() && !has_type) hdr += L"Content-Type: application/json\r\n";
    if (timeout_ms < 1000) timeout_ms = 1000;
    // timeout_ms is the whole request's time, not each step's: a server that drips its answer is cut off at it too.
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;

    static const std::wstring agent = L"COGBox/" + wide_of(NEXUS_APP_VERSION);
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // (pre-8.1 Windows)
    if (!s) {
        res.error = "Could not start a connection.";
        return res;
    }
    WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET c = WinHttpConnect(s, wide_of(t.host).c_str(), (INTERNET_PORT)t.port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, wide_of(method).c_str(), wide_of(t.path).c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, t.secure ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    if (!r) {
        res.error = "Nexus Mods could not be reached.";
    } else {
        DWORD off = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;   // a redirect could leave the allowed hosts
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
        if (!WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(r, NULL)) {
            const DWORD e = GetLastError();
            res.error = e == ERROR_WINHTTP_TIMEOUT ? "Nexus Mods did not answer in time." : "Nexus Mods could not be reached.";
        } else {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)status;
            std::string buf(1 << 16, '\0');
            for (;;) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) {
                    res.error = "Nexus Mods did not answer in time.";
                    res.body.clear();
                    break;
                }
                DWORD left = (DWORD)(deadline - now);   // (a read waits no longer than what is left)
                WinHttpSetOption(r, WINHTTP_OPTION_RECEIVE_TIMEOUT, &left, sizeof left);
                // (what is there only: a synchronous read of a full buffer would wait for all of it, past the deadline)
                DWORD avail = 0, got = 0;
                const bool ok = WinHttpQueryDataAvailable(r, &avail) &&
                                (avail == 0 || WinHttpReadData(r, buf.data(), (DWORD)std::min<size_t>(avail, buf.size()), &got));
                if (!ok) {
                    res.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "Nexus Mods did not answer in time." : "The answer from Nexus Mods was cut off.";
                    res.body.clear();
                    break;
                }
                if (got == 0) break;
                if (res.body.size() + got > kMaxAnswer) {
                    res.error = "The answer from Nexus Mods was too large.";
                    res.body.clear();
                    res.body.shrink_to_fit();
                    break;
                }
                res.body.append(buf.data(), got);
            }
            if (!res.error.empty()) res.status = 0;
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return res;
}

#endif  // COGBOX_FAB_NEXUS
