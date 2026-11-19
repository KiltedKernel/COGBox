// CogBox - test of the Nexus Mods parts (Fabricator + Nexus, COGBOX_FAB_NEXUS): the GraphQL request bodies, the answer
// parsers with hostile data, the download links, the host allowlist, the deep links, and the transport against a
// loopback server (nexus_set_test_base). Prints FAIL lines; exit code = failures. MinGW, from cogbox\:
//   g++ -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DCOGBOX_FAB_NEXUS -DCOGBOX_NEXUS_TESTS
//       -Isrc -Ithird_party test/nexus_api_test.cpp src/nexus_api.cpp src/nexus_net.cpp
//       -static-libgcc -static-libstdc++ -lwinhttp -lws2_32 -o nexus_api_test.exe
// nexus_api_test.exe --live also asks the real API (one game lookup, one mods page) and prints counts only.
#include "../src/nexus_api.h"
#include "../src/nexus_net.h"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

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

static void test_gql_mods_filter()
{
    NexusQuery q;
    q.domain = "gearsofwar3";
    q.cogbox_only = true;
    json j = json::parse(nexus_gql_mods(q), nullptr, false);
    CHECK(j.is_object());
    CHECK(j.contains("query") && j["query"].is_string() && contains(j["query"].get<std::string>(), "mods("));
    const json f = j.value("variables", json::object()).value("filter", json::object());
    CHECK(f.contains("gameDomainName") && f["gameDomainName"].is_array() && f["gameDomainName"][0]["value"] == "gearsofwar3" &&
          f["gameDomainName"][0]["op"] == "EQUALS");
    CHECK(f.contains("adultContent") && f["adultContent"][0]["value"] == false && f["adultContent"][0]["op"] == "EQUALS");
    CHECK(f.contains("tag") && f["tag"][0]["value"] == "COGBox" && f["tag"][0]["op"] == "EQUALS");
    CHECK(!f.contains("nameStemmed"));
    const json s = j["variables"].value("sort", json::array());
    CHECK(s.is_array() && s.size() == 1 && s[0]["endorsements"]["direction"] == "DESC");
    CHECK(j["variables"]["offset"] == 0 && j["variables"]["count"] == 20);

    // adult allowed: no adultContent filter; no tag when not asked; text: nameStemmed MATCHES; name sorts ASC
    q.allow_adult = true;
    q.cogbox_only = false;
    q.text = "lancer \"x\"}";
    q.sort = "name";
    j = json::parse(nexus_gql_mods(q), nullptr, false);
    const json f2 = j["variables"]["filter"];
    CHECK(!f2.contains("adultContent"));
    CHECK(!f2.contains("tag"));
    CHECK(f2.contains("nameStemmed") && f2["nameStemmed"][0]["value"] == "lancer \"x\"}" && f2["nameStemmed"][0]["op"] == "MATCHES");
    CHECK(j["variables"]["sort"][0]["name"]["direction"] == "ASC");

    // an unknown sort falls back to endorsements; the other sorts are DESC; count and offset are kept sane
    q.sort = "random; drop";
    q.count = 100000;
    q.offset = -5;
    j = json::parse(nexus_gql_mods(q), nullptr, false);
    CHECK(j["variables"]["sort"][0].contains("endorsements"));
    CHECK(j["variables"]["count"].get<int>() <= 100 && j["variables"]["count"].get<int>() >= 1);
    CHECK(j["variables"]["offset"] == 0);
    for (const char *k : { "downloads", "updatedAt", "createdAt" }) {
        q.sort = k;
        j = json::parse(nexus_gql_mods(q), nullptr, false);
        CHECK(j["variables"]["sort"][0][k]["direction"] == "DESC");
    }

    // the other builders: valid JSON with their query and variables
    // invalid UTF-8 in the player's text: a body all the same (no throw), still JSON
    q.text = std::string("bad \xC3\x28 \xFF\xFE text");
    std::string bad_body;
    try {
        bad_body = nexus_gql_mods(q);
    } catch (...) {
        CHECK(!"nexus_gql_mods threw on invalid UTF-8");
    }
    CHECK(!bad_body.empty() && json::parse(bad_body, nullptr, false).is_object());
    try {
        CHECK(json::parse(nexus_gql_game("gears\xFF"), nullptr, false).is_object());
    } catch (...) {
        CHECK(!"nexus_gql_game threw on invalid UTF-8");
    }

    j = json::parse(nexus_gql_mod(27, 3590), nullptr, false);
    CHECK(j.is_object() && contains(j["query"].get<std::string>(), "mod(") && j["variables"]["modId"] == "27" && j["variables"]["gameId"] == "3590");
    j = json::parse(nexus_gql_files(27, 3590), nullptr, false);
    CHECK(j.is_object() && contains(j["query"].get<std::string>(), "modFiles(") && j["variables"]["modId"] == "27");
    j = json::parse(nexus_gql_game("gearsofwar3"), nullptr, false);
    CHECK(j.is_object() && contains(j["query"].get<std::string>(), "game(") && j["variables"]["domain"] == "gearsofwar3");
    j = json::parse(nexus_gql_preferences(), nullptr, false);
    CHECK(j.is_object() && contains(j["query"].get<std::string>(), "preferences") && contains(j["query"].get<std::string>(), "adult"));
}

