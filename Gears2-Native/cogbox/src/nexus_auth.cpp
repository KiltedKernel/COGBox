// CogBox - signing in to Nexus Mods (see nexus_auth.h). COGBOX_FAB_NEXUS builds only.
// The loopback listener binds 127.0.0.1:28654 only (SO_EXCLUSIVEADDRUSE, so no other program can share the port and
// read the code), takes one connection at a time, reads at most 8 KB of it, and closes on success, cancel or 5 min.
// The stored blob is JSON {access, refresh, id_token, expires_at, name}, split into 2000-byte generic credentials
// (Credential Manager caps one at 2560 bytes).
#ifdef COGBOX_FAB_NEXUS
#include "nexus_auth.h"
#include "nexus_net.h"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincred.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

using json = nlohmann::json;

namespace {
#ifdef COGBOX_NEXUS_TESTS
constexpr const wchar_t *kCredTarget = L"COGBox/NexusModsTest";   // test builds never touch the player's sign-in
#else
constexpr const wchar_t *kCredTarget = L"COGBox/NexusMods";
#endif
constexpr size_t kChunk = 2000;        // bytes per credential (the limit is 2560)
constexpr int kMaxChunks = 32;         // 64 KB: a session is a few KB
constexpr size_t kMaxHead = 8192;      // the most of a browser request the listener reads
constexpr int64_t kRefreshEarly = 300; // refresh 5 min before expiry

constexpr const char *kSignedOut = kNexusSignedOut;
constexpr const char *kBusyPort = "Close whatever is using port 28654 and try again.";
constexpr const char *kDenied = "Sign-in was cancelled on Nexus Mods.";
constexpr const char *kUnreachable = "Nexus Mods could not be reached - try again.";
constexpr const char *kNoClient = "Signing in to Nexus Mods is not available in this build yet.";

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::mutex g_session_mu;   // sign-in's store, the refresh and sign-out: one at a time (a refresh never races)
std::mutex g_store_mu;     // the storage itself
#ifdef COGBOX_NEXUS_TESTS
bool g_memory = false;
std::optional<std::string> g_mem_blob;
std::function<void(const std::string &)> g_browser_hook;
std::mutex g_hook_mu;
#endif

std::string b64url_encode(const unsigned char *p, size_t n)
{
    std::string o;
    o.reserve((n * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8 | p[i + 2];
        o += kB64[v >> 18 & 63]; o += kB64[v >> 12 & 63]; o += kB64[v >> 6 & 63]; o += kB64[v & 63];
    }
    if (n - i == 1) {
        const unsigned v = (unsigned)p[i] << 16;
        o += kB64[v >> 18 & 63]; o += kB64[v >> 12 & 63];
    } else if (n - i == 2) {
        const unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8;
        o += kB64[v >> 18 & 63]; o += kB64[v >> 12 & 63]; o += kB64[v >> 6 & 63];
    }
    return o;
}

// base64url or base64, padding optional. False for any other character.
bool b64_decode(const std::string &in, std::string *out)
{
    out->clear();
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *out += (char)((acc >> bits) & 0xff);
        }
    }
    return true;
}

bool random_bytes(unsigned char *p, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string random_b64(size_t n)
{
    std::vector<unsigned char> b(n);
    if (!random_bytes(b.data(), n)) return "";
    std::string s = b64url_encode(b.data(), n);
    SecureZeroMemory(b.data(), n);
    return s;
}

bool sha256(const std::string &data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    DWORD objlen = 0, got = 0;
    std::vector<unsigned char> obj;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objlen, sizeof objlen, &got, 0))) {
        obj.resize(objlen ? objlen : 1);
        ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, obj.data(), objlen, nullptr, 0, 0)) &&
             BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0)) &&
             BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0));
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string pct_encode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string pct_decode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            o += (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            o += s[i] == '+' ? ' ' : s[i];
        }
    }
    return o;
}

// The first value of `key` in a query string; false when the key is absent.
bool query_value(const std::string &qs, const std::string &key, std::string *out)
{
    size_t at = 0;
    for (;;) {
        const size_t amp = qs.find('&', at);
        const std::string kv = qs.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        const size_t eq = kv.find('=');
        if (pct_decode(kv.substr(0, eq)) == key) {
            *out = eq == std::string::npos ? std::string() : pct_decode(kv.substr(eq + 1));
            return true;
        }
        if (amp == std::string::npos) return false;
        at = amp + 1;
    }
}

