// CogBox - getting a Nexus Mods file for the Fabricator (see nexus_download.h). COGBOX_FAB_NEXUS builds only.
#ifdef COGBOX_FAB_NEXUS
#include "nexus_download.h"
#include "nexus_auth.h"
#include "nexus_net.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {
std::wstring wide_of(const std::string &s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string utf8_of(const std::wstring &w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
// lower case for ASCII; other characters (UTF-8 bytes) stay as they are
std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool ends_with(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
void wipe(std::string &s)
{
    if (!s.empty()) SecureZeroMemory(&s[0], s.size());
    s.clear();
}

// a plain file name: no path, wildcard (FindFirstFile's DOS ones too) or control character, not "." / ".."
bool plain_name(const std::string &n)
{
    if (n.empty() || n.size() > 255 || n == "." || n == "..") return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c)) return false;
    return true;
}

// a browser's copy suffix at the end of a stem: " (12)" or "(12)"; the stem without it
std::string without_copy(const std::string &stem)
{
    if (stem.size() < 3 || stem.back() != ')') return stem;
    const size_t open = stem.rfind('(');
    if (open == std::string::npos || open + 2 >= stem.size()) return stem;
    const std::string digits = stem.substr(open + 1, stem.size() - open - 2);
    if (digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) return stem;
    size_t at = open;
    if (at > 0 && stem[at - 1] == ' ') at--;
    return at == 0 ? stem : stem.substr(0, at);
}

void split_ext(const std::string &name, std::string *stem, std::string *ext)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) { *stem = name; ext->clear(); return; }
    *stem = name.substr(0, dot);
    *ext = name.substr(dot);
}

bool unfinished(const std::string &low_name)
{
    for (const char *e : { ".crdownload", ".part", ".tmp", ".partial", ".download", ".opdownload" })
        if (ends_with(low_name, e)) return true;
    return false;
}

int64_t unix_of(const FILETIME &ft)
{
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000000ull) - 11644473600ll;
}

// "YYYY-MM-DDTHH:MM:SSZ" (nexus_api's dates) as a Unix time; -1 for anything else
int64_t unix_of_iso(const std::string &s)
{
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return -1;
    for (int i : { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 })
        if (!isdigit((unsigned char)s[i])) return -1;
    const int y = atoi(s.substr(0, 4).c_str()), m = atoi(s.substr(5, 2).c_str()), d = atoi(s.substr(8, 2).c_str());
    const int hh = atoi(s.substr(11, 2).c_str()), mm = atoi(s.substr(14, 2).c_str()), ss = atoi(s.substr(17, 2).c_str());
    if (m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60) return -1;
    // days from civil (H. Hinnant)
    const int yy = y - (m <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

// a file's name without a trailing version token ("Lancer Pack v1.2" -> "lancer pack"), lower case
std::string name_stem(const std::string &name)
{
    std::string s = lower(name);
    for (;;) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '-' || s.back() == '_')) s.pop_back();
        const size_t sep = s.find_last_of(" -_");
        const std::string tok = sep == std::string::npos ? s : s.substr(sep + 1);
        const size_t digits = tok.size() > 1 && tok[0] == 'v' ? 1 : 0;
        if (sep == std::string::npos || tok.size() <= digits || !isdigit((unsigned char)tok[digits]) ||
            tok.find_first_not_of("0123456789.", digits) != std::string::npos)
            break;
        s.resize(sep);
    }
    return s;
}

bool dead_category(const std::string &c) { return c == "OLD_VERSION" || c == "ARCHIVED" || c == "DELETED" || c == "REMOVED"; }

std::vector<uint64_t> version_numbers(const std::string &v)
{
    std::vector<uint64_t> out;
    for (size_t i = 0; i < v.size() && out.size() < 16;) {
        if (!isdigit((unsigned char)v[i])) { i++; continue; }
        uint64_t n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) {
            if (n < 1000000000000ull) n = n * 10 + (uint64_t)(v[i] - '0');
            i++;
        }
        out.push_back(n);
    }
    return out;
}
}  // namespace

