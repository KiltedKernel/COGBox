// CogBox - Nexus Mods request bodies, answer parsers and links (see nexus_api.h). COGBOX_FAB_NEXUS builds only.
// The answer shapes were checked against the live API on 2026-11-18: ids and counts are numbers, modFiles' sizeInBytes
// is a string and its date a Unix time, primary is 0 / 1, mods' category is a string. Every reader takes the other
// plausible types too (a string for a number, a number for a bool), and anything else as the default.
#ifdef COGBOX_FAB_NEXUS
#include "nexus_api.h"
#include "nexus_net.h"

#include <nlohmann/json.hpp>

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

using json = nlohmann::json;

namespace {
constexpr const char *kModFields =
    "modId name summary author uploader { name } version pictureUrl thumbnailUrl endorsements downloads updatedAt "
    "adultContent category tags { name } gameId game { domainName }";
constexpr const char *kFileFields = "fileId name version category sizeInBytes date description primary";

bool valid_domain(const std::string &d)
{
    if (d.empty() || d.size() > 64) return false;
    for (char c : d)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// A number from the server: negative = 0, too large = INT64_MAX; a string of digits counts; anything else 0.
int64_t to_i64(const json *v)
{
    if (!v) return 0;
    if (v->is_number_unsigned()) {
        const uint64_t u = v->get<uint64_t>();
        return u > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)u;
    }
    if (v->is_number_integer()) {
        const int64_t i = v->get<int64_t>();
        return i < 0 ? 0 : i;
    }
    if (v->is_number_float()) {
        const double d = v->get<double>();
        if (!(d > 0)) return 0;   // (NaN too)
        if (d >= 9.2e18) return INT64_MAX;
        return (int64_t)d;
    }
    if (v->is_string()) {
        const std::string &s = v->get_ref<const std::string &>();
        if (s.empty() || s.size() > 32 || s.find_first_not_of("0123456789") != std::string::npos) return 0;
        if (s.size() > 18) {
            const size_t nz = s.find_first_not_of('0');
            if (nz == std::string::npos) return 0;
            if (s.size() - nz > 18) return INT64_MAX;
        }
        return (int64_t)strtoull(s.c_str(), nullptr, 10);
    }
    return 0;
}
int to_int(const json *v)
{
    const int64_t i = to_i64(v);
    return i > INT_MAX ? INT_MAX : (int)i;
}

const json *field(const json &j, const char *key)
{
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}
// A string field as plain text of at most max bytes; "" when missing or not a string.
std::string text(const json &j, const char *key, size_t max)
{
    const json *v = field(j, key);
    return v && v->is_string() ? nexus_plain_text(v->get_ref<const std::string &>(), max) : std::string();
}
// A picture address: kept only when it is https on an allowed host.
std::string picture(const json &j, const char *key)
{
    const json *v = field(j, key);
    if (!v || !v->is_string()) return {};
    const std::string &s = v->get_ref<const std::string &>();
    return s.size() <= 2048 && nexus_host_allowed(s) ? s : std::string();
}

// The GraphQL answer's data.<key>: nullopt for a body that is not a JSON object, or errors without that data; a null
// json when the data simply isn't there.
std::optional<json> gql_data(const std::string &body, const char *key)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    const json *d = field(j, "data");
    const json *v = d ? field(*d, key) : nullptr;
    const json *e = field(j, "errors");
    const bool errors = e && (!e->is_array() || !e->empty());
    if (!v || v->is_null()) {
        if (errors) return std::nullopt;
        return json(nullptr);
    }
    return *v;
}

// Unix seconds as "YYYY-MM-DDTHH:MM:SSZ" (H. Hinnant's civil_from_days).
std::string iso_time(int64_t t)
{
    if (t <= 0 || t > 253402300799LL) return {};
    const int64_t days = t / 86400, rem = t % 86400;
    const int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    const int64_t y = yoe + era * 400 + (m <= 2);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)y, m, d, (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
    return buf;
}
// A date: a Unix time, or a string (an ISO time as the server wrote it, as plain text).
std::string date_of(const json &j, const char *key)
{
    const json *v = field(j, key);
    if (!v) return {};
    if (v->is_number()) return iso_time(to_i64(v));
    if (v->is_string()) return nexus_plain_text(v->get_ref<const std::string &>(), 40);
    return {};
}