bool equal_ct(const std::string &a, const std::string &b)   // constant time for a given length
{
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned d = a.size() != b.size();
    for (size_t i = 0; i < n; i++)
        d |= (unsigned)(unsigned char)(i < a.size() ? a[i] : 0) ^ (unsigned)(unsigned char)(i < b.size() ? b[i] : 0);
    return d == 0;
}

int64_t now_unix() { return (int64_t)time(nullptr); }

// A display name: no control characters, at most 64 bytes, not cut inside a UTF-8 character.
std::string clean_name(const std::string &s)
{
    std::string o;
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7f) o += (char)c;
    if (o.size() > 64) {
        size_t n = 64;
        while (n > 0 && ((unsigned char)o[n] & 0xc0) == 0x80) n--;
        o.resize(n);
    }
    return o;
}

json jwt_payload(const std::string &jwt)
{
    const size_t a = jwt.find('.');
    const size_t b = a == std::string::npos ? std::string::npos : jwt.find('.', a + 1);
    if (b == std::string::npos || b - a - 1 > 16384) return json();
    std::string raw;
    if (!b64_decode(jwt.substr(a + 1, b - a - 1), &raw)) return json();
    json j = json::parse(raw, nullptr, false);
    return j.is_object() ? j : json();
}

int64_t num_of(const json &v, int64_t dflt)
{
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return d == d && d > -9e15 && d < 9e15 ? (int64_t)d : dflt;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (!s.empty() && s.size() < 16 && s.find_first_not_of("0123456789") == std::string::npos) return atoll(s.c_str());
    }
    return dflt;
}