static void test_parse_mods()
{
    // the live shape (2026-11-18): ints for ids and counts, a string category, tags as objects
    const char *body = R"({"data":{"mods":{"totalCount":28,"nodes":[
      {"modId":27,"name":"Multi Patch","summary":"A fix","author":"A","uploader":{"name":"U"},"version":"0.4.2",
       "pictureUrl":"https://staticdelivery.nexusmods.com/mods/3590/images/27/a.jpg",
       "thumbnailUrl":"https://staticdelivery.nexusmods.com/mods/3590/images/thumbnails/27/a.jpg",
       "endorsements":99,"downloads":14037,"updatedAt":"2026-11-17T19:40:23Z","adultContent":false,"category":"Miscellaneous",
       "tags":[{"name":"COGBox"},{"name":"Bug Fixes"},{"bad":1},7],"gameId":3590,"game":{"domainName":"gearsofwar3"}},
      {"modId":"31","name":"Second","downloads":"lots","endorsements":null,"adultContent":true,"gameId":3590,
       "uploader":"not an object","tags":"nope"}
    ]}}})";
    int total = -1;
    auto mods = nexus_parse_mods(body, &total);
    CHECK(mods.has_value());
    CHECK(total == 28);
    if (mods && mods->size() == 2) {
        const NexusMod &a = (*mods)[0], &b = (*mods)[1];
        CHECK(a.mod_id == 27 && a.game_id == 3590 && a.domain == "gearsofwar3" && a.name == "Multi Patch");
        CHECK(a.summary == "A fix" && a.author == "A" && a.uploader == "U" && a.version == "0.4.2");
        CHECK(a.picture == "https://staticdelivery.nexusmods.com/mods/3590/images/27/a.jpg");
        CHECK(a.thumbnail == "https://staticdelivery.nexusmods.com/mods/3590/images/thumbnails/27/a.jpg");
        CHECK(a.endorsements == 99 && a.downloads == 14037 && !a.adult && a.category == "Miscellaneous");
        CHECK(a.updated_at == "2026-11-17T19:40:23Z");
        CHECK(a.tags.size() == 2 && a.tags[0] == "COGBox" && a.tags[1] == "Bug Fixes");
        CHECK(b.mod_id == 31 && b.name == "Second");
        CHECK(b.summary.empty() && b.downloads == 0 && b.endorsements == 0 && b.uploader.empty() && b.tags.empty());
        CHECK(b.adult);
    } else {
        CHECK(mods && mods->size() == 2);
    }

    // an empty page, a missing nodes list, no total pointer
    mods = nexus_parse_mods(R"({"data":{"mods":{"totalCount":0,"nodes":[]}}})", nullptr);
    CHECK(mods && mods->empty());
    mods = nexus_parse_mods(R"({"data":{"mods":{}}})", &total);
    CHECK(mods && mods->empty() && total == 0);

    // one mod (with its description) and its files (sizeInBytes is a string and date a number in the live API)
    auto m = nexus_parse_mod(R"({"data":{"mod":{"modId":27,"gameId":3590,"name":"M","description":"[b]Hi[/b] there<br />x"}}})");
    CHECK(m && m->mod_id == 27 && m->description == "Hi there\nx");
    CHECK(!nexus_parse_mod(R"({"data":{"mod":null}})"));
    auto files = nexus_parse_files(R"({"data":{"modFiles":[
        {"fileId":43,"name":"Tool","version":"0.1","category":"MAIN","sizeInBytes":"5195","date":1787485174,"description":"d","primary":1},
        {"fileId":44,"name":"Old","category":"OLD_VERSION","sizeInBytes":12,"date":"2026-01-02T03:04:05Z","primary":false},
        {"name":"no id"}]}})");
    CHECK(files && files->size() == 2);
    if (files && files->size() == 2) {
        const NexusFile &f = (*files)[0], &g = (*files)[1];
        CHECK(f.file_id == 43 && f.name == "Tool" && f.version == "0.1" && f.category == "MAIN" && f.size_bytes == 5195 && f.primary);
        CHECK(f.uploaded_at == "2026-08-23T11:39:34Z" && f.description == "d");
        CHECK(g.file_id == 44 && g.size_bytes == 12 && !g.primary && g.uploaded_at == "2026-01-02T03:04:05Z");
    }

    // the game lookup and the adult preference
    CHECK(nexus_parse_game_id(R"({"data":{"game":{"id":3590,"domainName":"gearsofwar3","name":"Gears of War 3"}}})") == 3590);
    CHECK(nexus_parse_game_id(R"({"data":{"game":{"id":"3590"}}})") == 3590);
    CHECK(!nexus_parse_game_id(R"({"data":{"game":null}})"));
    CHECK(nexus_parse_adult_pref(R"({"data":{"preferences":{"adult":true}}})") == true);
    CHECK(nexus_parse_adult_pref(R"({"data":{"preferences":{"adult":false}}})") == false);
    // signed out (the live answer): unknown, never "allowed"
    CHECK(!nexus_parse_adult_pref(R"({"errors":[{"message":"You must be logged in","extensions":{"code":"UNAUTHORIZED"}}],"data":{"preferences":null}})"));
    CHECK(!nexus_parse_adult_pref(R"({"data":{"preferences":{"adult":"yes"}}})"));
}

