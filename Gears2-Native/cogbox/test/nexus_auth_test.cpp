// CogBox - test of signing in to Nexus Mods (COGBOX_FAB_NEXUS): PKCE (the RFC 7636 vector), the state, the authorize
// URL, the callback parser, a busy port 28654, a whole mock sign-in (the test plays the browser and, through
// nexus_set_test_base, users.nexusmods.com), the refresh (near expiry, refused, unreachable), sign-out and the chunked
// Credential Manager storage (test-only target COGBox/NexusModsTest, deleted after). Never opens a browser and never
// touches the player's COGBox/NexusMods. Prints FAIL lines; exit code = failures. MinGW, from cogbox\:
//   g++ -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DCOGBOX_FAB_NEXUS -DCOGBOX_NEXUS_TESTS
//       -Isrc -Ithird_party test/nexus_auth_test.cpp src/nexus_auth.cpp src/nexus_net.cpp
//       -static-libgcc -static-libstdc++ -lwinhttp -lws2_32 -lbcrypt -ladvapi32 -lshell32 -o nexus_auth_test.exe
#include "../src/nexus_auth.h"
#include "../src/nexus_net.h"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincred.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static std::string pct_decode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

// key=value from a query string or a form body ("" if absent)
static std::string param(const std::string &qs, const std::string &key)
{
    size_t at = 0;
    while (at <= qs.size()) {
        size_t amp = qs.find('&', at);
        std::string kv = qs.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return pct_decode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        at = amp + 1;
    }
    return "";
}

static std::string http_answer(int status, const std::string &body, const char *type = "application/json")
{
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

static std::string b64url(const std::string &in)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63];
    }
    return o;
}