std::string str_of(const json &j, const char *k)
{
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A token endpoint answer into t (refresh / id_token left as they are when absent). False without an access token.
bool parse_token_answer(const std::string &body, NexusTokens *t)
{
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return false;
    const std::string access = str_of(j, "access_token");
    if (access.empty() || access.size() > 16384) return false;
    t->access = access;
    const std::string refresh = str_of(j, "refresh_token"), id = str_of(j, "id_token");
    if (!refresh.empty() && refresh.size() <= 16384) t->refresh = refresh;
    if (!id.empty() && id.size() <= 16384) t->id_token = id;
    int64_t life = j.contains("expires_in") ? num_of(j["expires_in"], -1) : -1;
    if (life < 0) {   // no expires_in: the JWT's exp, else an hour
        const json p = jwt_payload(access);
        const int64_t exp = p.is_object() && p.contains("exp") ? num_of(p["exp"], 0) : 0;
        life = exp > now_unix() ? exp - now_unix() : 3600;
    }
    if (life > 30LL * 86400) life = 30LL * 86400;
    t->expires_at = now_unix() + life;
    return true;
}

const std::vector<std::pair<std::string, std::string>> &form_headers()
{
    static const std::vector<std::pair<std::string, std::string>> h = { { "Content-Type", "application/x-www-form-urlencoded" },
                                                                        { "Accept", "application/json" } };
    return h;
}

// ---- storage --------------------------------------------------------------------------------------------------------

std::wstring chunk_name(int i) { return std::wstring(kCredTarget) + L"/" + std::to_wstring(i); }

std::optional<std::string> read_blob()   // (g_store_mu held)
{
#ifdef COGBOX_NEXUS_TESTS
    if (g_memory) return g_mem_blob;
#endif
    std::string blob;
    int i = 0;
    for (; i < kMaxChunks; i++) {
        PCREDENTIALW c = nullptr;
        if (!CredReadW(chunk_name(i).c_str(), CRED_TYPE_GENERIC, 0, &c)) break;
        if (c->CredentialBlobSize > 0 && c->CredentialBlob) blob.append((const char *)c->CredentialBlob, c->CredentialBlobSize);
        if (c->CredentialBlob) SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
        CredFree(c);
    }
    if (i == 0) return std::nullopt;
    return blob;
}

void delete_chunks_from(int first)   // (g_store_mu held)
{
    for (int i = first; i < kMaxChunks; i++)   // every slot: one missing chunk (deleted by hand) must not hide the rest
        CredDeleteW(chunk_name(i).c_str(), CRED_TYPE_GENERIC, 0);
}

bool write_blob(const std::string &blob)   // (g_store_mu held)
{
#ifdef COGBOX_NEXUS_TESTS
    if (g_memory) {
        g_mem_blob = blob;
        return true;
    }
#endif
    const size_t n = (blob.size() + kChunk - 1) / kChunk;
    if (n == 0 || n > (size_t)kMaxChunks) return false;
    static wchar_t user[] = L"COGBox";
    for (size_t i = 0; i < n; i++) {
        const std::wstring name = chunk_name((int)i);
        const std::string part = blob.substr(i * kChunk, kChunk);
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = (LPWSTR)name.c_str();
        c.CredentialBlobSize = (DWORD)part.size();
        c.CredentialBlob = (LPBYTE)part.data();
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        c.UserName = user;
        if (!CredWriteW(&c, 0)) {
            delete_chunks_from(0);
            return false;
        }
    }
    delete_chunks_from((int)n);   // a longer session before left more chunks
    return true;
}

void erase_blob()   // (g_store_mu held)
{
#ifdef COGBOX_NEXUS_TESTS
    if (g_memory) {
        g_mem_blob.reset();
        return;
    }
#endif
    delete_chunks_from(0);
}

struct Session {
    NexusTokens t;
    std::string name;
};

std::optional<Session> load_session()
{
    std::lock_guard<std::mutex> lk(g_store_mu);
    const auto blob = read_blob();
    if (!blob) return std::nullopt;
    const json j = json::parse(*blob, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    Session s;
    s.t.access = str_of(j, "access");
    s.t.refresh = str_of(j, "refresh");
    s.t.id_token = str_of(j, "id_token");
    s.t.expires_at = j.contains("expires_at") ? num_of(j["expires_at"], 0) : 0;
    s.name = clean_name(str_of(j, "name"));
    if (s.t.access.empty() && s.t.refresh.empty()) return std::nullopt;
    return s;
}

bool store_session(const NexusTokens &t, const std::string *name)   // name null: keep the stored one
{
    std::lock_guard<std::mutex> lk(g_store_mu);
    std::string keep;
    if (!name) {
        const auto old = read_blob();
        const json o = old ? json::parse(*old, nullptr, false) : json();
        if (o.is_object()) keep = str_of(o, "name");
    }
    const json j = { { "access", t.access }, { "refresh", t.refresh }, { "id_token", t.id_token },
                     { "expires_at", t.expires_at }, { "name", clean_name(name ? *name : keep) } };
    return write_blob(j.dump(-1, ' ', false, json::error_handler_t::replace));
}

void delete_session()
{
    std::lock_guard<std::mutex> lk(g_store_mu);
    erase_blob();
}

// ---- the loopback listener ------------------------------------------------------------------------------------------

struct WsaScope {
    bool ok;
    WsaScope() { WSADATA w; ok = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    ~WsaScope() { if (ok) WSACleanup(); }
};

struct Sock {
    SOCKET s = INVALID_SOCKET;
    ~Sock() { if (s != INVALID_SOCKET) closesocket(s); }
};

using Clock = std::chrono::steady_clock;

// What is left of `until`, in ms, for a socket timeout (at least 1; 0 = past it).
DWORD ms_left(Clock::time_point until)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
    return left <= 0 ? 0 : (DWORD)left;
}

// Reads the request head (up to the blank line), at most kMaxHead bytes, by `until`. "" when there is none.
std::string read_head(SOCKET c, Clock::time_point until)
{
    std::string head;
    char buf[2048];
    while (head.size() < kMaxHead) {
        const DWORD to = ms_left(until);
        if (to == 0) break;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        const int n = recv(c, buf, (int)std::min(sizeof buf, kMaxHead - head.size()), 0);
        if (n <= 0) break;   // closed, timed out or failed
        head.append(buf, n);
        const size_t end = head.find("\r\n\r\n");
        if (end != std::string::npos) {
            head.resize(end + 4);
            return head;
        }
    }
    return "";
}

// Sends the answer with Connection: close and waits (until `until` at most) for the browser to close first, so the
// port is not left in TIME_WAIT for the next sign-in.
void answer(SOCKET c, int status, const char *reason, const std::string &html, Clock::time_point until)
{
    const std::string a = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                          "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.size()) +
                          "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
                          "Connection: close\r\n\r\n" + html;
    DWORD to = ms_left(until);
    if (to == 0) return;
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    size_t at = 0;
    while (at < a.size()) {
        const int n = send(c, a.data() + at, (int)(a.size() - at), 0);
        if (n <= 0) return;
        at += (size_t)n;
    }
    char buf[512];
    for (int spins = 0; spins < 16; spins++) {
        to = ms_left(until);
        if (to == 0) break;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        if (recv(c, buf, sizeof buf, 0) <= 0) break;
    }
}

std::string page(const char *text)
{
    return std::string("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>COGBox</title></head>"
                       "<body style=\"font-family:sans-serif;margin:3em\"><p>") + text + "</p></body></html>";
}

bool open_browser(const std::string &url)
{
#ifdef COGBOX_NEXUS_TESTS
    std::function<void(const std::string &)> hook;
    {
        std::lock_guard<std::mutex> lk(g_hook_mu);
        hook = g_browser_hook;
    }
    if (hook) hook(url);
    return true;   // test builds never open a browser
#else
    if (url.rfind(std::string(kNexusAuthorizeUrl) + "?", 0) != 0 || !nexus_host_allowed(url)) return false;
    const std::wstring w(url.begin(), url.end());   // (ASCII: every part is percent-encoded)
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool ok = (INT_PTR)ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
#endif
}

void wipe(std::string &s)
{
    if (!s.empty()) SecureZeroMemory(&s[0], s.size());
    s.clear();
}
}  // namespace

// ---- public ---------------------------------------------------------------------------------------------------------

std::string nexus_client_id()
{
    auto valid = [](const std::string &s) {
        return !s.empty() && s.size() <= 128 &&
               s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos;
    };
    const char *env = getenv("COGBOX_NEXUS_CLIENT_ID");
    if (env && valid(env)) return env;
#ifdef COGBOX_NEXUS_CLIENT_ID
    const std::string built = COGBOX_NEXUS_CLIENT_ID;
    if (valid(built)) return built;
#endif
    return "";
}

std::string pkce_verifier() { return random_b64(48); }   // 48 bytes = 64 characters

std::string pkce_challenge(const std::string &verifier)
{
    unsigned char h[32];
    if (!sha256(verifier, h)) return "";
    return b64url_encode(h, sizeof h);
}

std::string oauth_state() { return random_b64(32); }

std::string nexus_authorize_url(const std::string &client_id, const std::string &challenge, const std::string &state)
{
    return std::string(kNexusAuthorizeUrl) + "?response_type=code&client_id=" + pct_encode(client_id) +
           "&redirect_uri=" + pct_encode(kNexusRedirectUri) + "&scope=openid%20public&code_challenge=" + pct_encode(challenge) +
           "&code_challenge_method=S256&state=" + pct_encode(state);
}

NexusCallback nexus_parse_callback(const std::string &request_head, const std::string &expected_state)
{
    NexusCallback r;
    const std::string line = request_head.substr(0, request_head.find("\r\n"));
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || line.compare(0, sp1, "GET") != 0) return r;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string target = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    const size_t q = target.find('?');
    if (target.substr(0, q) != "/callback") return r;
    const std::string qs = q == std::string::npos ? std::string() : target.substr(q + 1);

    std::string state, error, code;
    if (expected_state.empty() || !query_value(qs, "state", &state) || !equal_ct(state, expected_state)) {
        r.kind = NexusCallback::BadState;
        return r;
    }
    r.kind = NexusCallback::Failed;
    if (query_value(qs, "error", &error)) {
        r.problem = error == "access_denied" ? kDenied : "Nexus Mods did not sign you in - try again.";
        return r;
    }
    if (!query_value(qs, "code", &code) || code.empty() || code.size() > 4096) {
        r.problem = "Nexus Mods sent no sign-in code - try again.";
        return r;
    }
    r.kind = NexusCallback::Code;
    r.code = code;
    return r;
}