NexusMod mod_from(const json &n)
{
    NexusMod m;
    if (!n.is_object()) return m;
    m.mod_id = to_i64(field(n, "modId"));
    m.game_id = to_int(field(n, "gameId"));
    if (const json *g = field(n, "game")) {
        const json *d = field(*g, "domainName");
        if (d && d->is_string() && valid_domain(d->get<std::string>())) m.domain = d->get<std::string>();
    }
    m.name = text(n, "name", 256);
    m.summary = text(n, "summary", 1024);
    m.description = text(n, "description", 32 * 1024);
    m.author = text(n, "author", 256);
    if (const json *u = field(n, "uploader")) m.uploader = text(*u, "name", 128);
    m.version = text(n, "version", 64);
    m.picture = picture(n, "pictureUrl");
    m.thumbnail = picture(n, "thumbnailUrl");
    if (const json *c = field(n, "category")) {
        if (c->is_string()) m.category = text(n, "category", 128);
        else if (c->is_object()) m.category = text(*c, "name", 128);
    }
    m.updated_at = date_of(n, "updatedAt");
    m.endorsements = to_i64(field(n, "endorsements"));
    m.downloads = to_i64(field(n, "downloads"));
    // Unknown counts as adult (the design: when anything is unknown, adult content stays hidden).
    const json *a = field(n, "adultContent");
    m.adult = !(a && a->is_boolean()) || a->get<bool>();
    if (const json *t = field(n, "tags"); t && t->is_array()) {
        for (const json &e : *t) {
            if (m.tags.size() >= 64) break;
            std::string s = text(e, "name", 64);
            if (!s.empty()) m.tags.push_back(std::move(s));
        }
    }
    return m;
}

std::string gql_body(const std::string &query, json variables)
{
    json j = { { "query", query }, { "variables", std::move(variables) } };
    return j.dump(-1, ' ', false, json::error_handler_t::replace);   // (invalid UTF-8 from the player: U+FFFD, never a throw)
}

// The player's search text: no control characters, at most 200 bytes (not splitting a UTF-8 character).
std::string search_text(const std::string &s)
{
    std::string out;
    for (unsigned char c : s) out += c < 0x20 || c == 0x7f ? ' ' : (char)c;
    if (out.size() > 200) {
        size_t p = 200;
        while (p > 0 && ((unsigned char)out[p] & 0xC0) == 0x80) p--;
        out.resize(p);
    }
    return out;
}

void put_utf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool alnum(char c) { return alpha(c) || (c >= '0' && c <= '9'); }

// Case-insensitive find of an ASCII word from pos; npos when not there.
size_t ifind(const std::string &s, const char *w, size_t pos)
{
    const size_t n = strlen(w);
    for (size_t i = pos; i + n <= s.size(); i++) {
        size_t k = 0;
        while (k < n && tolower((unsigned char)s[i + k]) == w[k]) k++;
        if (k == n) return i;
    }
    return std::string::npos;
}

const char *const kBbcode[] = { "b", "i", "u", "s", "url", "img", "color", "colour", "size", "font", "center", "left", "right",
                                "justify", "quote", "code", "list", "*", "line", "spoiler", "youtube", "heading", "hr", "sup",
                                "sub", "email", "table", "tr", "td", "th", "indent", "ol", "ul", "li", "align", "highlight",
                                "strike", "h1", "h2", "h3", "h4", "pre", "nobbc" };
bool is_bbcode(const std::string &name)
{
    for (const char *k : kBbcode)
        if (name == k) return true;
    return false;
}
// HTML tags after which the text goes on a new line.
bool is_break_tag(const std::string &name)
{
    static const char *const k[] = { "br", "p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "ul", "ol", "table", "blockquote" };
    for (const char *t : k)
        if (name == t) return true;
    return false;
}
}  // namespace

// ---- GraphQL request bodies ----

