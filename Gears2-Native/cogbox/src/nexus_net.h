// CogBox - the Fabricator's one road to Nexus Mods (COGBOX_FAB_NEXUS builds only): a blocking WinHTTP request that
// goes only over https to Nexus Mods' own hosts, carries the app headers and never keeps more than 8 MiB of answer.
// Callers run it on a worker thread. test/nexus_api_test.cpp checks it against a loopback server.
#pragma once
#ifdef COGBOX_FAB_NEXUS

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

#ifdef COGBOX_NEXUS_TESTS
// Test builds only: requests to an allowed host go to this http://127.0.0.1:<port> base instead (same path). "" = off.
void nexus_set_test_base(const std::string &http_loopback_base);
#endif

#endif  // COGBOX_FAB_NEXUS