std::string nexus_name_from_userinfo(const std::string &userinfo_body, const std::string &id_token, const std::string &access)
{
    const json j = json::parse(userinfo_body, nullptr, false);
    if (j.is_object()) {
        for (const char *k : { "name", "preferred_username" }) {
            const std::string n = clean_name(str_of(j, k));
            if (!n.empty()) return n;
        }
    }
    for (const std::string *tok : { &id_token, &access }) {
        const json p = jwt_payload(*tok);
        if (p.is_object() && p.contains("user") && p["user"].is_object()) {
            const std::string n = clean_name(str_of(p["user"], "username"));
            if (!n.empty()) return n;
        }
    }
    return "";
}

std::string nexus_sign_in(std::atomic<bool> &cancel, NexusAccount *out)
{
    if (out) *out = NexusAccount{};
    const std::string client = nexus_client_id();
    if (client.empty()) return kNoClient;

    WsaScope wsa;
    if (!wsa.ok) return "COGBox could not listen for the sign-in.";
    Sock ls;
    ls.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls.s == INVALID_SOCKET) return "COGBox could not listen for the sign-in.";
    BOOL exclusive = TRUE;
    if (setsockopt(ls.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive) != 0)
        return "COGBox could not listen for the sign-in.";   // (never listen on a port another program could share)
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only, never every address
    a.sin_port = htons((u_short)kNexusCallbackPort);
    if (bind(ls.s, (sockaddr *)&a, sizeof a) != 0) {
        const int e = WSAGetLastError();
        return e == WSAEADDRINUSE || e == WSAEACCES ? kBusyPort : "COGBox could not listen for the sign-in.";
    }
    if (listen(ls.s, 4) != 0) return "COGBox could not listen for the sign-in.";

    std::string verifier = pkce_verifier();
    const std::string state = oauth_state(), challenge = pkce_challenge(verifier);
    if (verifier.size() != 64 || state.empty() || challenge.empty()) return "COGBox could not start the sign-in.";
    if (!open_browser(nexus_authorize_url(client, challenge, state))) return "COGBox could not open your browser.";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    std::string code;
    while (code.empty()) {
        if (cancel) return "Sign-in was cancelled.";
        if (std::chrono::steady_clock::now() >= deadline) return "Sign-in timed out - try again.";
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ls.s, &fs);
        timeval tv{ 0, 100000 };
        if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;
        Sock c;
        c.s = accept(ls.s, nullptr, nullptr);
        if (c.s == INVALID_SOCKET) continue;
        const auto conn_until = Clock::now() + std::chrono::seconds(2);   // one connection's whole time: read + answer
        const std::string head = read_head(c.s, conn_until);
        if (head.empty()) {
            answer(c.s, 400, "Bad Request", page("Bad request."), conn_until);
            continue;
        }
        NexusCallback cb = nexus_parse_callback(head, state);
        switch (cb.kind) {
        case NexusCallback::NotFound:
            answer(c.s, 404, "Not Found", page("Not found."), conn_until);
            break;
        case NexusCallback::BadState:
            answer(c.s, 400, "Bad Request", page("This is not the sign-in COGBox is waiting for. Start it again from COGBox."), conn_until);
            break;
        case NexusCallback::Failed:
            answer(c.s, 200, "OK", page("You can close this tab and return to COGBox."), conn_until);
            return cb.problem;
        case NexusCallback::Code:
            answer(c.s, 200, "OK", page("You can close this tab and return to COGBox."), conn_until);
            code = cb.code;
            wipe(cb.code);
            break;
        }
    }
    closesocket(ls.s);   // nothing more to hear
    ls.s = INVALID_SOCKET;
    if (cancel) return "Sign-in was cancelled.";
    if (std::chrono::steady_clock::now() >= deadline) return "Sign-in timed out - try again.";

    const std::string form = "grant_type=authorization_code&code=" + pct_encode(code) + "&redirect_uri=" + pct_encode(kNexusRedirectUri) +
                             "&client_id=" + pct_encode(client) + "&code_verifier=" + pct_encode(verifier);
    wipe(code);
    wipe(verifier);
    NexusResponse r = nexus_request("POST", kNexusTokenUrl, form, form_headers(), 15000);
    NexusTokens t;
    if (cancel) return "Sign-in was cancelled.";   // (nothing stored)
    if (r.status == 0) return kUnreachable;
    if (r.status != 200 || !parse_token_answer(r.body, &t)) return "Nexus Mods did not accept the sign-in - try again.";
    wipe(r.body);

    NexusResponse u = nexus_request("GET", kNexusUserinfoUrl, "", { { "Authorization", "Bearer " + t.access }, { "Accept", "application/json" } }, 15000);
    if (cancel) return "Sign-in was cancelled.";
    const std::string name = nexus_name_from_userinfo(u.status == 200 ? u.body : std::string(), t.id_token, t.access);

    std::lock_guard<std::mutex> lk(g_session_mu);
    if (!store_session(t, &name)) return "COGBox could not save the sign-in in Windows Credential Manager.";
    if (out) {
        out->name = name;
        out->signed_in = true;
    }
    return "";
}

