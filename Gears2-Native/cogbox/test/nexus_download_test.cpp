// CogBox - test of getting a Nexus Mods file (COGBOX_FAB_NEXUS): the Downloads pickup's matching (fixtures in a
// temporary folder: the exact name, a " (1)" copy, the upload-time tail, unfinished, old, empty and other files), the
// update check, version order, and a mock Premium download (the test plays api.nexusmods.com and the CDN through
// nexus_set_test_base: a 403 is "not premium"; a link list gives the file's bytes, no .part, and no token sent to the
// CDN), plus the streaming download's limits. Tokens are kept in memory (nexus_auth_test_memory_store). Prints FAIL
// lines; exit code = failures. MinGW, from cogbox\:
//   g++ -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DCOGBOX_FAB_NEXUS -DCOGBOX_NEXUS_TESTS
//       -Isrc -Ithird_party test/nexus_download_test.cpp src/nexus_download.cpp src/nexus_api.cpp src/nexus_auth.cpp
//       src/nexus_net.cpp -static-libgcc -static-libstdc++ -lwinhttp -lws2_32 -lbcrypt -ladvapi32 -lshell32 -lole32 -luuid
//       -o nexus_download_test.exe
#include "../src/nexus_auth.h"
#include "../src/nexus_download.h"
#include "../src/nexus_net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static std::wstring W(const std::string &s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static bool exists(const std::string &p) { return GetFileAttributesW(W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string read_all(const std::string &p)
{
    HANDLE f = CreateFileW(W(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) s.append(buf, n);
    CloseHandle(f);
    return s;
}
// a file of `bytes`, modified at the Unix time `at`
static void make_file(const std::string &p, const std::string &bytes, int64_t at)
{
    HANDLE f = CreateFileW(W(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    if (!bytes.empty()) WriteFile(f, bytes.data(), (DWORD)bytes.size(), &n, nullptr);
    const uint64_t t = ((uint64_t)(at + 11644473600ll)) * 10000000ull;
    FILETIME ft{ (DWORD)t, (DWORD)(t >> 32) };
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}
static void remove_tree(const std::string &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = W(dir) + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char buf[1024];
                WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
                remove_tree(buf);
            } else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(W(dir).c_str());
}
static std::string temp_dir(const char *name)
{
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, t, -1, buf, sizeof buf, nullptr, nullptr);
    std::string d = std::string(buf) + name + "-" + std::to_string(GetCurrentProcessId()) + " \xC3\xA9";   // (a non-ASCII name)
    remove_tree(d);
    CreateDirectoryW(W(d).c_str(), nullptr);
    return d;
}
static std::string leaf(const std::optional<std::string> &p)
{
    if (!p) return "(none)";
    const size_t at = p->find_last_of('\\');
    return at == std::string::npos ? *p : p->substr(at + 1);
}

static void test_find_download()
{
    const std::string d = temp_dir("cogbox-nexus-pickup");
    const int64_t now = (int64_t)time(nullptr);
    NexusPickup p;
    p.expect_name = "Lancer Pack-27-1-2-1787649366.zip";
    p.started_at = now;

    // nothing there yet
    CHECK(!nexus_find_download(d, p));
    // an unfinished download, an older file of the same name, an empty placeholder and another name: not it
    make_file(d + "\\Lancer Pack-27-1-2-1787649366.zip.crdownload", "partial", now);
    make_file(d + "\\Lancer Pack-27-1-2-1787649366 (2).zip.part", "partial", now);
    make_file(d + "\\Other Mod-31-1-0-1787000000.zip", "zip", now);
    make_file(d + "\\Lancer Pack-27-1-2-1787649366 (3).zip", "", now);
    CHECK(!nexus_find_download(d, p));
    make_file(d + "\\Lancer Pack-27-1-2-1787649366.zip", "old", now - 3600);   // downloaded an hour before the pickup
    CHECK(!nexus_find_download(d, p));

    // the exact name (any case), modified after the pickup started
    DeleteFileW(W(d + "\\Lancer Pack-27-1-2-1787649366.zip").c_str());
    make_file(d + "\\LANCER pack-27-1-2-1787649366.zip", "new", now + 1);
    CHECK(leaf(nexus_find_download(d, p)) == "LANCER pack-27-1-2-1787649366.zip");
    // a " (1)" copy, newer: it wins
    make_file(d + "\\Lancer Pack-27-1-2-1787649366 (1).zip", "copy", now + 5);
    CHECK(leaf(nexus_find_download(d, p)) == "Lancer Pack-27-1-2-1787649366 (1).zip");
    // a copy with another extension or a longer name is not a copy
    make_file(d + "\\Lancer Pack-27-1-2-1787649366 (4).7z", "x", now + 9);
    make_file(d + "\\Lancer Pack-27-1-2-1787649366 extra.zip", "x", now + 9);
    CHECK(leaf(nexus_find_download(d, p)) == "Lancer Pack-27-1-2-1787649366 (1).zip");
    // within 60 s before the start counts (a download finished just before the pickup began)
    {
        const std::string e = temp_dir("cogbox-nexus-pickup2");
        make_file(e + "\\Lancer Pack-27-1-2-1787649366.zip", "x", now - 30);
        CHECK(leaf(nexus_find_download(e, p)) == "Lancer Pack-27-1-2-1787649366.zip");
        remove_tree(e);
    }

    // the tail: the archive's own name is unknown, the -<mod>-<version>-<upload time> end is not
    {
        const std::string e = temp_dir("cogbox-nexus-pickup3");
        NexusFile f;
        f.file_id = 56;
        f.version = "1.2";
        f.uploaded_at = "2026-08-25T00:36:06Z";
        const std::string tail = nexus_download_tail(27, f);
        CHECK(tail == "-27-1-2-1787618166");
        NexusPickup t;
        t.expect_name = "Lancer Pack.zip";
        t.expect_tail = tail;
        t.started_at = now;
        make_file(e + "\\SomeArchive-27-1-2-1787618166.zip.crdownload", "x", now);
        CHECK(!nexus_find_download(e, t));
        make_file(e + "\\SomeArchive-27-1-2-1787618166 (1).zip", "x", now);
        CHECK(leaf(nexus_find_download(e, t)) == "SomeArchive-27-1-2-1787618166 (1).zip");
        make_file(e + "\\SomeArchive-28-1-2-1787618166.zip", "x", now + 9);   // another mod's id
        CHECK(leaf(nexus_find_download(e, t)) == "SomeArchive-27-1-2-1787618166 (1).zip");
        NexusFile undated;
        undated.version = "1";
        CHECK(nexus_download_tail(27, undated).empty());
        remove_tree(e);
    }

    // hostile names match nothing (no wildcard or path reaches FindFirstFile)
    for (const char *bad : { "*", "*.zip", "..\\x.zip", "a/b.zip", "C:x.zip", "a?.zip", "a<.zip", "", "." }) {
        NexusPickup h;
        h.expect_name = bad;
        h.started_at = now;
        CHECK(!nexus_find_download(d, h));
    }
    NexusPickup h;
    h.expect_tail = "*";
    h.started_at = now;
    CHECK(!nexus_find_download(d, h));
    CHECK(!nexus_find_download("", p));
    CHECK(!nexus_find_download(d + "\\missing", p));
    remove_tree(d);

    CHECK(!nexus_downloads_folder().empty());
}

static NexusFile file(int64_t id, const char *name, const char *version, const char *category, const char *uploaded)
{
    NexusFile f;
    f.file_id = id;
    f.name = name;
    f.version = version;
    f.category = category;
    f.uploaded_at = uploaded;
    return f;
}

static void test_find_update()
{
    const std::vector<NexusFile> files = {
        file(10, "Lancer Pack 1.0", "1.0", "OLD_VERSION", "2026-01-01T00:00:00Z"),
        file(11, "Lancer Pack 1.1", "1.1", "MAIN", "2026-02-01T00:00:00Z"),
        file(12, "Optional Skins", "2.0", "OPTIONAL", "2026-03-01T00:00:00Z"),
    };
    // installed 1.0 (now OLD_VERSION): the newer MAIN file is the update
    auto u = nexus_find_update(files, 10, "1.0");
    CHECK(u && u->available && u->file.file_id == 11);
    // installed the MAIN file itself: no update (the OPTIONAL file is newer but another file)
    u = nexus_find_update(files, 11, "1.1");
    CHECK(u && !u->available);
    // an OPTIONAL file is an update of an OPTIONAL install with the same name stem
    std::vector<NexusFile> opt = files;
    opt.push_back(file(13, "Optional Skins v2.1", "2.1", "OPTIONAL", "2026-04-01T00:00:00Z"));
    u = nexus_find_update(opt, 12, "2.0");
    CHECK(u && u->available && u->file.file_id == 13);
    // ... but not for the MAIN install: another stem
    u = nexus_find_update(opt, 11, "1.1");
    CHECK(u && !u->available);
    // a MAIN file uploaded later with the same version string counts (re-uploaded fix); an older version doesn't
    std::vector<NexusFile> later = { file(20, "Pack", "1.0", "MAIN", "2026-01-01T00:00:00Z"),
                                     file(21, "Pack", "1.0", "MAIN", "2026-05-01T00:00:00Z"),
                                     file(22, "Pack", "0.9", "MAIN", "2026-06-01T00:00:00Z") };
    u = nexus_find_update(later, 20, "1.0");
    CHECK(u && u->available && u->file.file_id == 21);
    // the installed file gone from the list (deleted): a higher-version MAIN file still counts; ARCHIVED never does
    std::vector<NexusFile> gone = { file(31, "Pack", "1.3", "ARCHIVED", "2026-07-01T00:00:00Z"),
                                    file(32, "Pack", "1.2", "MAIN", "2026-06-01T00:00:00Z") };
    u = nexus_find_update(gone, 30, "1.1");
    CHECK(u && u->available && u->file.file_id == 32);
    u = nexus_find_update(gone, 30, "1.2");
    CHECK(u && !u->available);
    // no files: can't say
    CHECK(!nexus_find_update({}, 1, "1.0"));
    // the newest of several
    std::vector<NexusFile> many = { file(40, "Pack", "1.0", "MAIN", "2026-01-01T00:00:00Z"),
                                    file(41, "Pack", "1.1", "MAIN", "2026-02-01T00:00:00Z"),
                                    file(42, "Pack", "1.2", "MAIN", "2026-03-01T00:00:00Z") };
    u = nexus_find_update(many, 40, "1.0");
    CHECK(u && u->available && u->file.file_id == 42);

    CHECK(nexus_version_compare("1.10", "1.9") > 0);
    CHECK(nexus_version_compare("v2", "2.0") == 0);
    CHECK(nexus_version_compare("1.0.1", "1.0") > 0);
    CHECK(nexus_version_compare("0.9", "1.0") < 0);
    CHECK(nexus_version_compare("beta", "1.0") == 0);
    CHECK(nexus_version_compare("99999999999999999999999", "1") > 0);   // (huge numbers clamp, no overflow)
}

// A loopback HTTP server for several requests in turn: each answer chosen by the request's path.
struct Server {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::vector<std::string> requests;
    std::thread th;
    std::function<std::string(const std::string &path)> answer;
    bool start(int count)
    {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        int len = sizeof a;
        getsockname(ls, (sockaddr *)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this, count] {
            for (int i = 0; i < count; i++) {
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                std::string req;
                char buf[8192];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    int n = recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                }
                requests.push_back(req);
                const size_t sp = req.find(' '), sp2 = req.find(' ', sp + 1);
                const std::string ans = answer(sp == std::string::npos || sp2 == std::string::npos ? "" : req.substr(sp + 1, sp2 - sp - 1));
                size_t at = 0;
                while (at < ans.size()) {
                    int n = send(c, ans.data() + at, (int)std::min<size_t>(ans.size() - at, 1 << 16), 0);
                    if (n <= 0) break;
                    at += n;
                }
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        });
        return true;
    }
    void stop()
    {
        if (th.joinable()) th.join();
        closesocket(ls);
    }
};
static std::string http(int status, const std::string &body)
{
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

static void test_premium()
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    nexus_auth_test_memory_store(true);
    const std::string d = temp_dir("cogbox-nexus-dl");
    const std::string zip = d + "\\mod.zip";
    std::string bytes;
    for (int i = 0; i < 300000; i++) bytes += (char)(i * 7 + (i >> 9));   // (binary, over one read buffer)

    // nobody signed in: "not premium", nothing asked
    nexus_delete_tokens();
    {
        FabProgress p;
        CHECK(nexus_try_premium_download("gearsofwar3", 27, 56, zip, p) == "not premium");
    }
    NexusTokens t;
    t.access = "test-access-token";
    t.refresh = "test-refresh";
    t.expires_at = (int64_t)time(nullptr) + 3600;
    CHECK(nexus_store_tokens(t));

    // the download link refused (403: not Premium): "not premium", no file
    {
        Server s;
        s.answer = [](const std::string &) { return http(403, R"({"code":403,"message":"You don't have permission"})"); };
        CHECK(s.start(1));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        FabProgress p;
        CHECK(nexus_try_premium_download("gearsofwar3", 27, 56, zip, p) == "not premium");
        s.stop();
        CHECK(s.requests.size() == 1 && contains(s.requests[0], "GET /v1/games/gearsofwar3/mods/27/files/56/download_link.json"));
        CHECK(s.requests.size() == 1 && contains(s.requests[0], "Authorization: Bearer test-access-token\r\n"));
        CHECK(!exists(zip) && !exists(zip + ".part"));
    }
    // 401 too
    {
        Server s;
        s.answer = [](const std::string &) { return http(401, "{}"); };
        CHECK(s.start(1));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        FabProgress p;
        CHECK(nexus_try_premium_download("gearsofwar3", 27, 56, zip, p) == "not premium");
        s.stop();
    }
    // links: the first allowed one is streamed (no token to the CDN), the bytes are right, no .part is left
    {
        Server s;
        s.answer = [&](const std::string &path) {
            if (path.rfind("/v1/", 0) == 0)
                return http(200, R"([{"name":"Evil","URI":"http://cf-files.nexus-cdn.com/evil.zip"},)"
                                 R"({"name":"CDN","short_name":"CDN","URI":"https://cf-files.nexus-cdn.com/files/27/mod.zip?md5=x&expires=1"}])");
            if (path.rfind("/files/27/mod.zip", 0) == 0) return http(200, bytes);
            return http(404, "");
        };
        CHECK(s.start(2));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        FabProgress p;
        const std::string e = nexus_try_premium_download("gearsofwar3", 27, 56, zip, p);
        s.stop();
        CHECK(e.empty());
        CHECK(read_all(zip) == bytes);
        CHECK(!exists(zip + ".part"));
        CHECK(p.done == (int64_t)bytes.size() && p.total == (int64_t)bytes.size());
        CHECK(s.requests.size() == 2);
        if (s.requests.size() == 2) {
            CHECK(contains(s.requests[1], "GET /files/27/mod.zip?md5=x&expires=1 HTTP/1.1"));
            CHECK(!contains(s.requests[1], "Authorization"));
            CHECK(!contains(s.requests[1], "test-access-token"));
            CHECK(contains(s.requests[1], "Application-Name: COGBox\r\n"));
        }
    }
    // a CDN failure: a problem without the address, no .part, and the earlier file stays
    {
        Server s;
        s.answer = [&](const std::string &path) {
            if (path.rfind("/v1/", 0) == 0) return http(200, R"([{"URI":"https://cf-files.nexus-cdn.com/files/27/gone.zip"}])");
            return http(410, "gone");
        };
        CHECK(s.start(2));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        FabProgress p;
        const std::string e = nexus_try_premium_download("gearsofwar3", 27, 56, zip, p);
        s.stop();
        CHECK(!e.empty() && e != "not premium" && !contains(e, "nexus-cdn") && !contains(e, "127.0.0.1"));
        CHECK(!exists(zip + ".part") && read_all(zip) == bytes);
    }
    // no links / not a list: a problem; a 404 for the file
    {
        Server s;
        s.answer = [](const std::string &) { return http(200, R"({"not":"a list"})"); };
        CHECK(s.start(1));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        FabProgress p;
        const std::string e = nexus_try_premium_download("gearsofwar3", 27, 56, zip, p);
        s.stop();
        CHECK(!e.empty() && e != "not premium");
    }
    // cancelled before it starts: nothing asked
    {
        FabProgress p;
        p.cancel = true;
        CHECK(nexus_try_premium_download("gearsofwar3", 27, 56, zip, p) == "cancelled");
    }
    // a bad domain or id: refused before any request
    {
        FabProgress p;
        CHECK(!nexus_try_premium_download("../evil", 27, 56, zip, p).empty());
        CHECK(!nexus_try_premium_download("gearsofwar3", 0, 56, zip, p).empty());
    }

    // the streaming GET itself: a cut-off answer (Content-Length larger than sent), the size cap, and a token kept
    // for api.nexusmods.com only
    {
        Server s;
        s.answer = [](const std::string &) { return std::string("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\nshort"); };
        CHECK(s.start(1));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        std::atomic<bool> cancel{ false };
        NexusResponse r = nexus_download("https://cf-files.nexus-cdn.com/x.zip", d + "\\cut.zip", {}, &cancel);
        s.stop();
        CHECK(!r.error.empty() && !exists(d + "\\cut.zip") && !exists(d + "\\cut.zip.part"));
    }
    {
        Server s;
        s.answer = [](const std::string &) { return http(200, std::string(5000, 'z')); };
        CHECK(s.start(1));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        std::atomic<bool> cancel{ false };
        NexusResponse r = nexus_download("https://cf-files.nexus-cdn.com/x.zip", d + "\\big.zip", {}, &cancel, nullptr, nullptr, 4096);
        s.stop();
        CHECK(!r.error.empty() && !exists(d + "\\big.zip") && !exists(d + "\\big.zip.part"));
    }
    {
        Server s;
        s.answer = [](const std::string &) { return http(200, "ok"); };
        CHECK(s.start(2));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(s.port));
        std::atomic<bool> cancel{ false };
        NexusResponse r1 = nexus_download("https://api.nexusmods.com/v1/x", d + "\\a.bin", { { "Authorization", "Bearer k" } }, &cancel);
        NexusResponse r2 = nexus_download("https://staticdelivery.nexusmods.com/x", d + "\\b.bin", { { "authorization", "Bearer k" } }, &cancel);
        s.stop();
        CHECK(r1.error.empty() && r2.error.empty() && s.requests.size() == 2);
        if (s.requests.size() == 2) {
            CHECK(contains(s.requests[0], "Authorization: Bearer k\r\n"));
            CHECK(!contains(s.requests[1], "Bearer k"));
        }
        // not an allowed address: refused, nothing written
        NexusResponse r3 = nexus_download("https://evil.com/x.zip", d + "\\c.bin", {}, &cancel);
        CHECK(!r3.error.empty() && !contains(r3.error, "evil") && !exists(d + "\\c.bin"));
        cancel = true;
    }

    nexus_set_test_base("");
    nexus_delete_tokens();
    nexus_auth_test_memory_store(false);
    remove_tree(d);
    WSACleanup();
}

int main()
{
    test_find_download();
    test_find_update();
    test_premium();
    printf("%d failure(s)\n", g_failures);
    return g_failures;
}