// ---- Premium ----------------------------------------------------------------------------------------------------------
std::string nexus_try_premium_download(const std::string &domain, int64_t mod_id, int64_t file_id, const std::string &dest_zip,
                                       FabProgress &p)
{
    const std::string url = nexus_rest_download_link_url(domain, mod_id, file_id);
    if (url.empty() || dest_zip.empty()) return "That isn't a Nexus Mods file COGBox can get.";
    if (p.cancel) return "cancelled";
    std::string problem;
    std::optional<std::string> token = nexus_access_token(&problem);
    if (!token || token->empty()) return "not premium";   // (signed out: the free flow; a sign-out is said by the account line)
    std::string auth = "Bearer " + *token;
    wipe(*token);
    NexusResponse r = nexus_request("GET", url, "", { { "Authorization", auth }, { "Accept", "application/json" } });
    wipe(auth);
    if (r.status == 401 || r.status == 403) return "not premium";
    if (r.status == 0) return r.error.empty() ? "Nexus Mods could not be reached." : r.error;
    if (r.status == 404) return "That file isn't on Nexus Mods any more.";
    if (r.status == 429) return "Nexus Mods is busy - try again in a few minutes.";
    if (r.status != 200) return "Nexus Mods couldn't give the file (HTTP " + std::to_string(r.status) + ").";
    std::optional<std::vector<std::string>> links = nexus_parse_download_links(r.body);
    wipe(r.body);
    if (!links || links->empty()) return "Nexus Mods gave no download for that file.";
    if (p.cancel) return "cancelled";
    // the links in order: the next one when one fails (refused, cut off, unreachable)
    std::string e = "Nexus Mods gave no download for that file.";
    for (const std::string &l : *links) {
        NexusResponse d = nexus_download(l, dest_zip, {}, &p.cancel, &p.done, &p.total);
        e = d.error;
        if (e.empty() || e == "cancelled" || p.cancel) break;
    }
    for (std::string &l : *links) wipe(l);
    if (p.cancel) e = "cancelled";
    if (!e.empty()) return e;
    const std::string bad = nexus_archive_problem(dest_zip);
    if (!bad.empty()) DeleteFileW(wide_of(dest_zip).c_str());   // (ours: just written)
    return bad;
}

std::string nexus_archive_problem(const std::string &path)
{
    HANDLE f = CreateFileW(wide_of(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "The file couldn't be read.";
    unsigned char b[8] = {};
    DWORD n = 0;
    const BOOL ok = ReadFile(f, b, sizeof b, &n, nullptr);
    CloseHandle(f);
    if (!ok) return "The file couldn't be read.";
    static const unsigned char zip[4] = { 0x50, 0x4B, 0x03, 0x04 }, sevenz[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
    if (n >= 4 && !memcmp(b, zip, 4)) return {};
    const char *kind = nullptr;
    if (n >= 6 && !memcmp(b, sevenz, 6)) kind = ".7z";
    else if (n >= 4 && !memcmp(b, "Rar!", 4)) kind = ".rar";
    if (kind)
        return std::string("COGBox can install .zip mods only. This file is a ") + kind +
               " - ask the mod's author for a .zip, or unpack it and add the folder.";
    return "not a .zip file";
}

// ---- the free flow ----------------------------------------------------------------------------------------------------
std::string nexus_downloads_folder()
{
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &w)) && w) out = utf8_of(w);
    if (w) CoTaskMemFree(w);
    return out;
}

std::string nexus_download_tail(int64_t mod_id, const NexusFile &f)
{
    const int64_t at = unix_of_iso(f.uploaded_at);
    if (mod_id <= 0 || at <= 0) return {};
    std::string v;
    for (char c : f.version) v += isalnum((unsigned char)c) ? c : '-';
    if (v.size() > 64) return {};
    return "-" + std::to_string(mod_id) + "-" + v + "-" + std::to_string(at);
}