std::optional<std::string> nexus_access_token(std::string *problem)
{
    auto fail = [&](const char *p) -> std::optional<std::string> {
        if (problem) *problem = p;
        return std::nullopt;
    };
    std::lock_guard<std::mutex> lk(g_session_mu);
    auto s = load_session();
    if (!s) return fail("Sign in to Nexus Mods first.");
    if (!s->t.access.empty() && s->t.expires_at - now_unix() > kRefreshEarly) return s->t.access;
    if (s->t.refresh.empty()) {
        delete_session();
        return fail(kSignedOut);
    }
    const std::string client = nexus_client_id();
    if (client.empty()) return fail(kNoClient);
    const std::string form = "grant_type=refresh_token&refresh_token=" + pct_encode(s->t.refresh) + "&client_id=" + pct_encode(client);
    NexusResponse r = nexus_request("POST", kNexusTokenUrl, form, form_headers(), 30000);
    // A 4xx means Nexus Mods no longer takes this refresh token (revoked, expired, reused): signed out. Not for 408 /
    // 429 (time-out, too many requests): those are worth another try later.
    if (r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429) {
        delete_session();
        return fail(kSignedOut);
    }
    if (r.status != 200) return fail(kUnreachable);
    NexusTokens t = s->t;
    if (!parse_token_answer(r.body, &t)) return fail("Nexus Mods sent an answer COGBox could not read - try again.");
    wipe(r.body);
    store_session(t, nullptr);   // (if this fails the token still works now; the next refresh may sign out)
    return t.access;
}