static void test_hostile()
{
    const std::string desc = "<script>alert('x')</script><b>Bold</b> &amp; [url=https://evil.com/]link text[/url] "
                             "[img]https://evil.com/a.png[/img][color=#ff0000]red[/color] a &lt; b &#65;&#x42; [v1.2]\n\n\n\n  end";
    const std::string t = nexus_plain_text(desc, 4096);
    CHECK(!contains(t, "<script") && !contains(t, "alert") && !contains(t, "<b>") && !contains(t, "[url") && !contains(t, "[/url]"));
    CHECK(!contains(t, "[img]") && !contains(t, "evil.com") && !contains(t, "[color"));
    CHECK(contains(t, "Bold & link text"));
    CHECK(contains(t, "red"));
    CHECK(contains(t, "a < b AB"));
    CHECK(contains(t, "[v1.2]"));   // not BBCode: kept
    CHECK(!contains(t, "\n\n\n"));
    CHECK(t.size() >= 3 && t.substr(t.size() - 3) == "end");
    // cut UTF-8-safe: "é" is two bytes, never split
    const std::string cut = nexus_plain_text("ab\xC3\xA9\xC3\xA9", 3);
    CHECK(cut == "ab");
    CHECK(nexus_plain_text("an unclosed <script> and more", 100).find("more") == std::string::npos);
    CHECK(nexus_plain_text("&#0;&#xD800;&#1114112;ok", 100) == "ok");
    CHECK(nexus_plain_text("", 10).empty());

    // review fix: hostile repetition stays linear (each case well under 100 ms)
    auto timed = [](const std::string &in, size_t max) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string out = nexus_plain_text(in, max);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return std::make_pair(out, ms);
    };
    std::string rep_img, rep_amp, rep_attr, rep_yt;
    for (int k = 0; k < 40000; k++) rep_img += "[img]";      // 200 KB of unclosed [img]
    for (int k = 0; k < 200000; k++) rep_amp += "&";         // no ';' anywhere
    for (int k = 0; k < 66000; k++) rep_attr += "[b=";       // BBCode attributes never closed
    for (int k = 0; k < 20000; k++) rep_yt += "[youtube]x";
    for (const std::string *in : { &rep_img, &rep_amp, &rep_attr, &rep_yt }) {
        auto r = timed(*in, 32 * 1024);
        if (r.second >= 100) printf("  plain text took %.1f ms on %zu bytes\n", r.second, in->size());
        CHECK(r.second < 100);
        CHECK(r.first.size() <= 32 * 1024);
    }
    {
        auto r = timed(rep_img, 64);   // a small cap reads little of a large input
        CHECK(r.second < 100 && r.first.size() <= 64);
        std::string many(4u << 20, 'a');   // 4 MiB of text, a 1 KiB cap
        r = timed(many, 1024);
        CHECK(r.second < 100 && r.first.size() == 1024);
    }

    // huge numbers clamp, negative ones are 0; non-https and foreign pictures are dropped; descriptions become plain text
    const std::string body = std::string(R"({"data":{"mods":{"totalCount":1e300,"nodes":[{"modId":99999999999999999999999,"gameId":123456789012,)") +
                             R"("endorsements":-5,"downloads":1.5e30,"name":"<i>N</i>","summary":"[b]S[/b] &amp; more",)" +
                             R"("pictureUrl":"http://staticdelivery.nexusmods.com/a.png","thumbnailUrl":"https://evil.com/nexusmods.com/a.png"},)" +
                             R"({"modId":2,"pictureUrl":"https://staticdelivery.nexusmods.com.evil.com/a.png","thumbnailUrl":"https://user@staticdelivery.nexusmods.com/a.png"},)" +
                             R"J({"modId":3,"pictureUrl":"https://cf-files.nexus-cdn.com/a.png","thumbnailUrl":"javascript:alert(1)"}]}}})J";
    int total = 0;
    auto mods = nexus_parse_mods(body, &total);
    CHECK(mods && mods->size() == 3);
    CHECK(total > 0);
    if (mods && mods->size() == 3) {
        const NexusMod &a = (*mods)[0];
        CHECK(a.mod_id > 0 && a.game_id > 0 && a.endorsements == 0 && a.downloads > 0);
        CHECK(a.name == "N" && a.summary == "S & more");
        CHECK(a.picture.empty() && a.thumbnail.empty());
        CHECK((*mods)[1].picture.empty() && (*mods)[1].thumbnail.empty());
        CHECK((*mods)[2].picture == "https://cf-files.nexus-cdn.com/a.png" && (*mods)[2].thumbnail.empty());
    }
    // a huge name is cut
    const std::string big = R"({"data":{"mod":{"modId":1,"name":")" + std::string(100000, 'x') + R"("}}})";
    auto m = nexus_parse_mod(big);
    CHECK(m && m->name.size() <= 512);
}

