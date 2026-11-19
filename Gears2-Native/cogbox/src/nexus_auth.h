// CogBox - signing in to Nexus Mods (COGBOX_FAB_NEXUS builds only): OAuth 2.0 authorization code with PKCE (S256) as a
// public client, a one-shot loopback listener on 127.0.0.1:28654 for the browser's redirect, the token refresh, and the
// tokens in Windows Credential Manager (target COGBox/NexusMods, chunked at 2000 bytes: COGBox/NexusMods/0..n).
// Tokens, codes and verifiers are never logged, never put in problem text and only ever sent to users.nexusmods.com /
// api.nexusmods.com (through nexus_net). Everything here blocks: call it from a worker thread.
// test/nexus_auth_test.cpp.
#pragma once
#ifdef COGBOX_FAB_NEXUS

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

constexpr const char *kNexusAuthorizeUrl = "https://users.nexusmods.com/oauth/authorize";
constexpr const char *kNexusTokenUrl = "https://users.nexusmods.com/oauth/token";
constexpr const char *kNexusRevokeUrl = "https://users.nexusmods.com/oauth/revoke";
constexpr const char *kNexusUserinfoUrl = "https://users.nexusmods.com/oauth/userinfo";
constexpr const char *kNexusRedirectUri = "http://127.0.0.1:28654/callback";
constexpr int kNexusCallbackPort = 28654;

struct NexusTokens {
    std::string access, refresh, id_token;
    int64_t expires_at = 0;   // Unix time the access token stops working
};

struct NexusAccount {
    std::string name;   // the Nexus Mods user name ("" if Nexus did not say)
    bool signed_in = false;
};

// The OAuth client ID: -DCOGBOX_NEXUS_CLIENT_ID="..." (default ""), overridden by the env var COGBOX_NEXUS_CLIENT_ID
// (letters, digits, . _ - only, at most 128; anything else is ignored). "" = sign-in is not available yet.
std::string nexus_client_id();

std::string pkce_verifier();                                  // 64 characters, base64url (48 random bytes)
std::string pkce_challenge(const std::string &verifier);      // base64url(SHA-256(verifier)), no padding
std::string oauth_state();                                    // 256 random bits, base64url
// https://users.nexusmods.com/oauth/authorize?response_type=code&client_id=..&redirect_uri=..&scope=openid%20public
// &code_challenge=..&code_challenge_method=S256&state=..
std::string nexus_authorize_url(const std::string &client_id, const std::string &challenge, const std::string &state);

// What the loopback listener does with one request (its head: request line and headers).
struct NexusCallback {
    enum Kind {
        NotFound,    // not GET /callback: answer 404, keep waiting
        BadState,    // /callback with a missing or wrong state (not our sign-in): answer 400, keep waiting
        Code,        // our sign-in, with a code: answer the "close this tab" page, exchange the code
        Failed,      // our sign-in, but Nexus Mods refused or sent no code: answer the page, stop with `problem`
    } kind = NotFound;
    std::string code;      // Code only
    std::string problem;   // Failed only: plain words for the player
};
NexusCallback nexus_parse_callback(const std::string &request_head, const std::string &expected_state);

// The player's name from a userinfo answer (`name`, else `preferred_username`), else the JWT's `user.username` claim
// (the id token's, else the access token's). "" when none says.
std::string nexus_name_from_userinfo(const std::string &userinfo_body, const std::string &id_token, const std::string &access);

// Signs in: listens on 127.0.0.1:28654, opens the authorize page in the browser, waits up to 5 min (or until cancel)
// for the redirect, exchanges the code, asks for the user name, and stores the tokens. "" on success (out gets the
// account), else a problem in plain words, e.g. "Close whatever is using port 28654 and try again." or "Sign-in was
// cancelled on Nexus Mods.".
std::string nexus_sign_in(std::atomic<bool> &cancel, NexusAccount *out);

// The access token for an API call, refreshed when it ends within 5 min. nullopt with *problem set when there is none:
// not signed in, Nexus Mods not reachable (the tokens are kept), or a refresh refused with a 4xx (the tokens are
// deleted: "You were signed out of Nexus Mods - sign in again."). Calls are serialized, so a refresh never races.
std::optional<std::string> nexus_access_token(std::string *problem);
void nexus_sign_out();          // best-effort revoke at Nexus Mods, then the tokens are deleted here
NexusAccount nexus_account();   // from the stored tokens (no request)

// Storage (Credential Manager). nexus_store_tokens keeps the stored account name.
bool nexus_store_tokens(const NexusTokens &t);
std::optional<NexusTokens> nexus_load_tokens();
void nexus_delete_tokens();

#ifdef COGBOX_NEXUS_TESTS
// Test builds only (they also store under COGBox/NexusModsTest, never the player's COGBox/NexusMods):
void nexus_auth_test_memory_store(bool on);   // keep the tokens in memory instead of Credential Manager
// Called instead of opening the browser (test builds never open one; without a hook the URL goes nowhere).
void nexus_auth_test_browser_hook(std::function<void(const std::string &authorize_url)> hook);
#endif

#endif  // COGBOX_FAB_NEXUS