std::string nexus_gql_mods(const NexusQuery &q)
{
    json filter = json::object();
    filter["op"] = "AND";
    filter["gameDomainName"] = json::array({ { { "value", q.domain }, { "op", "EQUALS" } } });
    if (!q.allow_adult) filter["adultContent"] = json::array({ { { "value", false }, { "op", "EQUALS" } } });
    if (q.cogbox_only) filter["tag"] = json::array({ { { "value", "COGBox" }, { "op", "EQUALS" } } });
    const std::string t = search_text(q.text);
    if (t.find_first_not_of(' ') != std::string::npos) filter["nameStemmed"] = json::array({ { { "value", t }, { "op", "MATCHES" } } });
    std::string sort = "endorsements";
    for (const char *k : { "endorsements", "downloads", "updatedAt", "createdAt", "name" })
        if (q.sort == k) sort = k;
    json order = json::array({ json::object({ { sort, { { "direction", sort == "name" ? "ASC" : "DESC" } } } }) });
    const int count = q.count < 1 ? 1 : q.count > 100 ? 100 : q.count;
    const int offset = q.offset < 0 ? 0 : q.offset;
    return gql_body(std::string("query Mods($filter: ModsFilter, $sort: [ModsSort!], $offset: Int, $count: Int) { "
                                "mods(filter: $filter, sort: $sort, offset: $offset, count: $count) { totalCount nodes { ") +
                        kModFields + " } } }",
                    { { "filter", filter }, { "sort", order }, { "offset", offset }, { "count", count } });
}

std::string nexus_gql_mod(int64_t mod_id, int game_id)
{
    return gql_body(std::string("query Mod($modId: ID!, $gameId: ID!) { mod(modId: $modId, gameId: $gameId) { ") + kModFields +
                        " description } }",
                    { { "modId", std::to_string(mod_id) }, { "gameId", std::to_string(game_id) } });
}

std::string nexus_gql_files(int64_t mod_id, int game_id)
{
    return gql_body(std::string("query ModFiles($modId: ID!, $gameId: ID!) { modFiles(modId: $modId, gameId: $gameId) { ") +
                        kFileFields + " } }",
                    { { "modId", std::to_string(mod_id) }, { "gameId", std::to_string(game_id) } });
}

std::string nexus_gql_game(const std::string &domain)
{
    return gql_body("query Game($domain: String) { game(domainName: $domain) { id domainName name } }", { { "domain", domain } });
}

std::string nexus_gql_preferences()
{
    return gql_body("query Preferences { preferences { adult } }", json::object());
}

// ---- parsers ----

std::optional<std::vector<NexusMod>> nexus_parse_mods(const std::string &body, int *total)
{
    if (total) *total = 0;
    auto v = gql_data(body, "mods");
    if (!v) return std::nullopt;
    std::vector<NexusMod> out;
    if (total) *total = to_int(field(*v, "totalCount"));
    if (const json *nodes = field(*v, "nodes"); nodes && nodes->is_array()) {
        for (const json &n : *nodes) {
            if (out.size() >= 200) break;
            if (!n.is_object()) continue;
            NexusMod m = mod_from(n);
            if (m.mod_id > 0) out.push_back(std::move(m));
        }
    }
    return out;
}

std::optional<NexusMod> nexus_parse_mod(const std::string &body)
{
    auto v = gql_data(body, "mod");
    if (!v || !v->is_object()) return std::nullopt;
    NexusMod m = mod_from(*v);
    if (m.mod_id <= 0) return std::nullopt;
    return m;
}