static void test_download_links()
{
    auto l = nexus_parse_download_links(R"([
        {"name":"Nexus Global Content Delivery Network","short_name":"Nexus CDN","URI":"https://cf-files.nexus-cdn.com/files/3590/27/x.zip?md5=a&expires=1"},
        {"name":"plain","URI":"http://cf-files.nexus-cdn.com/files/x.zip"},
        {"name":"foreign","URI":"https://evil.com/x.zip"},
        {"name":"lookalike","URI":"https://nexus-cdn.com.evil.com/x.zip"},
        {"name":"no uri"},
        {"name":"typed","URI":5},
        {"name":"supporter","URI":"https://supporter-files.nexus-cdn.com/x.zip"}])");
    CHECK(l && l->size() == 2);
    if (l && l->size() == 2) {
        CHECK((*l)[0] == "https://cf-files.nexus-cdn.com/files/3590/27/x.zip?md5=a&expires=1");
        CHECK((*l)[1] == "https://supporter-files.nexus-cdn.com/x.zip");
    }
    CHECK(!nexus_parse_download_links("not json"));
    CHECK(!nexus_parse_download_links(R"({"code":403,"message":"You don't have permission"})"));
    auto e = nexus_parse_download_links("[]");
    CHECK(e && e->empty());
    CHECK(nexus_rest_download_link_url("gearsofwar3", 27, 47) == "https://api.nexusmods.com/v1/games/gearsofwar3/mods/27/files/47/download_link.json");
    CHECK(nexus_rest_download_link_url("../x", 27, 47).empty());
    CHECK(nexus_rest_download_link_url("gearsofwar3", 0, 47).empty());
}

static void test_mod_url()
{
    CHECK(nexus_mod_url("gearsofwar3", 12, 34) == "https://www.nexusmods.com/gearsofwar3/mods/12?tab=files&file_id=34");
    CHECK(nexus_mod_url("gearsofwar3", 12) == "https://www.nexusmods.com/gearsofwar3/mods/12");
    CHECK(nexus_mod_url("gears of war", 12).empty());
    CHECK(nexus_mod_url("gearsofwar3", -1).empty());
    CHECK(std::string(nexus_domain_for_game(G_GEARS1)) == "gearsofwar");
    CHECK(std::string(nexus_domain_for_game(G_GEARS2)) == "gearsofwar2");
    CHECK(std::string(nexus_domain_for_game(G_GEARS3)) == "gearsofwar3");
    CHECK(std::string(nexus_domain_for_game(G_JUDGMENT)) == "gearsofwarjudgement");
    CHECK(std::string(nexus_domain_for_game(99)).empty());
    CHECK(nexus_game_for_domain("gearsofwar3") == G_GEARS3);
    CHECK(nexus_game_for_domain("gearsofwarjudgement") == G_JUDGMENT);
    CHECK(nexus_game_for_domain("skyrim") == -1);
}

static void test_allowlist()
{
    CHECK(!nexus_host_allowed("http://api.nexusmods.com/v2/graphql"));
    CHECK(!nexus_host_allowed("https://evil.com"));
    CHECK(nexus_host_allowed("https://staticdelivery.nexusmods.com/x.png"));
    CHECK(nexus_host_allowed("https://api.nexusmods.com/v2/graphql"));
    CHECK(nexus_host_allowed("https://users.nexusmods.com/oauth/token"));
    CHECK(nexus_host_allowed("https://cf-files.nexus-cdn.com/x.zip"));
    CHECK(nexus_host_allowed("https://API.NexusMods.com/v2/graphql"));
    CHECK(nexus_host_allowed("https://api.nexusmods.com:443/v2/graphql"));
    CHECK(!nexus_host_allowed("https://api.nexusmods.com:8443/v2/graphql"));
    CHECK(!nexus_host_allowed("https://nexusmods.com.evil.com/"));
    CHECK(!nexus_host_allowed("https://evilnexusmods.com/"));
    CHECK(!nexus_host_allowed("https://api.nexusmods.com@evil.com/"));
    CHECK(!nexus_host_allowed("https://evil.com\\@api.nexusmods.com/"));
    CHECK(!nexus_host_allowed("https://evil.com#.nexusmods.com"));
    CHECK(!nexus_host_allowed("https://evil.com?.nexusmods.com"));
    CHECK(!nexus_host_allowed("https://"));
    CHECK(!nexus_host_allowed("ftp://api.nexusmods.com/"));
    CHECK(!nexus_host_allowed("https://127.0.0.1/"));
    CHECK(!nexus_host_allowed("https://api.nexusmods.com./v2/graphql"));   // a trailing dot: refused
    CHECK(!nexus_host_allowed("https://staticdelivery.nexusmods.com./x.png"));
    CHECK(!nexus_host_allowed("https://.nexusmods.com/"));
    CHECK(!nexus_host_allowed("https://a..nexusmods.com/"));
    // refused before any connection, and the error names no address
    nexus_set_test_base("");
    NexusResponse r = nexus_request("GET", "https://evil.com/x", "", {});
    CHECK(r.status == 0 && !r.error.empty() && !contains(r.error, "evil"));
    r = nexus_request("GET", "http://api.nexusmods.com/v2/graphql", "", {});
    CHECK(r.status == 0 && !r.error.empty() && !contains(r.error, "nexusmods"));
    r = nexus_request("GET", "https://api.nexusmods.com/x", "", { { "X-Bad", "a\r\nInjected: 1" } });
    CHECK(r.status == 0 && !r.error.empty());
}

static void test_errors()
{
    CHECK(!nexus_parse_mods(R"({"errors":[{"message":"bad filter"}]})", nullptr));
    CHECK(!nexus_parse_mods(R"({"errors":[{"message":"bad"}],"data":null})", nullptr));
    CHECK(!nexus_parse_mods("<html>502 Bad Gateway</html>", nullptr));
    CHECK(!nexus_parse_mods("", nullptr));
    CHECK(!nexus_parse_mod(R"({"errors":[{"message":"x"}]})"));
    CHECK(!nexus_parse_files(R"({"errors":[{"message":"x"}]})"));
    CHECK(!nexus_parse_files("{"));
    CHECK(!nexus_parse_game_id(R"({"errors":[{"message":"x"}]})"));
    CHECK(!nexus_parse_adult_pref(R"({"errors":[{"message":"x"}]})"));
}

// A one-shot loopback HTTP server: answers one request with `answer`, keeps the request it saw.
struct Loopback {
    SOCKET ls = INVALID_SOCKET;
    int port = 0;
    std::string request;
    std::thread th;
    int drip_ms = 0;   // > 0: the body after the headers goes out one byte per drip_ms
    bool start(const std::string &answer)
    {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) return false;
        int len = sizeof a;
        getsockname(ls, (sockaddr *)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this, answer] {
            const size_t head_end = answer.find("\r\n\r\n") + 4;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            char buf[8192];
            size_t want = std::string::npos;
            for (;;) {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                request.append(buf, n);
                size_t he = request.find("\r\n\r\n");
                if (he != std::string::npos && want == std::string::npos) {
                    size_t cl = request.find("Content-Length: ");
                    want = he + 4 + (cl != std::string::npos && cl < he ? strtoul(request.c_str() + cl + 16, nullptr, 10) : 0);
                }
                if (want != std::string::npos && request.size() >= want) break;
            }
            size_t at = 0;
            while (at < answer.size()) {
                size_t chunk = std::min<size_t>(answer.size() - at, 1 << 16);
                if (drip_ms > 0 && at >= head_end) { chunk = 1; Sleep(drip_ms); }
                else if (drip_ms > 0) chunk = head_end - at;
                int n = send(c, answer.data() + at, (int)chunk, 0);
                if (n <= 0) break;
                at += n;
            }
            shutdown(c, SD_SEND);
            closesocket(c);
        });
        return true;
    }
    void stop()
    {
        if (th.joinable()) th.join();
        closesocket(ls);
    }
};