// A loopback HTTP server that answers one connection at a time with handler(request), until stopped: the mock
// users.nexusmods.com (nexus_set_test_base points here).
struct MockServer {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::thread th;
    std::mutex mu;
    std::vector<std::string> requests;
    std::atomic<bool> stopping{ false };
    bool start(std::function<std::string(const std::string &)> handler)
    {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        int len = sizeof a;
        getsockname(ls, (sockaddr *)&a, &len);
        port = ntohs(a.sin_port);
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(port));
        th = std::thread([this, handler] {
            while (!stopping) {
                fd_set fs;
                FD_ZERO(&fs);
                FD_SET(ls, &fs);
                timeval tv{ 0, 50000 };
                if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(ls, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                std::string req;
                char buf[8192];
                size_t want = std::string::npos;
                for (;;) {
                    int n = recv(c, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    req.append(buf, n);
                    size_t he = req.find("\r\n\r\n");
                    if (he != std::string::npos && want == std::string::npos) {
                        size_t cl = req.find("Content-Length: ");
                        want = he + 4 + (cl != std::string::npos && cl < he ? strtoul(req.c_str() + cl + 16, nullptr, 10) : 0);
                    }
                    if (want != std::string::npos && req.size() >= want) break;
                }
                {
                    std::lock_guard<std::mutex> lk(mu);
                    requests.push_back(req);
                }
                const std::string ans = handler(req);
                send(c, ans.data(), (int)ans.size(), 0);
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        });
        return true;
    }
    void stop()
    {
        stopping = true;
        if (th.joinable()) th.join();
        closesocket(ls);
        nexus_set_test_base("");
    }
    std::vector<std::string> got()
    {
        std::lock_guard<std::mutex> lk(mu);
        return requests;
    }
};

static std::string body_of(const std::string &req)
{
    size_t he = req.find("\r\n\r\n");
    return he == std::string::npos ? "" : req.substr(he + 4);
}

// The browser: GET target from the listener on 127.0.0.1:28654 (retrying the connect briefly), the whole answer.
static std::string browser_get(const std::string &target)
{
    for (int attempt = 0; attempt < 40; attempt++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(kNexusCallbackPort);
        if (connect(s, (sockaddr *)&a, sizeof a) != 0) {
            closesocket(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        DWORD to = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        const std::string req = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:28654\r\nConnection: close\r\n\r\n";
        send(s, req.data(), (int)req.size(), 0);
        std::string ans;
        char buf[4096];
        size_t want = std::string::npos;
        for (;;) {   // like a browser with Connection: close: read the answer (Content-Length), then close first
            int n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) break;
            ans.append(buf, n);
            size_t he = ans.find("\r\n\r\n");
            if (he != std::string::npos && want == std::string::npos) {
                size_t cl = ans.find("Content-Length: ");
                want = he + 4 + (cl != std::string::npos && cl < he ? strtoul(ans.c_str() + cl + 16, nullptr, 10) : 0);
            }
            if (want != std::string::npos && ans.size() >= want) break;
        }
        closesocket(s);
        return ans;
    }
    return "";
}

static void set_client_id(const char *id) { _putenv_s("COGBOX_NEXUS_CLIENT_ID", id); }

static void test_pkce()
{
    // RFC 7636 Appendix B (the verifier is base64url of the RFC's 32 octets)
    CHECK(pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    const std::string v = pkce_verifier(), v2 = pkce_verifier();
    CHECK(v.size() == 64);
    CHECK(v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
    CHECK(v != v2);
    const std::string c = pkce_challenge(v);
    CHECK(c.size() == 43 && c.find('=') == std::string::npos && c.find('+') == std::string::npos && c.find('/') == std::string::npos);
    const std::string s1 = oauth_state(), s2 = oauth_state();
    CHECK(s1 != s2);
    CHECK(s1.size() >= 43);   // 256 bits in base64url
    CHECK(s1.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
}

static void test_authorize_url()
{
    const std::string u = nexus_authorize_url("my-client.1", "CHAL_-x", "STATE-y_z");
    CHECK(u.rfind("https://users.nexusmods.com/oauth/authorize?", 0) == 0);
    CHECK(contains(u, "response_type=code"));
    CHECK(contains(u, "client_id=my-client.1"));
    CHECK(contains(u, "redirect_uri=http%3A%2F%2F127.0.0.1%3A28654%2Fcallback"));
    CHECK(contains(u, "scope=openid%20public"));
    CHECK(contains(u, "code_challenge=CHAL_-x"));
    CHECK(contains(u, "code_challenge_method=S256"));
    CHECK(contains(u, "state=STATE-y_z"));
    CHECK(nexus_host_allowed(u));
}

static void test_callback_parse()
{
    const std::string S = "abcDEF-_123";
    auto head = [](const std::string &line) { return line + " HTTP/1.1\r\nHost: 127.0.0.1:28654\r\n\r\n"; };
    NexusCallback r = nexus_parse_callback(head("GET /callback?code=the%2Fcode&state=" + S), S);
    CHECK(r.kind == NexusCallback::Code && r.code == "the/code");
    r = nexus_parse_callback(head("GET /callback?state=" + S + "&code=c2"), S);
    CHECK(r.kind == NexusCallback::Code && r.code == "c2");
    r = nexus_parse_callback(head("GET /callback?error=access_denied&error_description=no&state=" + S), S);
    CHECK(r.kind == NexusCallback::Failed && r.problem == "Sign-in was cancelled on Nexus Mods.");
    r = nexus_parse_callback(head("GET /callback?error=server_error&state=" + S), S);
    CHECK(r.kind == NexusCallback::Failed && !r.problem.empty());
    // wrong or missing state: not our sign-in (a stale tab or another page): keep waiting
    r = nexus_parse_callback(head("GET /callback?code=c&state=abcDEF-_124"), S);
    CHECK(r.kind == NexusCallback::BadState && r.code.empty());
    r = nexus_parse_callback(head("GET /callback?code=c&state=" + S + "x"), S);
    CHECK(r.kind == NexusCallback::BadState);
    r = nexus_parse_callback(head("GET /callback?code=c"), S);
    CHECK(r.kind == NexusCallback::BadState);
    r = nexus_parse_callback(head("GET /callback?code=c&state="), "");
    CHECK(r.kind == NexusCallback::BadState);   // an empty expected state never matches
    // our state but no code
    r = nexus_parse_callback(head("GET /callback?state=" + S), S);
    CHECK(r.kind == NexusCallback::Failed && !r.problem.empty());
    r = nexus_parse_callback(head("GET /callback?code=&state=" + S), S);
    CHECK(r.kind == NexusCallback::Failed);
    // other paths and methods: 404
    for (const char *line : { "GET /favicon.ico", "GET /", "GET /callbackx?code=c&state=abcDEF-_123", "POST /callback?code=c&state=abcDEF-_123",
                              "GET /other/callback?code=c&state=abcDEF-_123", "garbage" })
        CHECK(nexus_parse_callback(head(line), S).kind == NexusCallback::NotFound);
    CHECK(nexus_parse_callback("", S).kind == NexusCallback::NotFound);
    // the problem text never carries the code or the state
    r = nexus_parse_callback(head("GET /callback?error=%3Cscript%3E&state=" + S), S);
    CHECK(r.kind == NexusCallback::Failed && !contains(r.problem, "script") && !contains(r.problem, S.c_str()));
}

static void test_name_from_userinfo()
{
    CHECK(nexus_name_from_userinfo(R"({"sub":"1","name":"Marcus"})", "", "") == "Marcus");
    CHECK(nexus_name_from_userinfo(R"({"sub":"1","preferred_username":"Dom"})", "", "") == "Dom");
    const std::string jwt = b64url(R"({"alg":"none"})") + "." + b64url(R"({"sub":"7","user":{"id":7,"username":"Baird"}})") + ".sig";
    CHECK(nexus_name_from_userinfo("not json", jwt, "") == "Baird");
    CHECK(nexus_name_from_userinfo("{}", "", jwt) == "Baird");
    CHECK(nexus_name_from_userinfo(R"({"name":5})", "x.y.z", "...") == "");
    CHECK(nexus_name_from_userinfo("", "", "") == "");
}

static void test_no_client_id()
{
    set_client_id("");
    CHECK(nexus_client_id().empty());
    set_client_id("bad id!");
    CHECK(nexus_client_id().empty());
    set_client_id("cogbox-test_1.0");
    CHECK(nexus_client_id() == "cogbox-test_1.0");
    set_client_id("");
    bool opened = false;
    nexus_auth_test_browser_hook([&](const std::string &) { opened = true; });
    std::atomic<bool> cancel{ false };
    NexusAccount acc;
    const std::string p = nexus_sign_in(cancel, &acc);
    CHECK(!p.empty() && !opened && !acc.signed_in);
}

static void test_busy_port()
{
    set_client_id("test-client");
    SOCKET hold = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(kNexusCallbackPort);
    const bool held = bind(hold, (sockaddr *)&a, sizeof a) == 0 && listen(hold, 1) == 0;
    if (!held) printf("note: port 28654 was already in use before the test\n");
    bool opened = false;
    nexus_auth_test_browser_hook([&](const std::string &) { opened = true; });
    std::atomic<bool> cancel{ false };
    NexusAccount acc;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string p = nexus_sign_in(cancel, &acc);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(p == "Close whatever is using port 28654 and try again.");
    CHECK(ms < 2000);
    CHECK(!opened);
    closesocket(hold);
}

static void test_cancel()
{
    set_client_id("test-client");
    std::atomic<bool> cancel{ false };
    nexus_auth_test_browser_hook([&](const std::string &) { cancel = true; });
    NexusAccount acc;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string p = nexus_sign_in(cancel, &acc);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!p.empty() && ms < 3000 && !acc.signed_in);
}

static void test_mock_flow()
{
    set_client_id("test-client");
    nexus_delete_tokens();
    const std::string access = b64url(R"({"alg":"none"})") + "." + b64url(R"({"user":{"username":"FromJwt"}})") + ".s";
    MockServer api;
    CHECK(api.start([&](const std::string &req) {
        if (req.rfind("POST /oauth/token ", 0) == 0)
            return http_answer(200, json{ { "access_token", access }, { "refresh_token", "refresh-1" }, { "expires_in", 21600 },
                                          { "id_token", "" }, { "token_type", "Bearer" } }.dump());
        if (req.rfind("GET /oauth/userinfo ", 0) == 0) return http_answer(200, R"({"sub":"42","name":"TestPlayer"})");
        return http_answer(404, "{}");
    }));
    std::string url, a404, abad, aok;
    std::thread browser;
    nexus_auth_test_browser_hook([&](const std::string &u) {
        url = u;
        browser = std::thread([&, u] {
            const std::string q = u.substr(u.find('?') + 1);
            a404 = browser_get("/favicon.ico");
            abad = browser_get("/callback?code=evil&state=not-it");
            aok = browser_get("/callback?code=the-code&state=" + param(q, "state"));
        });
    });
    std::atomic<bool> cancel{ false };
    NexusAccount acc;
    const std::string p = nexus_sign_in(cancel, &acc);
    if (browser.joinable()) browser.join();
    api.stop();
    CHECK(p.empty());
    CHECK(acc.signed_in && acc.name == "TestPlayer");
    CHECK(contains(a404, " 404 "));
    CHECK(contains(abad, " 400 "));
    CHECK(contains(aok, " 200 ") && contains(aok, "You can close this tab and return to COGBox"));
    CHECK(!contains(aok, "the-code"));

    const std::string q = url.substr(url.find('?') + 1);
    const auto reqs = api.got();
    CHECK(reqs.size() == 2);
    if (reqs.size() == 2) {
        const std::string &tok = reqs[0], form = body_of(reqs[0]);
        CHECK(tok.rfind("POST /oauth/token ", 0) == 0);
        CHECK(contains(tok, "Content-Type: application/x-www-form-urlencoded"));
        CHECK(contains(tok, "Application-Name: COGBox"));
        CHECK(param(form, "grant_type") == "authorization_code");
        CHECK(param(form, "code") == "the-code");
        CHECK(param(form, "redirect_uri") == "http://127.0.0.1:28654/callback");
        CHECK(param(form, "client_id") == "test-client");
        const std::string verifier = param(form, "code_verifier");
        CHECK(verifier.size() == 64);
        CHECK(pkce_challenge(verifier) == param(q, "code_challenge"));   // the verifier matches the challenge sent
        CHECK(reqs[1].rfind("GET /oauth/userinfo ", 0) == 0);
        CHECK(contains(reqs[1], ("Authorization: Bearer " + access + "\r\n").c_str()));
    }
    auto t = nexus_load_tokens();
    CHECK(t && t->access == access && t->refresh == "refresh-1");
    CHECK(t && t->expires_at > (int64_t)time(nullptr) + 21000 && t->expires_at <= (int64_t)time(nullptr) + 21600);
    const NexusAccount stored = nexus_account();
    CHECK(stored.signed_in && stored.name == "TestPlayer");
    nexus_delete_tokens();
    CHECK(!nexus_account().signed_in);
}

static void test_denied_flow()
{
    set_client_id("test-client");
    nexus_delete_tokens();
    MockServer api;
    CHECK(api.start([](const std::string &) { return http_answer(500, "{}"); }));
    std::thread browser;
    std::string aok;
    nexus_auth_test_browser_hook([&](const std::string &u) {
        browser = std::thread([&, u] {
            // a connection that never sends anything is dropped after about 2 s and does not block the sign-in
            SOCKET silent = INVALID_SOCKET;
            for (int i = 0; i < 40 && silent == INVALID_SOCKET; i++) {
                silent = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = htons(kNexusCallbackPort);
                if (connect(silent, (sockaddr *)&a, sizeof a) != 0) {
                    closesocket(silent);
                    silent = INVALID_SOCKET;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // (accepted first)
            aok = browser_get("/callback?error=access_denied&state=" + param(u.substr(u.find('?') + 1), "state"));
            if (silent != INVALID_SOCKET) closesocket(silent);
        });
    });
    std::atomic<bool> cancel{ false };
    NexusAccount acc;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string p = nexus_sign_in(cancel, &acc);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (browser.joinable()) browser.join();
    api.stop();
    CHECK(p == "Sign-in was cancelled on Nexus Mods.");
    CHECK(ms < 4000);   // the silent connection cost at most its 2 s
    CHECK(contains(aok, "You can close this tab and return to COGBox"));
    CHECK(api.got().empty());   // nothing exchanged
    CHECK(!nexus_load_tokens() && !acc.signed_in);
}

static void test_token_refused_flow()
{
    // the token endpoint refuses the code: a problem, nothing stored, the problem names no code or verifier
    set_client_id("test-client");
    nexus_delete_tokens();
    MockServer api;
    CHECK(api.start([](const std::string &) { return http_answer(400, R"({"error":"invalid_grant"})"); }));
    std::thread browser;
    nexus_auth_test_browser_hook([&](const std::string &u) {
        browser = std::thread([&, u] { browser_get("/callback?code=secret-code&state=" + param(u.substr(u.find('?') + 1), "state")); });
    });
    std::atomic<bool> cancel{ false };
    NexusAccount acc;
    const std::string p = nexus_sign_in(cancel, &acc);
    if (browser.joinable()) browser.join();
    api.stop();
    CHECK(!p.empty() && !contains(p, "secret-code") && !contains(p, "invalid_grant"));
    CHECK(!nexus_load_tokens());
}

static void test_refresh()
{
    set_client_id("test-client");
    std::string problem;
    nexus_delete_tokens();
    CHECK(!nexus_access_token(&problem) && !problem.empty());

    // far from expiry: no request (the test base points at a closed port, so a request would fail)
    nexus_set_test_base("http://127.0.0.1:1");
    NexusTokens t;
    t.access = "access-old";
    t.refresh = "refresh-old";
    t.expires_at = (int64_t)time(nullptr) + 3600;
    CHECK(nexus_store_tokens(t));
    problem.clear();
    auto a = nexus_access_token(&problem);
    CHECK(a && *a == "access-old" && problem.empty());

    // 60 s from expiry: refreshed (the refresh token is kept when Nexus does not rotate it)
    t.expires_at = (int64_t)time(nullptr) + 60;
    CHECK(nexus_store_tokens(t));
    {
        MockServer api;
        CHECK(api.start([](const std::string &req) {
            if (req.rfind("POST /oauth/token ", 0) == 0) return http_answer(200, R"({"access_token":"access-new","expires_in":3600})");
            return http_answer(404, "{}");
        }));
        problem.clear();
        a = nexus_access_token(&problem);
        api.stop();
        CHECK(a && *a == "access-new" && problem.empty());
        const auto reqs = api.got();
        CHECK(reqs.size() == 1);
        if (!reqs.empty()) {
            const std::string form = body_of(reqs[0]);
            CHECK(param(form, "grant_type") == "refresh_token");
            CHECK(param(form, "refresh_token") == "refresh-old");
            CHECK(param(form, "client_id") == "test-client");
            CHECK(contains(reqs[0], "Content-Type: application/x-www-form-urlencoded"));
        }
        auto s = nexus_load_tokens();
        CHECK(s && s->access == "access-new" && s->refresh == "refresh-old" && s->expires_at > (int64_t)time(nullptr) + 3000);
    }

    // already expired, rotated refresh token
    t.expires_at = (int64_t)time(nullptr) - 10;
    CHECK(nexus_store_tokens(t));
    {
        MockServer api;
        CHECK(api.start([](const std::string &) {
            return http_answer(200, R"({"access_token":"access-3","refresh_token":"refresh-3","expires_in":3600})");
        }));
        a = nexus_access_token(&problem);
        api.stop();
        auto s = nexus_load_tokens();
        CHECK(a && *a == "access-3" && s && s->refresh == "refresh-3");
    }

    // a 200 without an access token is not a refresh: no stale token comes back
    t.expires_at = (int64_t)time(nullptr) + 60;
    CHECK(nexus_store_tokens(t));
    {
        MockServer api;
        CHECK(api.start([](const std::string &) { return http_answer(200, R"({"nope":1})"); }));
        problem.clear();
        a = nexus_access_token(&problem);
        api.stop();
        CHECK(!a && !problem.empty());
    }

    // Nexus Mods unreachable (503): no token, a problem, the tokens kept for later
    t.expires_at = (int64_t)time(nullptr) + 60;
    CHECK(nexus_store_tokens(t));
    {
        MockServer api;
        CHECK(api.start([](const std::string &) { return http_answer(503, "{}"); }));
        problem.clear();
        a = nexus_access_token(&problem);
        api.stop();
        CHECK(!a && !problem.empty() && problem != "You were signed out of Nexus Mods - sign in again.");
        CHECK(nexus_load_tokens().has_value());
    }

    // refused with 400: signed out, with the message, the tokens gone
    {
        MockServer api;
        CHECK(api.start([](const std::string &) { return http_answer(400, R"({"error":"invalid_grant"})"); }));
        problem.clear();
        a = nexus_access_token(&problem);
        api.stop();
        CHECK(!a);
        CHECK(problem == "You were signed out of Nexus Mods - sign in again.");
        CHECK(!nexus_load_tokens());
        CHECK(!nexus_account().signed_in);
        problem.clear();
        CHECK(!nexus_access_token(&problem));   // and stays signed out (no stale token)
    }

    // two callers at once near expiry: one refresh, both get the new token
    t.expires_at = (int64_t)time(nullptr) + 60;
    CHECK(nexus_store_tokens(t));
    {
        MockServer api;
        CHECK(api.start([](const std::string &req) {
            // a reused refresh token would be refused, like a rotating server
            if (param(body_of(req), "refresh_token") != "refresh-old") return http_answer(400, "{}");
            Sleep(200);
            return http_answer(200, R"({"access_token":"access-r","refresh_token":"refresh-r","expires_in":3600})");
        }));
        std::string p1, p2;
        std::optional<std::string> r1, r2;
        std::thread x([&] { r1 = nexus_access_token(&p1); });
        std::thread y([&] { r2 = nexus_access_token(&p2); });
        x.join();
        y.join();
        api.stop();
        CHECK(r1 && r2 && *r1 == "access-r" && *r2 == "access-r");
        CHECK(api.got().size() == 1);
    }
    nexus_delete_tokens();
}

static void test_sign_out()
{
    set_client_id("test-client");
    NexusTokens t;
    t.access = "access-x";
    t.refresh = "refresh-x";
    t.expires_at = (int64_t)time(nullptr) + 3600;
    CHECK(nexus_store_tokens(t));
    CHECK(nexus_account().signed_in);
    MockServer api;
    CHECK(api.start([](const std::string &) { return http_answer(200, "{}"); }));
    nexus_sign_out();
    api.stop();
    CHECK(!nexus_load_tokens() && !nexus_account().signed_in);
    const auto reqs = api.got();
    CHECK(!reqs.empty() && reqs[0].rfind("POST /oauth/revoke ", 0) == 0);
    if (!reqs.empty()) {
        CHECK(param(body_of(reqs[0]), "client_id") == "test-client");
        CHECK(param(body_of(reqs[0]), "token") == "refresh-x");
    }
    // unreachable revoke: still signed out here
    CHECK(nexus_store_tokens(t));
    nexus_set_test_base("http://127.0.0.1:1");
    nexus_sign_out();
    nexus_set_test_base("");
    CHECK(!nexus_load_tokens());
}

static bool cred_exists(const wchar_t *target, DWORD *size)
{
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c)) return false;
    if (size) *size = c->CredentialBlobSize;
    CredFree(c);
    return true;
}

static void test_chunking()
{
    nexus_auth_test_memory_store(false);   // the real Credential Manager, under the test-only COGBox/NexusModsTest
    NexusTokens t;
    for (int i = 0; i < 5120; i++) t.access += (char)('A' + i % 26);
    t.refresh = "r-" + std::string(300, 'z');
    t.id_token = "id";
    t.expires_at = 1234567890123LL;
    CHECK(nexus_store_tokens(t));
    auto back = nexus_load_tokens();
    CHECK(back && back->access == t.access && back->refresh == t.refresh && back->id_token == t.id_token && back->expires_at == t.expires_at);
    DWORD sz = 0;
    CHECK(cred_exists(L"COGBox/NexusModsTest/0", &sz) && sz <= 2000);
    CHECK(cred_exists(L"COGBox/NexusModsTest/1", &sz) && sz <= 2000);
    CHECK(cred_exists(L"COGBox/NexusModsTest/2", &sz) && sz <= 2000);
    // a shorter token: the extra chunks are deleted
    NexusTokens s;
    s.access = "short";
    s.refresh = "r";
    s.expires_at = 5;
    CHECK(nexus_store_tokens(s));
    back = nexus_load_tokens();
    CHECK(back && back->access == "short" && back->expires_at == 5);
    CHECK(cred_exists(L"COGBox/NexusModsTest/0", nullptr));
    CHECK(!cred_exists(L"COGBox/NexusModsTest/1", nullptr) && !cred_exists(L"COGBox/NexusModsTest/2", nullptr));
    nexus_delete_tokens();
    CHECK(!nexus_load_tokens());
    for (int i = 0; i < 4; i++) {
        const std::wstring n = L"COGBox/NexusModsTest/" + std::to_wstring(i);
        CHECK(!cred_exists(n.c_str(), nullptr));
        CredDeleteW(n.c_str(), CRED_TYPE_GENERIC, 0);   // (left nothing behind either way)
    }
    // chunk 0 deleted by hand (or lost): nexus_delete_tokens still removes the rest
    CHECK(nexus_store_tokens(t));
    CHECK(cred_exists(L"COGBox/NexusModsTest/2", nullptr));
    CredDeleteW(L"COGBox/NexusModsTest/0", CRED_TYPE_GENERIC, 0);
    CHECK(!nexus_load_tokens());
    nexus_delete_tokens();
    for (int i = 0; i < 4; i++) {
        const std::wstring n = L"COGBox/NexusModsTest/" + std::to_wstring(i);
        CHECK(!cred_exists(n.c_str(), nullptr));
        CredDeleteW(n.c_str(), CRED_TYPE_GENERIC, 0);
    }
    nexus_auth_test_memory_store(true);
}

int main()
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    nexus_auth_test_memory_store(true);
    test_pkce();
    test_authorize_url();
    test_callback_parse();
    test_name_from_userinfo();
    test_no_client_id();
    test_busy_port();
    test_cancel();
    test_mock_flow();
    test_denied_flow();
    test_token_refused_flow();
    test_refresh();
    test_sign_out();
    test_chunking();
    nexus_auth_test_browser_hook(nullptr);
    WSACleanup();
    printf("%d failure(s)\n", g_failures);
    return g_failures;
}