std::optional<std::vector<NexusFile>> nexus_parse_files(const std::string &body)
{
    auto v = gql_data(body, "modFiles");
    if (!v) return std::nullopt;
    std::vector<NexusFile> out;
    if (!v->is_array()) return out;
    for (const json &n : *v) {
        if (out.size() >= 500) break;
        if (!n.is_object()) continue;
        NexusFile f;
        f.file_id = to_i64(field(n, "fileId"));
        if (f.file_id <= 0) continue;
        f.name = text(n, "name", 256);
        f.version = text(n, "version", 64);
        const std::string cat = text(n, "category", 32);
        if (cat.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_") == std::string::npos) f.category = cat;
        f.size_bytes = to_i64(field(n, "sizeInBytes"));
        f.uploaded_at = date_of(n, "date");
        f.description = text(n, "description", 4096);
        const json *p = field(n, "primary");
        f.primary = p && ((p->is_boolean() && p->get<bool>()) || (p->is_number() && to_i64(p) > 0));
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<int> nexus_parse_game_id(const std::string &body)
{
    auto v = gql_data(body, "game");
    if (!v || !v->is_object()) return std::nullopt;
    const int64_t id = to_i64(field(*v, "id"));
    if (id <= 0 || id >= INT_MAX) return std::nullopt;
    return (int)id;
}

std::optional<bool> nexus_parse_adult_pref(const std::string &body)
{
    auto v = gql_data(body, "preferences");
    if (!v) return std::nullopt;
    const json *a = field(*v, "adult");
    if (!a || !a->is_boolean()) return std::nullopt;
    return a->get<bool>();
}

// ---- REST download link ----

std::string nexus_rest_download_link_url(const std::string &domain, int64_t mod_id, int64_t file_id)
{
    if (!valid_domain(domain) || mod_id <= 0 || file_id <= 0) return {};
    return "https://api.nexusmods.com/v1/games/" + domain + "/mods/" + std::to_string(mod_id) + "/files/" + std::to_string(file_id) +
           "/download_link.json";
}

std::optional<std::vector<std::string>> nexus_parse_download_links(const std::string &body)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return std::nullopt;
    std::vector<std::string> out;
    for (const json &e : j) {
        if (out.size() >= 32) break;
        const json *u = field(e, "URI");
        if (!u || !u->is_string()) continue;
        const std::string &s = u->get_ref<const std::string &>();
        if (s.size() <= 4096 && nexus_host_allowed(s)) out.push_back(s);
    }
    return out;
}

// ---- helpers ----

std::string nexus_mod_url(const std::string &domain, int64_t mod_id, int64_t file_id)
{
    if (!valid_domain(domain) || mod_id <= 0 || file_id < 0) return {};
    std::string u = "https://www.nexusmods.com/" + domain + "/mods/" + std::to_string(mod_id);
    if (file_id > 0) u += "?tab=files&file_id=" + std::to_string(file_id);
    return u;
}

std::string nexus_plain_text(const std::string &whole, size_t max_bytes)
{
    // Review fix: only the first few multiples of max_bytes are read (markup can be most of a description, but never
    // that much), so a huge hostile description costs no more than a sane one.
    const size_t limit = max_bytes > ((size_t)1 << 28) ? ((size_t)1 << 31) : max_bytes * 8 + 4096;
    std::string head;
    if (whole.size() > limit) head = whole.substr(0, limit);
    const std::string &in = whole.size() > limit ? head : whole;
    bool no_img_end = false, no_youtube_end = false;   // an unclosed [img] / [youtube] is searched for once only
    std::string out;
    int newlines = 0;     // line breaks waiting to be written (at most 2: one empty line)
    bool space = false;   // a space waiting
    auto put = [&](char c) {
        const unsigned char u = (unsigned char)c;
        if (c == '\n') { if (newlines < 2) newlines++; space = false; return; }
        if (u < 0x20 || u == 0x7f || c == ' ') { space = true; return; }
        if (!out.empty()) {
            if (newlines) out.append(newlines, '\n');
            else if (space) out += ' ';
        }
        newlines = 0;
        space = false;
        out += c;
    };
    const size_t n = in.size();
    size_t i = 0;
    while (i < n && out.size() <= max_bytes + 8) {
        const char c = in[i];
        if (c == '<' && i + 1 < n && (alpha(in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!' || in[i + 1] == '?')) {
            if (in.compare(i, 4, "<!--") == 0) {
                const size_t e = in.find("-->", i + 4);
                if (e == std::string::npos) break;
                i = e + 3;
                continue;
            }
            const size_t gt = in.find('>', i);
            if (gt == std::string::npos) break;   // an unclosed tag: the rest is not text
            const bool closing = in[i + 1] == '/';
            const bool self_closed = gt > i && in[gt - 1] == '/';   // <script ... />: nothing inside
            size_t k = i + 1 + closing;
            std::string name;
            while (k < gt && alnum(in[k]) && name.size() < 16) name += (char)tolower((unsigned char)in[k++]);
            i = gt + 1;
            if ((name == "script" || name == "style") && !closing && !self_closed) {
                const size_t e = ifind(in, name == "script" ? "</script" : "</style", i);
                if (e == std::string::npos) break;   // never closed: nothing after it is shown
                const size_t g2 = in.find('>', e);
                if (g2 == std::string::npos) break;
                i = g2 + 1;
                continue;
            }
            if (is_break_tag(name)) put('\n');
            else put(' ');
            continue;
        }
        if (c == '[') {
            size_t k = i + 1;
            const bool closing = k < n && in[k] == '/';
            if (closing) k++;
            std::string name;
            while (k < n && (alnum(in[k]) || in[k] == '*') && name.size() < 12) name += (char)tolower((unsigned char)in[k++]);
            if (!name.empty() && k < n && (in[k] == ']' || in[k] == '=' || in[k] == ' ') && is_bbcode(name)) {
                size_t e = k;
                while (e < n && e - k < 2048 && in[e] != ']' && in[e] != '\n' && in[e] != '[') e++;   // ('[': stays linear)
                if (e < n && in[e] == ']') {
                    i = e + 1;
                    if (!closing && (name == "img" || name == "youtube")) {   // their content is an address, not text
                        bool &none = name == "img" ? no_img_end : no_youtube_end;
                        const size_t end = none ? std::string::npos : ifind(in, name == "img" ? "[/img]" : "[/youtube]", i);
                        if (end != std::string::npos) i = end + (name == "img" ? 6 : 10);
                        else none = true;
                        continue;
                    }
                    if (name == "line" || name == "hr" || name == "*" || name == "li" || (closing && (name == "quote" || name == "list")))
                        put('\n');
                    continue;
                }
            }
            put(c);
            i++;
            continue;
        }
        if (c == '&') {
            size_t semi = i + 1;   // (looked for in the next 10 bytes only: a run of '&' stays linear)
            while (semi < n && semi - i <= 10 && in[semi] != ';') semi++;
            if (semi < n && in[semi] == ';' && semi - i <= 10) {
                const std::string ent = in.substr(i + 1, semi - i - 1);
                std::string rep;
                bool known = true;
                if (ent == "amp") rep = "&";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "quot") rep = "\"";
                else if (ent == "apos") rep = "'";
                else if (ent == "nbsp") rep = " ";
                else if (ent.size() >= 2 && ent[0] == '#') {
                    const bool hex = ent[1] == 'x' || ent[1] == 'X';
                    const std::string digits = ent.substr(hex ? 2 : 1);
                    if (!digits.empty() && digits.size() <= 8 &&
                        digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") == std::string::npos) {
                        const uint32_t cp = (uint32_t)strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
                        if (cp == '\n' || cp == '\t') rep = (char)cp;
                        else if (cp >= 0x20 && !(cp >= 0x7f && cp <= 0x9f) && !(cp >= 0xD800 && cp <= 0xDFFF) && cp <= 0x10FFFF)
                            put_utf8(rep, cp);
                    } else {
                        known = false;
                    }
                } else {
                    known = false;
                }
                if (known) {
                    for (char r : rep) put(r);
                    i = semi + 1;
                    continue;
                }
            }
        }
        put(c);
        i++;
    }
    if (out.size() > max_bytes) {
        size_t p = max_bytes;
        while (p > 0 && ((unsigned char)out[p] & 0xC0) == 0x80) p--;
        out.resize(p);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    }
    return out;
}

const char *nexus_domain_for_game(int g)
{
    switch (g) {
    case G_GEARS1: return "gearsofwar";
    case G_GEARS2: return "gearsofwar2";
    case G_GEARS3: return "gearsofwar3";
    case G_JUDGMENT: return "gearsofwarjudgement";
    default: return "";
    }
}

int nexus_game_for_domain(const std::string &domain)
{
    for (int g = 0; g < G_COUNT; g++)
        if (domain == nexus_domain_for_game(g)) return g;
    return -1;
}

#endif  // COGBOX_FAB_NEXUS