static void test_transport()
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    {
        Loopback lb;
        const std::string body = R"({"data":{"game":{"id":3590}}})";
        CHECK(lb.start("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        NexusResponse r = nexus_request("POST", "https://api.nexusmods.com/v2/graphql", nexus_gql_game("gearsofwar3"),
                                        { { "Content-Type", "application/json" } }, 5000);
        lb.stop();
        CHECK(r.status == 200 && r.error.empty());
        CHECK(r.body == body);
        CHECK(nexus_parse_game_id(r.body) == 3590);
        CHECK(contains(lb.request, "POST /v2/graphql HTTP/1.1"));
        CHECK(contains(lb.request, "Application-Name: COGBox\r\n"));
        CHECK(contains(lb.request, "Application-Version: "));
        CHECK(contains(lb.request, "gearsofwar3"));
    }
    {
        // over 8 MiB: refused, no body kept
        Loopback lb;
        const size_t n = (8u << 20) + 4096;
        CHECK(lb.start("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(n) + "\r\nConnection: close\r\n\r\n" + std::string(n, 'a')));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        NexusResponse r = nexus_request("GET", "https://api.nexusmods.com/v1/x.json", "", {}, 10000);
        lb.stop();
        CHECK(!r.error.empty() && r.body.empty());
        CHECK(!contains(r.error, "127.0.0.1"));
    }
    {
        // a redirect is not followed (it could leave the allowlist): the 302 comes back as it is
        Loopback lb;
        CHECK(lb.start("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/evil\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        NexusResponse r = nexus_request("GET", "https://api.nexusmods.com/v1/x.json", "", {}, 5000);
        lb.stop();
        CHECK(r.status == 302);
    }
    {
        // a dripping answer is cut off at timeout_ms for the whole request, not per read
        Loopback lb;
        lb.drip_ms = 100;
        CHECK(lb.start("HTTP/1.1 200 OK\r\nContent-Length: 40\r\nConnection: close\r\n\r\n" + std::string(40, 'a')));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        const auto t0 = std::chrono::steady_clock::now();
        NexusResponse r = nexus_request("GET", "https://api.nexusmods.com/v1/x.json", "", {}, 1000);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lb.stop();
        CHECK(r.status == 0 && !r.error.empty() && r.body.empty());
        CHECK(ms < 1800);
    }
    {
        // the caller can't replace the app headers
        Loopback lb;
        CHECK(lb.start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        NexusResponse r = nexus_request("GET", "https://api.nexusmods.com/v1/x.json", "",
                                        { { "Application-Name", "Other" }, { "application-version", "9" } }, 5000);
        lb.stop();
        CHECK(r.status == 200);
        CHECK(contains(lb.request, "Application-Name: COGBox\r\n") && !contains(lb.request, "Other") && !contains(lb.request, ": 9\r\n"));
    }
    for (const char *host : { "api.nexusmods.com", "users.nexusmods.com", "staticdelivery.nexusmods.com", "x.nexus-cdn.com" }) {
        // Authorization goes only to api / users.nexusmods.com, whatever the caller passes
        Loopback lb;
        CHECK(lb.start("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}"));
        nexus_set_test_base("http://127.0.0.1:" + std::to_string(lb.port));
        NexusResponse r = nexus_request("GET", std::string("https://") + host + "/x", "", { { "authorization", "Bearer secret-t" } }, 5000);
        lb.stop();
        const bool to_us = std::string(host) == "api.nexusmods.com" || std::string(host) == "users.nexusmods.com";
        CHECK(r.status == 200);
        CHECK(contains(lb.request, "secret-t") == to_us);
    }
    nexus_set_test_base("");
    WSACleanup();
}

static void live_smoke()
{
    printf("live: asking https://api.nexusmods.com/v2/graphql (2 requests)\n");
    const std::vector<std::pair<std::string, std::string>> h = { { "Content-Type", "application/json" } };
    NexusResponse g = nexus_request("POST", "https://api.nexusmods.com/v2/graphql", nexus_gql_game("gearsofwar3"), h);
    auto id = nexus_parse_game_id(g.body);
    printf("live: game lookup status %d, game id %s\n", g.status, id ? std::to_string(*id).c_str() : "none");
    CHECK(g.status == 200 && id && *id > 0);
    NexusQuery q;
    q.domain = "gearsofwar3";
    q.count = 10;
    NexusResponse m = nexus_request("POST", "https://api.nexusmods.com/v2/graphql", nexus_gql_mods(q), h);
    int total = 0;
    auto mods = nexus_parse_mods(m.body, &total);
    size_t with_picture = 0, adult = 0;
    if (mods)
        for (const NexusMod &x : *mods) { with_picture += !x.thumbnail.empty(); adult += x.adult; }
    printf("live: mods status %d, %zu mods on the page, total %d, %zu with a thumbnail, %zu adult\n", m.status,
           mods ? mods->size() : 0, total, with_picture, adult);
    CHECK(m.status == 200 && mods && !mods->empty() && total >= (int)mods->size() && adult == 0);
}

int main(int argc, char **argv)
{
    test_gql_mods_filter();
    test_parse_mods();
    test_hostile();
    test_download_links();
    test_mod_url();
    test_allowlist();
    test_errors();
    test_transport();
    if (argc > 1 && !strcmp(argv[1], "--live")) live_smoke();
    printf("%d failure(s)\n", g_failures);
    return g_failures;
}