std::optional<std::string> nexus_find_download(const std::string &folder, const NexusPickup &p)
{
    if (folder.empty()) return std::nullopt;
    const bool by_name = plain_name(p.expect_name), by_tail = p.expect_tail.size() >= 4 && plain_name(p.expect_tail);
    if (!by_name && !by_tail) return std::nullopt;
    std::string want_stem, want_ext;
    if (by_name) split_ext(lower(p.expect_name), &want_stem, &want_ext);
    const std::string want_tail = lower(p.expect_tail);
    const int64_t since = p.started_at - 60;

    std::string best;
    int64_t best_at = INT64_MIN;
    std::string dir = folder;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    std::vector<std::string> patterns;
    if (by_name) patterns.push_back(want_stem + "*");   // the name itself and its " (n)" copies
    if (by_tail) patterns.push_back("*" + want_tail + "*");
    for (const std::string &pat : patterns) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(wide_of(dir + "\\" + pat).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 0) continue;   // (a browser's placeholder)
            const std::string name = utf8_of(fd.cFileName), low = lower(name);
            if (unfinished(low)) continue;
            const int64_t at = unix_of(fd.ftLastWriteTime);
            if (at < since) continue;
            std::string stem, ext;
            split_ext(low, &stem, &ext);
            const std::string base = without_copy(stem);
            bool match = false;
            if (by_name) match = low == lower(p.expect_name) || (ext == want_ext && base != stem && base == want_stem);
            if (!match && by_tail) match = ends_with(base, want_tail);
            if (match && at > best_at) { best_at = at; best = dir + "\\" + name; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty()) return std::nullopt;
    return best;
}

// ---- updates ----------------------------------------------------------------------------------------------------------
int nexus_version_compare(const std::string &a, const std::string &b)
{
    std::vector<uint64_t> x = version_numbers(a), y = version_numbers(b);
    if (x.empty() || y.empty()) return 0;
    while (x.size() < y.size()) x.push_back(0);
    while (y.size() < x.size()) y.push_back(0);
    for (size_t i = 0; i < x.size(); i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

std::optional<NexusUpdate> nexus_find_update(const std::vector<NexusFile> &files, int64_t installed_file_id,
                                             const std::string &installed_version)
{
    if (files.empty()) return std::nullopt;
    const NexusFile *inst = nullptr;
    for (const NexusFile &f : files)
        if (f.file_id == installed_file_id) { inst = &f; break; }
    const std::string inst_stem = inst ? name_stem(inst->name) : std::string();
    const int64_t inst_at = inst ? unix_of_iso(inst->uploaded_at) : -1;
    // the installed Nexus file's own version when it is listed (mod.ini's version may be written differently)
    const std::string inst_ver = inst && !inst->version.empty() ? inst->version : installed_version;

    NexusUpdate u;
    const NexusFile *best = nullptr;
    int64_t best_at = -1;
    for (const NexusFile &f : files) {
        if (f.file_id <= 0 || f.file_id == installed_file_id || dead_category(f.category)) continue;
        // the installed file known: the same name stem (another MAIN variant is not an update); else a MAIN file
        if (inst ? name_stem(f.name) != inst_stem : f.category != "MAIN") continue;
        const int64_t at = unix_of_iso(f.uploaded_at);
        const int vc = nexus_version_compare(f.version, inst_ver);
        if (vc < 0) continue;   // an older version uploaded again later is not an update
        if (!(vc > 0 || (inst_at > 0 && at > inst_at))) continue;
        if (!best || at > best_at || (at == best_at && nexus_version_compare(f.version, best->version) > 0)) {
            best = &f;
            best_at = at;
        }
    }
    if (best) {
        u.available = true;
        u.file = *best;
    }
    return u;
}

#endif  // COGBOX_FAB_NEXUS