void nexus_sign_out()
{
    std::lock_guard<std::mutex> lk(g_session_mu);
    auto s = load_session();
    delete_session();   // first: gone here even if the revoke never answers or COGBox closes meanwhile
    if (!s) return;
    const std::string client = nexus_client_id();
    const bool by_refresh = !s->t.refresh.empty();
    if (!client.empty()) {   // then the revoke at Nexus Mods with the copy in memory (best effort: the answer does not matter)
        std::string form = "token=" + pct_encode(by_refresh ? s->t.refresh : s->t.access) + "&token_type_hint=" +
                           (by_refresh ? "refresh_token" : "access_token") + "&client_id=" + pct_encode(client);
        nexus_request("POST", kNexusRevokeUrl, form, form_headers(), 5000);
        wipe(form);
    }
    wipe(s->t.access);
    wipe(s->t.refresh);
    wipe(s->t.id_token);
}

NexusAccount nexus_account()
{
    NexusAccount a;
    if (auto s = load_session()) {
        a.signed_in = true;
        a.name = s->name;
    }
    return a;
}

bool nexus_store_tokens(const NexusTokens &t)
{
    std::lock_guard<std::mutex> lk(g_session_mu);   // (never in the middle of a refresh or sign-out)
    return store_session(t, nullptr);
}

std::optional<NexusTokens> nexus_load_tokens()
{
    auto s = load_session();
    if (!s) return std::nullopt;
    return s->t;
}

void nexus_delete_tokens()
{
    std::lock_guard<std::mutex> lk(g_session_mu);
    delete_session();
}

#ifdef COGBOX_NEXUS_TESTS
void nexus_auth_test_memory_store(bool on)
{
    std::lock_guard<std::mutex> lk(g_store_mu);
    g_memory = on;
    g_mem_blob.reset();
}

void nexus_auth_test_browser_hook(std::function<void(const std::string &authorize_url)> hook)
{
    std::lock_guard<std::mutex> lk(g_hook_mu);
    g_browser_hook = std::move(hook);
}
#endif

#endif  // COGBOX_FAB_NEXUS
