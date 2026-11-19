// CogBox - the Fabricator's one road to Nexus Mods (COGBOX_FAB_NEXUS builds only): a blocking WinHTTP request that
// goes only over https to Nexus Mods' own hosts, carries the app headers and never keeps more than 8 MiB of answer.
// nexus_download streams a file (a mod's download) to disk instead. Callers run both on a worker thread.
// test/nexus_api_test.cpp and test/nexus_download_test.cpp check them against loopback servers.
#pragma once
#ifdef COGBOX_FAB_NEXUS

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct NexusResponse {
    int status = 0;        // the HTTP status; 0 = no answer (refused, no connection, timed out, too large)
    std::string body;      // the answer (empty when too large)
    std::string error;     // "" or a short reason for the player; never an address, a token or a header
};

// https to api.nexusmods.com, users.nexusmods.com or a host ending in .nexusmods.com / .nexus-cdn.com, port 443 only,
// no user info. Everything else is refused (http, other hosts, lookalikes such as nexusmods.com.evil.com).
bool nexus_host_allowed(const std::string &url);

// One request. The url must pass nexus_host_allowed (else nothing is sent); Application-Name: COGBox and
// Application-Version: <COGBOX_VERSION> go with it, plus `headers` (a header with a line break refuses the request).
// Redirects are not followed: a 3xx comes back as it is. A body comes with Content-Type: application/json unless
// `headers` names one.
NexusResponse nexus_request(const std::string &method, const std::string &url, const std::string &body,
                            const std::vector<std::pair<std::string, std::string>> &headers, int timeout_ms = 30000);

// A download: GET url streamed to dest_path through dest_path + ".part" and a rename (the same hosts, headers, no
// redirects). An Authorization header goes only to api.nexusmods.com: for any other host it is dropped. Each read waits
// at most read_timeout_ms (a download has no overall deadline: the player can cancel). Only a 200 is saved; status is
// the HTTP status. error: "" when dest_path is written, "cancelled" (cancel set), else a short reason (never the
// address). A failed or cancelled download leaves no .part (an older dest_path stays). done / total (may be null):
// bytes so far, and Content-Length (0 unknown). Over max_bytes (default 4 GiB, the zip limit) is refused.
constexpr uint64_t kNexusMaxDownload = 4ull << 30;
NexusResponse nexus_download(const std::string &url, const std::string &dest_path,
                             const std::vector<std::pair<std::string, std::string>> &headers, std::atomic<bool> *cancel,
                             std::atomic<int64_t> *done = nullptr, std::atomic<int64_t> *total = nullptr,
                             uint64_t max_bytes = kNexusMaxDownload, int read_timeout_ms = 30000);

#ifdef COGBOX_NEXUS_TESTS
// Test builds only: requests to an allowed host go to this http://127.0.0.1:<port> base instead (same path). "" = off.
void nexus_set_test_base(const std::string &http_loopback_base);
#endif

#endif  // COGBOX_FAB_NEXUS
