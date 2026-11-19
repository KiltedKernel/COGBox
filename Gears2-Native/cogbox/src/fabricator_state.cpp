// COGBox - the FABRICATOR screen's state (see fabricator_state.h): the worker, the jobs, the LIBRARY's mods folders.
#include "fabricator_state.h"
#include "i18n.h"
#include "mod_ini_edit.h"
#ifdef COGBOX_FAB_NEXUS
#include "nexus_auth.h"
#include "nexus_net.h"

#include "../third_party/stb_image.h"

#include <wincodec.h>

#include <ctime>
#endif

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace {

// One thread running posted jobs in order. Never destroyed (its thread lives until the process ends).
class Worker {
public:
    void post(std::function<void()> f)
    {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back(std::move(f));
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        cv_.notify_one();
    }
    bool idle()
    {
        std::lock_guard<std::mutex> l(m_);
        return q_.empty() && !running_;
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return !q_.empty(); });
                f = std::move(q_.front());
                q_.pop_front();
                running_ = true;
            }
            f();
            std::lock_guard<std::mutex> l(m_);
            running_ = false;
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool started_ = false, running_ = false;
};
Worker &worker() { static Worker *w = new Worker; return *w; }
#ifdef COGBOX_FAB_NEXUS
// the STORE's requests (lists, pages, the games lookup, the account's preference) and its pictures: their own threads,
// so an install on the worker never holds them up (and they never hold up an install)
Worker &store_worker() { static Worker *w = new Worker; return *w; }
Worker &picture_worker() { static Worker *w = new Worker; return *w; }
#endif

// the jobs (the UI thread's list; the worker holds its own shared_ptr to the one it runs)
std::vector<std::shared_ptr<FabJob>> g_jobs;
int g_nextId = 1;

const char *const kClose = "Close the game first.";

std::string work_root() { return join(join(exe_dir(), "fabricator"), "work"); }

std::string base_name(const std::string &p)
{
    std::string s = p;
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const size_t at = s.find_last_of("\\/");
    return at == std::string::npos ? s : s.substr(at + 1);
}
std::string stem(const std::string &file)
{
    const std::string b = base_name(file);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}
std::string lower_s(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim_s(const std::string &s)
{
    const size_t x = s.find_first_not_of(" \t\r"), y = s.find_last_not_of(" \t\r");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}

int game_of_key(const std::string &k)
{
    for (int g = 0; g < G_COUNT; g++) if (k == game_def(g).key) return g;
    return -1;
}

std::string sha256_file(const std::string &path)
{
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string out;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD n = 0;
        bool good = true;
        while (good && ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) good = BCryptHashData(h, buf.data(), n, 0) == 0;
        unsigned char d[32];
        if (good && BCryptFinishHash(h, d, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
            out.assign(hex, 64);
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// the temporary zip of a folder being added, and its folder (only ever COGBox's own, under fabricator\work)
void drop_temp(FabJob &j)
{
    if (j.work_dir.empty()) return;
    mods_remove_tree(widen(j.work_dir));
    j.work_dir.clear();
}

std::string first_problems(const std::vector<std::string> &v)
{
    std::string s;
    for (size_t i = 0; i < v.size() && i < 3; i++) s += (i ? "; " : "") + v[i];
    if (v.size() > 3) s += " (and " + std::to_string(v.size() - 3) + " more)";
    return s;
}

// ---- the worker's parts -----------------------------------------------------------------------------------------------
void run_add(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, j->from_folder ? FabJob::Packing : FabJob::Checking)) return;   // cancelled
    if (j->from_folder) {
        if (!make_dirs(j->work_dir)) { j->problems = { "couldn't make the Fabricator's work folder (is the disk full?)" }; j->phase = FabJob::Failed; return; }
        j->zip = join(j->work_dir, base_name(j->source) + ".zip");   // its name is the mod's name when mod.ini has none
        const std::string e = fab_pack_folder(j->source, j->zip, j->progress, &j->pack_left_out);
        if (e == "cancelled" || (e.empty() && j->progress.cancel)) { drop_temp(*j); j->phase = FabJob::Cancelled; return; }
        if (!e.empty()) { drop_temp(*j); j->problems = { e }; j->phase = FabJob::Failed; return; }
        j->phase = FabJob::Checking;
    }
    const std::string want = game_is_port(j->want_game) ? game_def(j->want_game).key : "gears3";
    std::vector<std::string> p = fab_check_zip(j->zip, want, &j->info);
    // a mod.ini for another game is not a problem here: the mod goes to that game (fab_install_unpack checks it again)
    const std::string other = "mod.ini is for game ";
    p.erase(std::remove_if(p.begin(), p.end(), [&](const std::string &s) {
        const bool o = s.compare(0, other.size(), other) == 0;
        if (o) j->other_game = true;
        return o;
    }), p.end());
    j->problems = p;
    if (p.empty()) j->sha256 = sha256_file(j->zip);
    if (j->progress.cancel) { drop_temp(*j); j->phase = FabJob::Cancelled; return; }   // Cancel pressed while it checked
    j->phase = FabJob::Checked;
}

void run_unpack(const std::shared_ptr<FabJob> &j)
{
    const std::string e = fab_install_unpack(j->plan, &j->left_out);
    if (!e.empty()) {
        drop_temp(*j);
        j->problems = { e };
        j->phase = FabJob::Failed;
        return;
    }
    j->phase = FabJob::Unpacked;
}

void run_pack(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, FabJob::Packing)) return;
    const std::string e = fab_pack_folder(j->source, j->zip, j->progress);
    if (e.empty() && j->progress.cancel) {   // cancelled as it finished: no zip is left, as the screen says
        DeleteFileW(widen(j->zip).c_str());
        j->phase = FabJob::Cancelled;
        return;
    }
    if (e == "cancelled") { j->phase = FabJob::Cancelled; return; }
    if (!e.empty()) { j->problems = { e }; j->phase = FabJob::Failed; return; }
    j->phase = FabJob::Done;
}

void run_check(const std::shared_ptr<FabJob> &j)
{
    int q = FabJob::Queued;
    if (!j->phase.compare_exchange_strong(q, FabJob::Checking)) return;
    j->problems = fab_check_folder(j->source, j->game_key);
    j->phase = FabJob::Done;
}

// a failure found on the UI thread: said now
void fail_ui(App &a, FabJob &j, const std::string &msg)
{
    drop_temp(j);
    j.said = true;
    j.phase = FabJob::Failed;
    set_message(a, msg, true);
}

std::shared_ptr<FabJob> find_job(int id)
{
    for (auto &j : g_jobs) if (j->id == id) return j;
    return nullptr;
}

void start_add(const std::string &path, int game, const std::string &updateFolder)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Add;
    j->source = path;
    j->from_folder = dir_exists(path);
    j->want_game = game;
    j->update_folder = updateFolder;
    j->name = j->from_folder ? base_name(path) : stem(path);
    if (j->from_folder) j->work_dir = join(work_root(), std::to_string(j->id) + "-" + now_stamp());
    else j->zip = path;
    g_jobs.push_back(j);
    worker().post([j] { run_add(j); });
}

#ifdef COGBOX_FAB_NEXUS
// ---- Nexus Mods ---------------------------------------------------------------------------------------------------------
const char *const kNexusOffline = "Turn on Online play (SOCIAL) to get mods from Nexus Mods.";
bool online_on(const App &a) { return a.ini.get("Online", "Enabled") == "1"; }
// Online play as the UI thread last saw it (fab_state_poll): the threads look before each request, so turning it off
// stops Nexus work between requests
std::atomic<bool> g_online{false};

// the Nexus Mods game id of each domain that was looked up and found (the store's lookup and the update check share
// it; a failed or odd answer is not kept, so it is asked again next time)
// (heap, never freed, like worker(): the threads may still use them while the process exits)
std::mutex &g_gameIdMu = *new std::mutex;
std::map<std::string, int> &g_gameIds = *new std::map<std::string, int>;

bool same_nexus_file(const FabJob &j, const std::string &domain, int64_t mod_id, int64_t file_id)
{
    return j.nexus.kind == "nexus" && j.nexus.mod_id == mod_id && j.nexus.file_id == file_id && lower_s(j.nexus.game_domain) == lower_s(domain);
}

// the game domain of m for game g: its own when it is one of COGBox's games' domains, else the game's ("" none)
std::string domain_for(const NexusMod &m, int g)
{
    if (!m.domain.empty()) return nexus_game_for_domain(m.domain) >= 0 ? lower_s(m.domain) : std::string();
    return nexus_domain_for_game(g);
}

std::shared_ptr<FabJob> new_nexus_job(int g, const std::string &domain, const NexusMod &m, const NexusFile &f)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Add;
    j->want_game = g;
    j->name = !m.name.empty() ? m.name : !f.name.empty() ? f.name : "Nexus Mods file " + std::to_string(f.file_id);
    j->nexus.kind = "nexus";
    j->nexus.game_domain = domain;
    j->nexus.mod_id = m.mod_id;
    j->nexus.file_id = f.file_id;
    j->source = "Nexus Mods";
    return j;
}

// (worker) the Premium download into work_dir, then the usual check (run_add); "not premium": the free flow (Pickup)
void run_nexus_download(const std::shared_ptr<FabJob> &j)
{
    if (j->phase != FabJob::Downloading) return;
    std::string e = make_dirs(j->work_dir) ? nexus_try_premium_download(j->nexus.game_domain, j->nexus.mod_id, j->nexus.file_id, j->zip, j->progress)
                                           : "couldn't make the Fabricator's work folder (is the disk full?)";
    if (j->progress.cancel) e = "cancelled";
    if (e == "not premium") {
        j->zip.clear();
        j->progress.done = 0;
        j->progress.total = 0;
        j->phase = FabJob::Pickup;   // (the UI thread opens the page and looks in Downloads)
        return;
    }
    if (e == "cancelled") { drop_temp(*j); j->phase = FabJob::Cancelled; return; }
    if (!e.empty()) {   // (nexus_try_premium_download checked the file with nexus_archive_problem: a sentence is said as it is)
        drop_temp(*j);
        j->problems = { e };
        if (e[0] >= 'A' && e[0] <= 'Z') j->fail_text = j->name + ": " + e;
        j->phase = FabJob::Failed;
        return;
    }
    int d = FabJob::Downloading;
    if (j->phase.compare_exchange_strong(d, FabJob::Queued)) run_add(j);
}

// (worker) a file from Downloads or chosen: a .7z / .rar / not-a-zip is told as nexus_archive_problem says, then the
// usual check (run_add)
void run_nexus_file(const std::shared_ptr<FabJob> &j)
{
    if (j->phase != FabJob::Queued) return;   // cancelled
    const std::string p = nexus_archive_problem(j->zip);
    if (!p.empty()) {
        j->problems = { p };
        if (p[0] >= 'A' && p[0] <= 'Z') j->fail_text = j->name + ": " + p;
        int q = FabJob::Queued;
        j->phase.compare_exchange_strong(q, FabJob::Failed);
        return;
    }
    run_add(j);
}

// (UI) the file found in Downloads or chosen: checked and installed as an add of that file (never removed by COGBox)
void take_file(const std::shared_ptr<FabJob> &j, const std::string &zip)
{
    j->zip = zip;
    j->pickup_seen.clear();
    j->pickup_found.clear();
    j->phase = FabJob::Queued;
    worker().post([j] { run_nexus_file(j); });
}

// (UI, once a frame) a Pickup job: the page opened once, then Downloads looked at every 2 s for up to 30 min
void pickup_poll(App &a, const std::shared_ptr<FabJob> &j)
{
    const Uint64 now = SDL_GetTicks();
    if (j->progress.cancel) { drop_temp(*j); j->phase = FabJob::Cancelled; return; }
    if (!j->pickup_opened) {
        j->pickup_opened = true;
        j->pickup_folder = nexus_downloads_folder();
        j->pickup.started_at = (int64_t)time(nullptr);
        j->pickup_until = now + 30ull * 60 * 1000;
        j->pickup_next = now + 2000;
        const std::string url = nexus_mod_url(j->nexus.game_domain, j->nexus.mod_id, j->nexus.file_id);
        if (url.rfind("https://www.nexusmods.com/", 0) == 0) shell_open(url);
        if (j->pickup_folder.empty()) {   // no Downloads folder to watch: the player chooses the file
            j->pickup_manual = true;
            set_message(a, trf("Download {1} on the Nexus Mods page that opened, then choose the file.", { j->name }), false);
        } else set_message(a, trf("Download {1} on the Nexus Mods page that opened. COGBox installs it once it is in your Downloads folder (for 30 minutes), or choose the file.", { j->name }), false);
        return;
    }
    if (j->pickup_manual) return;   // (waits for Choose the file... or Cancel)
    if (!j->pickup_found.empty()) {   // "Add {file}?" asked (fab_answer); asked again when another question replaced it
        if (a.modal.open) return;
        ask(a, "fabnexusadd", j->want_game, "Add a mod", trf("Add {1}?", { base_name(j->pickup_found) }) + "\n\n" +
            tr("It is in your Downloads folder. COGBox checks it, then adds it from there (the file stays where it is).") + "\n\n" +
            tr("Source: Nexus Mods"),
            { "Add", "Cancel" }, std::to_string(j->id));
        return;
    }
    if (now >= j->pickup_until) {
        fail_ui(a, *j, trf("{1} didn't show up in your Downloads folder in 30 minutes. Press Get to try again (then Choose the file...), or add the .zip on the LIBRARY tab.", { j->name }));
        return;
    }
    if (now < j->pickup_next || j->pickup_folder.empty()) return;
    j->pickup_next = now + 2000;
    const std::optional<std::string> found = nexus_find_download(j->pickup_folder, j->pickup);
    if (!found) { j->pickup_seen.clear(); return; }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(widen(*found).c_str(), GetFileExInfoStandard, &fa)) return;
    const int64_t size = ((int64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (*found != j->pickup_seen || size != j->pickup_size) {   // new, or still growing: looked at again in 2 s
        j->pickup_seen = *found;
        j->pickup_size = size;
        return;
    }
    j->pickup_found = *found;   // "Add {file}?" next frame (or once the open question is answered)
}

// "Check for updates": one game's state, written by the worker, read by the UI thread
struct UpdateCheck {
    FabNexusUpdates u;
    bool said = true;   // (UI) its result was said
};
std::mutex &g_updMu = *new std::mutex;             // (heap, never freed: the worker may still write at exit)
UpdateCheck (&g_upd)[G_COUNT] = *new UpdateCheck[1][G_COUNT];

struct UpdateRow {
    std::string folder, domain, version;
    int64_t mod_id = 0, file_id = 0;
};

// (worker) the game id of a domain, asked once per run (worker-only cache); -1 unknown, 0 no such game
int nexus_game_id(const std::string &domain, std::string *problem)
{
    {
        std::lock_guard<std::mutex> l(g_gameIdMu);
        auto it = g_gameIds.find(domain);
        if (it != g_gameIds.end()) return it->second;
    }
    if (!g_online) { *problem = "Online play is off."; return -1; }
    NexusResponse r = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_game(domain), {});
    if (r.status != 200) {
        *problem = r.status == 0 && !r.error.empty() ? r.error : "Nexus Mods could not be reached.";
        return -1;
    }
    const std::optional<int> id = nexus_parse_game_id(r.body);
    if (!id || *id <= 0) return 0;   // (not kept: asked again next time)
    std::lock_guard<std::mutex> l(g_gameIdMu);
    return g_gameIds[domain] = *id;
}

void run_update_check(int g, const std::vector<UpdateRow> &rows)
{
    std::map<std::string, NexusUpdate> found;
    std::string problem;
    for (const UpdateRow &row : rows) {
        if (!g_online) {   // turned off meanwhile: no more requests
            if (problem.empty()) problem = "Online play was turned off.";
            break;
        }
        std::string p;
        const int game_id = nexus_game_id(row.domain, &p);
        if (game_id <= 0) {
            if (problem.empty()) problem = game_id < 0 ? p : "Nexus Mods doesn't know that game.";
            continue;
        }
        if (!g_online) { if (problem.empty()) problem = "Online play was turned off."; break; }
        NexusResponse r = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_files(row.mod_id, game_id), {});
        const std::optional<std::vector<NexusFile>> files = r.status == 200 ? nexus_parse_files(r.body) : std::nullopt;
        if (!files) {
            if (problem.empty()) problem = r.status == 0 && !r.error.empty() ? r.error : "Nexus Mods could not be reached.";
            continue;
        }
        const std::optional<NexusUpdate> u = nexus_find_update(*files, row.file_id, row.version);
        found[row.folder] = u ? *u : NexusUpdate{};
    }
    std::lock_guard<std::mutex> l(g_updMu);
    g_upd[g].u.by_folder = std::move(found);
    g_upd[g].u.problem = problem;
    g_upd[g].u.checking = false;
}

// (UI) a finished check said once
void say_updates(App &a)
{
    std::lock_guard<std::mutex> l(g_updMu);
    for (UpdateCheck &c : g_upd) {
        if (c.said || c.u.checking) continue;
        c.said = true;
        int n = 0;
        for (const auto &kv : c.u.by_folder) n += kv.second.available;
        if (!c.u.problem.empty() && n == 0) set_message(a, trf("Couldn't check for updates: {1}", { c.u.problem }), true);
        else if (n == 0) set_message(a, "Your Nexus Mods mods are up to date.", false);
        else set_message(a, n == 1 ? std::string(tr("1 update found.")) : trf("{1} updates found.", { std::to_string(n) }), false);
    }
}
#endif  // COGBOX_FAB_NEXUS

// ---- the LIBRARY's rows -------------------------------------------------------------------------------------------------
int g_libGame = -1;
std::vector<FabLibRow> g_lib;
int g_libFor = -1;
Uint64 g_libAt = 0;
bool g_libDirty = true;

// <folder>\mod.ini's [Mod] Type ("" none), as fab_check_zip reads it from a zip
std::string mod_type(const std::string &folder)
{
    std::string ini;
    if (!read_file(join(folder, "mod.ini"), ini, kFabMaxIniBytes)) return "";
    if (ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
    std::string section, type;
    for (size_t at = 0; at <= ini.size();) {
        size_t e = ini.find('\n', at);
        if (e == std::string::npos) e = ini.size();
        const std::string line = trim_s(ini.substr(at, e - at));
        at = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') { section = lower_s(trim_s(line.substr(1, line.size() - 2))); continue; }
        if (!section.empty() && section != "mod") continue;
        const size_t eq = line.find('=');
        if (eq != std::string::npos && lower_s(trim_s(line.substr(0, eq))) == "type") type = trim_s(line.substr(eq + 1));
    }
    return type;
}

}  // namespace

// ---- jobs ---------------------------------------------------------------------------------------------------------------
void fab_add_path(App &a, const std::string &path)
{
    if (path.empty()) return;
    const bool folder = dir_exists(path);
    const std::string low = lower_s(path);
    if (!folder && (low.size() < 4 || low.compare(low.size() - 4, 4, ".zip") != 0)) {
        set_message(a, "Add a mod's .zip or its folder.", true);
        return;
    }
    if (!folder && !file_exists(path)) { set_message(a, "That file isn't there any more.", true); return; }
    start_add(path, fab_library_game(a), "");
}

void fab_update_from(App &a, int g, const std::string &folder, const std::string &zip)
{
    if (zip.empty() || !game_is_port(g)) return;
    if (a.run.proc) { set_message(a, kClose, true); return; }
    start_add(zip, g, folder);
}

std::shared_ptr<FabJob> fab_pack_start(const std::string &folder, const std::string &zip_path, const std::string &name)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Pack;
    j->source = folder;
    j->zip = zip_path;
    j->name = name;
    g_jobs.push_back(j);   // (listed while it runs: the LIBRARY's WORKING lines, the front button's figure, cancelled at exit)
    worker().post([j] { run_pack(j); });
    return j;
}

std::shared_ptr<FabJob> fab_check_start(const std::string &folder, const std::string &game_key)
{
    auto j = std::make_shared<FabJob>();
    j->id = g_nextId++;
    j->kind = FabJob::Check;
    j->source = folder;
    j->game_key = game_key;
    worker().post([j] { run_check(j); });
    return j;
}

bool fab_can_cancel(const FabJob &j)
{
    const int ph = j.phase;
#ifdef COGBOX_FAB_NEXUS
    if (ph == FabJob::Downloading || ph == FabJob::Pickup) return true;
#endif
    return ph == FabJob::Queued || ph == FabJob::Packing || ph == FabJob::Checked || ph == FabJob::Asking ||
           ph == FabJob::Waiting || ph == FabJob::Unpacked;
}

void fab_cancel(const std::shared_ptr<FabJob> &j)
{
    if (!j) return;
    j->progress.cancel = true;   // a pack stops (fab_pack_folder watches it)
    int q = FabJob::Queued;
    if (j->phase.compare_exchange_strong(q, FabJob::Cancelled)) return;
    // the UI thread's own phases: nothing runs on the worker for them
#ifdef COGBOX_FAB_NEXUS
    int pk = FabJob::Pickup;   // (a Downloading job stops on progress.cancel: the worker sets Cancelled)
    if (j->phase.compare_exchange_strong(pk, FabJob::Cancelled)) { drop_temp(*j); return; }
#endif
    for (int ph : { (int)FabJob::Checked, (int)FabJob::Asking, (int)FabJob::Waiting }) {
        int x = ph;
        if (j->phase.compare_exchange_strong(x, FabJob::Cancelled)) {
            if (j->kind == FabJob::Add) drop_temp(*j);
            return;
        }
    }
    int u = FabJob::Unpacked;   // unpacked, waiting for the game to close: the .partial goes (on the worker, after any other file work)
    if (j->phase.compare_exchange_strong(u, FabJob::Cancelled)) {
        const FabInstallPlan plan = j->plan;
        const std::string work = j->work_dir;
        j->work_dir.clear();
        worker().post([plan, work] { fab_install_abandon(plan); if (!work.empty()) mods_remove_tree(widen(work)); });
    }
}

std::vector<std::shared_ptr<FabJob>> fab_jobs() { return g_jobs; }

bool fab_job_running(const FabJob &j)
{
    const int ph = j.phase;
    return ph != FabJob::Done && ph != FabJob::Failed && ph != FabJob::Cancelled;
}

int fab_job_percent()
{
#ifdef COGBOX_FAB_NEXUS
    bool waiting = false;   // a Pickup waits for the player's browser: it never hides another job's figure
#endif
    for (auto &j : g_jobs) {
        if (!fab_job_running(*j)) continue;
#ifdef COGBOX_FAB_NEXUS
        if (j->phase == FabJob::Pickup) { waiting = true; continue; }
#endif
        const int64_t total = j->progress.total, done = j->progress.done;
        if (j->phase == FabJob::Packing && total > 0) return (int)std::min<int64_t>(100, done * 100 / total);
#ifdef COGBOX_FAB_NEXUS
        if (j->phase == FabJob::Downloading && total > 0) return (int)std::min<int64_t>(100, done * 100 / total);
#endif
        return -2;
    }
#ifdef COGBOX_FAB_NEXUS
    if (waiting) return -2;
#endif
    return -1;
}

// ---- the LIBRARY --------------------------------------------------------------------------------------------------------
int fab_library_game(const App &a)
{
    if (!game_is_port(g_libGame)) g_libGame = game_is_port(a.front) ? a.front : (int)G_GEARS3;
    return g_libGame;
}
void fab_library_set_game(int g)
{
    if (game_is_port(g) && g != g_libGame) { g_libGame = g; g_libDirty = true; }
}
void fab_library_changed() { g_libDirty = true; }

const std::vector<FabLibRow> &fab_library(App &a, int g)
{
    if (!g_libDirty && g == g_libFor && SDL_GetTicks() - g_libAt < 2000) return g_lib;
    g_libDirty = false;
    g_libFor = g;
    g_libAt = SDL_GetTicks();
    g_lib.clear();
    for (const ModItem &m : mods_list(a, g)) {
        FabLibRow r;
        r.m = m;
        if (m.fab) r.rec = fab_read_record(m.path);
        r.type = mod_type(m.path);
        g_lib.push_back(std::move(r));
    }
    return g_lib;
}

// ---- questions ----------------------------------------------------------------------------------------------------------
void fab_uninstall_ask(App &a, int g, const ModItem &m)
{
    ask(a, "fabuninstall", g, "Uninstall a mod",
        trf("Uninstall {1}? Its folder is moved to a backup in the mods folder (.fabricator-backup), so it can be put back by hand.", { m.name }),
        { "Uninstall", "Cancel" }, m.folder);
}

bool fab_answer(App &a, const Modal &m)
{
    if (m.tag == "fabupdate") {
        auto j = find_job(atoi(m.data.c_str()));
        if (!j) return true;
        int x = FabJob::Asking;
        if (m.answer == 0) j->phase.compare_exchange_strong(x, FabJob::Waiting);
        else if (j->phase.compare_exchange_strong(x, FabJob::Cancelled)) drop_temp(*j);
        return true;
    }
#ifdef COGBOX_FAB_NEXUS
    if (m.tag == "fabnexusadd") {   // "Add {file}?" for the file found in Downloads
        auto j = find_job(atoi(m.data.c_str()));
        if (!j || j->phase != FabJob::Pickup || j->pickup_found.empty()) return true;
        if (m.answer == 0) take_file(j, j->pickup_found);
        else fab_cancel(j);
        return true;
    }
#endif
    if (m.tag == "fabuninstall") {
        if (m.answer != 0 || !game_is_port(m.game)) return true;
        const std::string e = fab_uninstall(a, m.game, m.data);
        if (e.empty()) {
            env_refresh(a, m.game);
            set_message(a, "The mod is uninstalled. Its folder is in the mods folder's .fabricator-backup.", false);
        } else set_message(a, e, true);
        fab_library_changed();
        return true;
    }
    return false;
}

// ---- once a frame -------------------------------------------------------------------------------------------------------
// (fab_sweep_partials: a .partial with COGBox's record is an install a closed or crashed COGBox never finished)
static void sweep_partials()
{
    for (int g = 0; g < G_COUNT; g++) fab_sweep_partials(g);
}

#ifdef COGBOX_FAB_NEXUS
static void store_poll(App &a);
#endif

void fab_state_poll(App &a)
{
#ifdef COGBOX_FAB_NEXUS
    store_poll(a);
#endif
    static bool swept = false;
    if (!swept) {   // what a closed (or crashed) COGBox left: temporary zips, unfinished installs; no job exists yet
        swept = true;
        mods_remove_tree(widen(work_root()));
        sweep_partials();
    }
    bool installing = false;
    for (auto &j : g_jobs)
        if (j->kind == FabJob::Add && (j->phase == FabJob::Unpacking || j->phase == FabJob::Unpacked)) installing = true;

    for (auto &j : g_jobs) {
        if (j->kind == FabJob::Pack) {   // the CREATOR tab shows how it went; said here once
            if (j->phase == FabJob::Done && !j->said) set_message(a, trf("Made {1}.", { j->name }), false);
            if (!fab_job_running(*j)) j->said = true;
            continue;
        }
        if (j->kind != FabJob::Add) continue;
        const int ph = j->phase;
#ifdef COGBOX_FAB_NEXUS
        if (ph == FabJob::Pickup) { pickup_poll(a, j); continue; }
#endif
        if (ph == FabJob::Asking && !(a.modal.open && a.modal.tag == "fabupdate" && a.modal.data == std::to_string(j->id))) {
            j->phase = FabJob::Checked;   // its question was replaced by another one: asked again
            continue;
        }
        if (ph == FabJob::Checked) {
            if (j->progress.cancel) { drop_temp(*j); j->phase = FabJob::Cancelled; continue; }
            if (a.modal.open) continue;   // (decided once the open question is answered; no disk scan meanwhile)
            // the game it goes to: the LIBRARY's, or the one its mod.ini names
            int g = j->want_game;
            if (j->other_game) {
                g = game_of_key(j->info.game);
                if (g < 0) j->problems.push_back("its mod.ini is for a game COGBox doesn't know");
                else if (!j->update_folder.empty() && g != j->want_game)
                    j->problems.push_back(std::string("this zip is for ") + game_def(g).shortName);
            }
            if (!j->problems.empty()) {
                fail_ui(a, *j, trf("{1} can't be added: {2}.", { j->name, first_problems(j->problems) }));
                continue;
            }
            j->game = g;
            j->name = j->info.name;
            FabInstallItem item;
            item.name = j->info.name; item.version = j->info.version; item.sha256 = j->sha256;
#ifdef COGBOX_FAB_NEXUS
            if (j->nexus.kind == "nexus") item.source = j->nexus;   // (the same Nexus mod installed: an update)
#endif
            std::string match;
            for (const ModItem &m : mods_list(a, g)) {
                if (!m.fab) continue;
                auto r = fab_read_record(m.path);
                if (r && fab_same_install(*r, item)) { match = m.folder; j->old_version = r->version; break; }
            }
            if (!j->update_folder.empty() && match != j->update_folder) {
                fail_ui(a, *j, trf("This zip is {1}, not the mod you chose to update. Add it with Add a mod instead.", { j->name }));
                continue;
            }
            j->decided_update = !match.empty();
            if (match.empty()) { j->phase = FabJob::Waiting; continue; }
            const std::string from = j->old_version.empty() ? "" : " " + j->old_version, to = j->info.version.empty() ? "" : " to " + j->info.version;
            // "Update Horde Bank 1.0 to 1.1?" - the name and versions as the mod gives them (not translated)
            ask(a, "fabupdate", g, "Update a mod", tr("Update") + std::string(" ") + j->name + from + to + "?\n\n" +
                tr("The installed version is kept as a backup in the mods folder. The mod keeps its place in the order, and stays on or off as it is now.")
#ifdef COGBOX_FAB_NEXUS
                + (j->nexus.kind == "nexus" ? "\n\n" + std::string(tr("Source: Nexus Mods")) : std::string())
#endif
                ,
                { "Update", "Cancel" }, std::to_string(j->id));
            j->phase = FabJob::Asking;
        } else if (ph == FabJob::Waiting) {
            if (installing) continue;
            if (a.run.proc) {
                if (!j->told_close) { j->told_close = true; set_message(a, std::string(tr(kClose)) + " " + tr("The mod is installed once the game is closed."), true); }
                continue;
            }
            FabInstallItem item;
            item.name = j->info.name; item.version = j->info.version; item.sha256 = j->sha256;
#ifdef COGBOX_FAB_NEXUS
            if (j->nexus.kind == "nexus") item.source = j->nexus;
#endif
            FabInstallPlan plan;
            const std::string e = fab_install_prepare(a, j->game, item, j->zip, &plan);
            if (e == kClose) continue;
            if (!e.empty()) { fail_ui(a, *j, e); continue; }
            if (plan.update != j->decided_update) {   // another add of this name was installed meanwhile: decided (asked) again
                j->phase = FabJob::Checked;
                continue;
            }
            plan.strip_top = !j->from_folder;   // a folder add: the chosen folder is the mod's root
            j->plan = plan;   // (read by the worker after the post)
            j->phase = FabJob::Unpacking;
            installing = true;
            worker().post([j] { run_unpack(j); });
        } else if (ph == FabJob::Unpacked) {
            if (a.run.proc) {   // a game started during the unpack: the move waits for it to close (the .partial stays)
                if (!j->told_close) { j->told_close = true; set_message(a, std::string(tr(kClose)) + " " + tr("The mod is installed once the game is closed."), true); }
                continue;
            }
            const std::string e = fab_install_commit(a, j->plan);
            if (e == kClose) continue;
            if (e.rfind("The mods folder changed", 0) == 0) {   // (the commit removed the .partial) decided and unpacked again
                j->phase = FabJob::Checked;
                continue;
            }
            fab_library_changed();
            if (!e.empty()) { fail_ui(a, *j, e); continue; }
            drop_temp(*j);
            env_refresh(a, j->plan.g);
            j->phase = FabJob::Done;
            std::string msg = j->plan.update ? trf("{1} is updated.", { j->name })
                            : j->game != j->want_game ? trf("{1} is added to {2} (its mod.ini is for it) and on.", { j->name, game_def(j->game).shortName })
                                                      : trf("{1} is added and on.", { j->name });
            std::vector<std::string> left = j->pack_left_out;   // a folder's (left out by the pack) and a zip's (by the unpack)
            left.insert(left.end(), j->left_out.begin(), j->left_out.end());
            if (!left.empty()) {   // the program files not added: the first 5 names, then how many more
                std::string names;
                for (size_t i = 0; i < left.size() && i < 5; i++) names += (i ? ", " : "") + base_name(left[i]);
                if (left.size() > 5) names += " +" + std::to_string(left.size() - 5);
                msg += " " + trf("Left out (program files): {1}", { names });
            }
            set_message(a, msg, false);
        } else if (ph == FabJob::Failed && !j->said) {   // a failure on the worker (pack, unpack): said once
            j->said = true;
#ifdef COGBOX_FAB_NEXUS
            if (!j->fail_text.empty()) { set_message(a, j->fail_text, true); continue; }
#endif
            set_message(a, trf("{1} can't be added: {2}.", { j->name, first_problems(j->problems) }), true);
        }
    }
#ifdef COGBOX_FAB_NEXUS
    say_updates(a);
#endif
    // finished jobs leave the list (a failure on the UI thread was said where it happened)
    g_jobs.erase(std::remove_if(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<FabJob> &j) {
        const int ph = j->phase;
        return ph == FabJob::Done || ph == FabJob::Cancelled || (ph == FabJob::Failed && j->said);
    }), g_jobs.end());
}

void fab_state_shutdown()
{
#ifdef COGBOX_FAB_NEXUS
    g_online = false;            // the store's threads send nothing more
    fab_nexus_sign_in_cancel();  // (the listener stops within 100 ms)
#endif
    for (auto &j : g_jobs) j->progress.cancel = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until && !worker().idle()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!worker().idle()) return;   // still unpacking: its .partial is hidden from the game; the next start's sweep removes it
    for (auto &j : g_jobs) {
        if (j->kind != FabJob::Add) continue;
        if (j->phase == FabJob::Unpacked) fab_install_abandon(j->plan);
        drop_temp(*j);
    }
}

#ifdef COGBOX_FAB_NEXUS
// ---- Nexus Mods ---------------------------------------------------------------------------------------------------------
void fab_nexus_get(App &a, int g, const NexusMod &m, const NexusFile &f)
{
    if (!online_on(a)) { set_message(a, kNexusOffline, true); return; }
    const std::string domain = domain_for(m, g);
    if (!game_is_port(g) || domain.empty() || m.mod_id <= 0 || f.file_id <= 0) {
        set_message(a, "That isn't a Nexus Mods file COGBox can get.", true);
        return;
    }
    for (auto &x : g_jobs)
        if (fab_job_running(*x) && same_nexus_file(*x, domain, m.mod_id, f.file_id)) {
            set_message(a, trf("{1} is already being added.", { x->name }), false);
            return;
        }
    auto j = new_nexus_job(g, domain, m, f);
    j->work_dir = join(work_root(), std::to_string(j->id) + "-" + now_stamp());
    std::string file = mod_folder_chars(j->name);
    if (file.empty()) file = "nexus-" + std::to_string(m.mod_id) + "-" + std::to_string(f.file_id);
    j->zip = join(j->work_dir, file + ".zip");   // (its name is the mod's name when mod.ini has none)
    j->pickup.expect_name = f.name;   // (the free flow: the browser's file name, or its Nexus Mods tail)
    j->pickup.expect_tail = nexus_download_tail(m.mod_id, f);
    j->phase = FabJob::Downloading;
    g_jobs.push_back(j);
    worker().post([j] { run_nexus_download(j); });
}

void fab_nexus_choose_file(App &a, int g, const NexusMod &m, const NexusFile &f, const std::string &zip)
{
    const std::string low = lower_s(zip);
    if (zip.empty() || low.size() < 4 || low.compare(low.size() - 4, 4, ".zip") != 0) { set_message(a, "Choose the mod's .zip.", true); return; }
    if (!file_exists(zip)) { set_message(a, "That file isn't there any more.", true); return; }
    const std::string domain = domain_for(m, g);
    if (!game_is_port(g) || domain.empty() || m.mod_id <= 0 || f.file_id <= 0) {
        set_message(a, "That isn't a Nexus Mods file COGBox can get.", true);
        return;
    }
    for (auto &x : g_jobs)
        if (x->phase == FabJob::Pickup && same_nexus_file(*x, domain, m.mod_id, f.file_id)) { take_file(x, zip); return; }
    auto j = new_nexus_job(g, domain, m, f);
    g_jobs.push_back(j);
    take_file(j, zip);
}

void fab_nexus_check_updates(App &a, int g)
{
    if (!online_on(a)) { set_message(a, kNexusOffline, true); return; }
    if (!game_is_port(g)) return;
    std::vector<UpdateRow> rows;
    for (const FabLibRow &r : fab_library(a, g)) {
        if (!r.rec || r.rec->source.kind != "nexus" || r.rec->source.mod_id <= 0) continue;
        const std::string domain = lower_s(r.rec->source.game_domain);
        if (nexus_game_for_domain(domain) < 0) continue;
        UpdateRow u;
        u.folder = r.m.folder;
        u.domain = domain;
        u.version = r.rec->version;
        u.mod_id = r.rec->source.mod_id;
        u.file_id = r.rec->source.file_id;
        rows.push_back(u);
        if (rows.size() >= 100) break;   // (a request per mod: a sane bound)
    }
    if (rows.empty()) { set_message(a, "None of these mods came from Nexus Mods.", false); return; }
    {
        std::lock_guard<std::mutex> l(g_updMu);
        if (g_upd[g].u.checking) return;
        g_upd[g].u.checking = true;
        g_upd[g].said = false;
    }
    worker().post([g, rows] { run_update_check(g, rows); });
}

FabNexusUpdates fab_nexus_updates(int g)
{
    if (g < 0 || g >= G_COUNT) return {};
    std::lock_guard<std::mutex> l(g_updMu);
    return g_upd[g].u;
}

void fab_nexus_update(App &a, int g, const std::string &folder)
{
    if (!game_is_port(g)) return;
    NexusUpdate u;
    {
        std::lock_guard<std::mutex> l(g_updMu);
        auto it = g_upd[g].u.by_folder.find(folder);
        if (it == g_upd[g].u.by_folder.end() || !it->second.available) return;
        u = it->second;
    }
    for (const FabLibRow &r : fab_library(a, g)) {
        if (r.m.folder != folder || !r.rec || r.rec->source.kind != "nexus") continue;
        NexusMod m;
        m.mod_id = r.rec->source.mod_id;
        m.domain = r.rec->source.game_domain;
        m.name = r.rec->name;
        fab_nexus_get(a, g, m, u.file);
        return;
    }
}
// ---- the STORE: browsing Nexus Mods -------------------------------------------------------------------------------------
namespace {

const Uint64 kStoreCacheMs = 5ull * 60 * 1000;   // a list or page answer is kept in memory this long
const size_t kStoreCacheMax = 64;                 // ... and at most this many of each
const size_t kPicturesMax = 64;                   // textures kept while the screen is open
const int kPictureMaxW = 3840, kPictureMaxH = 2160;

std::string store_problem(const NexusResponse &r)
{
    if (r.status == 0 && !r.error.empty()) return r.error;
    if (r.status == 429) return "Nexus Mods is busy - try again in a minute.";
    return "Nexus Mods could not be reached.";
}

// What the store's threads write (below), read by the UI thread. On the heap and never freed, like worker(): a store,
// picture, sign-in or sign-out thread may still be running while the process exits.
std::mutex &g_storeMu = *new std::mutex;

// the games lookup (once per run; Try again looks up the ones that failed)
FabStoreGames &g_games = *new FabStoreGames;
bool g_gamesStarted = false;   // (UI)

// the answers, 5 min
struct ListAnswer { Uint64 at = 0; std::vector<NexusMod> mods; int total = 0; };
struct PageAnswer { Uint64 at = 0; NexusMod mod; std::vector<NexusFile> files; };
std::map<std::string, ListAnswer> &g_lists = *new std::map<std::string, ListAnswer>;
std::map<std::string, PageAnswer> &g_pages = *new std::map<std::string, PageAnswer>;
template <class M> void cache_trim(M &m)   // the old answers out, and room for one more
{
    const Uint64 now = SDL_GetTicks();
    for (auto it = m.begin(); it != m.end();) {
        if (now - it->second.at >= kStoreCacheMs) it = m.erase(it);
        else ++it;
    }
    while (m.size() >= kStoreCacheMax) {
        auto old = m.begin();
        for (auto it = m.begin(); it != m.end(); ++it) if (it->second.at < old->second.at) old = it;
        m.erase(old);
    }
}

// the query and the page wanted now, and how far they are
FabStoreList &g_list = *new FabStoreList;
std::string &g_listKey = *new std::string;
FabStorePage &g_page = *new FabStorePage;
std::string &g_pageKey = *new std::string;

std::string list_key(const NexusQuery &q)
{
    return q.domain + '\x1f' + q.text + '\x1f' + q.sort + '\x1f' + (q.cogbox_only ? '1' : '0') + (q.allow_adult ? '1' : '0') + '\x1f' +
           std::to_string(q.offset) + '\x1f' + std::to_string(q.count);
}

// (store thread) one page of mods
void run_list(const std::string &key, const NexusQuery &q)
{
    {
        std::lock_guard<std::mutex> l(g_storeMu);
        if (key != g_listKey) return;   // no longer wanted: not sent
    }
    std::string problem;
    ListAnswer ans;
    bool ok = false;
    if (!g_online) problem = "Online play is off.";
    else {
        const NexusResponse r = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_mods(q), {}, 20000);
        int total = 0;
        std::optional<std::vector<NexusMod>> mods = r.status == 200 ? nexus_parse_mods(r.body, &total) : std::nullopt;
        if (mods) {
            ok = true;
            for (NexusMod &m : *mods)
                if (!m.adult || q.allow_adult) ans.mods.push_back(std::move(m));   // (the query asked for none; never shown anyway)
            ans.total = std::max(0, total);
            ans.at = SDL_GetTicks();
        } else problem = r.status == 200 ? std::string("Nexus Mods sent an answer COGBox can't read.") : store_problem(r);
    }
    std::lock_guard<std::mutex> l(g_storeMu);
    if (ok) { cache_trim(g_lists); g_lists[key] = ans; }
    if (key != g_listKey) return;
    g_list.loading = false;
    if (ok) {
        g_list.answered = true;
        g_list.mods = std::move(ans.mods);
        g_list.total = ans.total;
    } else g_list.problem = problem;
}

// (store thread) a mod and its files
void run_page(const std::string &key, int game_id, int64_t mod_id)
{
    {
        std::lock_guard<std::mutex> l(g_storeMu);
        if (key != g_pageKey) return;
    }
    std::string problem;
    PageAnswer ans;
    bool ok = false, files_ok = false;
    if (!g_online) problem = "Online play is off.";
    else {
        const NexusResponse r = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_mod(mod_id, game_id), {}, 20000);
        std::optional<NexusMod> m = r.status == 200 ? nexus_parse_mod(r.body) : std::nullopt;
        if (m) {
            ok = true;
            ans.mod = std::move(*m);
            if (g_online) {
                const NexusResponse f = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_files(mod_id, game_id), {}, 20000);
                std::optional<std::vector<NexusFile>> files = f.status == 200 ? nexus_parse_files(f.body) : std::nullopt;
                if (files) { files_ok = true; ans.files = std::move(*files); }
            }
            ans.at = SDL_GetTicks();
        } else problem = r.status == 200 ? std::string("This mod isn't on Nexus Mods any more.") : store_problem(r);
    }
    std::lock_guard<std::mutex> l(g_storeMu);
    if (ok && files_ok) { cache_trim(g_pages); g_pages[key] = ans; }   // (a page without its files is asked again next time)
    if (key != g_pageKey) return;
    g_page.loading = false;
    if (ok) {
        g_page.answered = true;
        g_page.mod = std::move(ans.mod);
        g_page.files = std::move(ans.files);
        g_page.files_ok = files_ok;
    } else g_page.problem = problem;
}

// (store thread) the games Nexus Mods has, by domain
void run_games()
{
    std::vector<FabStoreGame> found;
    std::string problem;
    for (int g : { (int)G_GEARS1, (int)G_GEARS2, (int)G_GEARS3, (int)G_JUDGMENT }) {
        const std::string d = nexus_domain_for_game(g);
        if (d.empty()) continue;
        std::string p;
        const int id = nexus_game_id(d, &p);
        if (id > 0) found.push_back({ g, d, id });
        else if (id < 0 && problem.empty()) problem = p;
    }
    std::lock_guard<std::mutex> l(g_storeMu);
    g_games.games = std::move(found);
    g_games.problem = problem;
    g_games.looking = false;
    g_games.done = true;
}

// the account (g_storeMu)
struct Account {
    FabNexusAccountState s;
    std::optional<bool> adult;     // the player's Nexus Mods preference (unknown: adult mods hidden)
    bool pref_asked = false;       // asked once per sign-in
    int missing = 0;               // reads in a row that found no tokens while signed in
    std::shared_ptr<std::atomic<bool>> cancel;   // the sign-in running
    int msg = 0;                   // said by the UI thread (store_poll): 1 signed in, 2 signed out
    int gen = 0;                   // the account's turn: a preference asked in an earlier one is not kept
};
Account &g_acc = *new Account;
Uint64 g_accNext = 0;   // (UI) the next Credential Manager read

// (store thread) the adult preference of the signed-in player
void run_pref(int gen)
{
    std::string problem;
    std::optional<std::string> tok = g_online ? nexus_access_token(&problem) : std::nullopt;
    std::optional<bool> adult;
    if (tok) {
        const NexusResponse r = nexus_request("POST", kNexusGraphqlUrl, nexus_gql_preferences(), { { "Authorization", "Bearer " + *tok } }, 20000);
        SecureZeroMemory(&(*tok)[0], tok->size());
        if (r.status == 200) adult = nexus_parse_adult_pref(r.body);
    }
    const bool still = tok ? true : nexus_account().signed_in;
    std::lock_guard<std::mutex> l(g_storeMu);
    if (gen != g_acc.gen) return;   // signed out or in again meanwhile: not this account's answer
    g_acc.adult = adult;
    if (!tok && !problem.empty()) g_acc.s.problem = problem;   // e.g. "You were signed out of Nexus Mods - sign in again."
    if (!still && !g_acc.s.signing_in) { g_acc.s.signed_in = false; g_acc.s.name.clear(); }
}

// (UI) the stored account read every 3 s; the preference asked once a sign-in
void account_tick(App &a)
{
    const Uint64 now = SDL_GetTicks();
    bool busy;
    {
        std::lock_guard<std::mutex> l(g_storeMu);
        busy = g_acc.s.signing_in || g_acc.s.signing_out;
    }
    if (!busy && now >= g_accNext) {
        g_accNext = now + 3000;
        const NexusAccount acc = nexus_account();   // (Credential Manager: no request)
        std::lock_guard<std::mutex> l(g_storeMu);
        if (!g_acc.s.signing_in && !g_acc.s.signing_out) {
            if (acc.signed_in) {
                if (!g_acc.s.signed_in) { g_acc.adult.reset(); g_acc.pref_asked = false; g_acc.gen++; }
                g_acc.s.signed_in = true;
                g_acc.s.name = acc.name;
                g_acc.missing = 0;
            } else if (g_acc.s.signed_in && ++g_acc.missing >= 2) {   // (two reads, so a token being rewritten is not taken for it)
                g_acc.s.signed_in = false;
                g_acc.s.name.clear();
                g_acc.adult.reset();
                g_acc.pref_asked = false;
                g_acc.gen++;
                g_acc.missing = 0;
                g_acc.s.problem = "You were signed out of Nexus Mods - sign in again.";
            }
        }
    }
    std::lock_guard<std::mutex> l(g_storeMu);
    if (g_acc.s.signed_in && !g_acc.s.signing_out && !g_acc.pref_asked && online_on(a)) {
        g_acc.pref_asked = true;
        const int gen = g_acc.gen;
        store_worker().post([gen] { run_pref(gen); });
    }
}

// the pictures (UI thread), and the decoded ones the picture thread hands over
struct Picture {
    SDL_Texture *tex = nullptr;
    int w = 0, h = 0;
    uint64_t used = 0;   // the frame it was last asked for
};
std::map<std::string, Picture> g_pics;
uint64_t g_picFrame = 0;
struct Decoded {
    std::string url;
    int gen = 0, w = 0, h = 0;
    std::vector<unsigned char> px;   // RGBA; empty: no picture
};
std::mutex &g_picMu = *new std::mutex;                          // (heap, never freed: see g_storeMu)
std::vector<Decoded> &g_picDone = *new std::vector<Decoded>;
std::atomic<int> g_picGen{0};   // the screen's opening: a picture asked for before it closed is dropped

// (picture thread) WebP (what Nexus Mods' CDN sends for every picture) through Windows' own decoder (WIC): its size read
// from the header before any pixel is decoded. false: not WebP, no WebP decoder on this system (Wine), or too large.
bool decode_webp(const std::string &b, int *w, int *h, std::vector<unsigned char> *px)
{
    if (b.size() < 16 || b.size() > 0xFFFFFFFFull || memcmp(b.data(), "RIFF", 4) != 0 || memcmp(b.data() + 8, "WEBP", 4) != 0) return false;
    static thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (!com) return false;
    IWICImagingFactory *f = nullptr;
    IWICStream *st = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *fr = nullptr;
    IWICFormatConverter *cv = nullptr;
    bool ok = false;
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE *)b.data(), (DWORD)b.size())) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(fr->GetSize(&fw, &fh)) && fw > 0 && fh > 0 && fw <= (UINT)kPictureMaxW && fh <= (UINT)kPictureMaxH &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        px->resize((size_t)fw * fh * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, fw * 4, (UINT)px->size(), px->data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (!ok) { px->clear(); return false; }
    *w = (int)fw;
    *h = (int)fh;
    return true;
}

// (picture thread) fetched through nexus_net (Nexus Mods' https hosts, 8 MiB at most); PNG / JPEG by stb_image, WebP by
// decode_webp; the size (at most 3840x2160) read before decoding
void run_picture(const std::string &url, int gen)
{
    Decoded d;
    d.url = url;
    d.gen = gen;
    if (gen == g_picGen && g_online && nexus_host_allowed(url)) {
        const NexusResponse r = nexus_request("GET", url, "", {}, 20000);
        int w = 0, h = 0, n = 0;
        if (r.status != 200 || r.body.empty() || r.body.size() >= (size_t)INT32_MAX) {
        } else if (stbi_info_from_memory((const stbi_uc *)r.body.data(), (int)r.body.size(), &w, &h, &n)) {
            if (w > 0 && h > 0 && w <= kPictureMaxW && h <= kPictureMaxH) {
                if (unsigned char *px = stbi_load_from_memory((const stbi_uc *)r.body.data(), (int)r.body.size(), &w, &h, &n, 4)) {
                    if (w > 0 && h > 0 && w <= kPictureMaxW && h <= kPictureMaxH) {
                        d.w = w;
                        d.h = h;
                        d.px.assign(px, px + (size_t)w * (size_t)h * 4);
                    }
                    stbi_image_free(px);
                }
            }
        } else if (decode_webp(r.body, &w, &h, &d.px)) {
            d.w = w;
            d.h = h;
        }
    }
    std::lock_guard<std::mutex> l(g_picMu);
    g_picDone.push_back(std::move(d));
}

void pictures_free()
{
    for (auto &kv : g_pics) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_pics.clear();
    g_picGen++;
    std::lock_guard<std::mutex> l(g_picMu);
    g_picDone.clear();
}

}  // namespace

// (UI, once a frame from fab_state_poll) Online play followed, pictures made into textures (and freed when the screen
// closes), the account's messages said
static void store_poll(App &a)
{
    const bool on = online_on(a);
    if (g_online && !on) {   // turned off: Nexus work stops (downloads cancelled, the browser wait ended, sign-in cancelled)
        for (auto &j : g_jobs) {
            const int ph = j->phase;
            if (j->nexus.kind != "nexus" || (ph != FabJob::Downloading && ph != FabJob::Pickup)) continue;
            fab_cancel(j);
            set_message(a, trf("Online play is off - getting {1} from Nexus Mods stopped.", { j->name }), false);
        }
        fab_nexus_sign_in_cancel();
    }
    if (!g_online && on) {   // back on: the "Online play is off." answers are asked again, the pictures start over
        fab_store_forget_failures();
        pictures_free();
    }
    g_online = on;
    g_picFrame++;
    std::vector<Decoded> done;
    {
        std::lock_guard<std::mutex> l(g_picMu);
        done.swap(g_picDone);
    }
    for (Decoded &d : done) {
        auto it = g_pics.find(d.url);
        if (d.gen != g_picGen || it == g_pics.end() || d.px.empty() || !a.ren) continue;
        SDL_Texture *t = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, d.w, d.h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, d.px.data(), d.w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        it->second.tex = t;
        it->second.w = d.w;
        it->second.h = d.h;
    }
    if (a.screen != SCR_FABRICATOR && (!g_pics.empty() || !done.empty())) pictures_free();
    int msg;
    std::string name;
    {
        std::lock_guard<std::mutex> l(g_storeMu);
        msg = g_acc.msg;
        g_acc.msg = 0;
        name = g_acc.s.name;
    }
    if (msg == 1) set_message(a, name.empty() ? std::string(tr("Signed in to Nexus Mods.")) : trf("Signed in to Nexus Mods as {1}.", { name }), false);
    else if (msg == 2) set_message(a, "Signed out of Nexus Mods.", false);
}

bool fab_store_online(const App &a) { return online_on(a); }

void fab_store_open(App &a)
{
    if (!online_on(a)) return;
    account_tick(a);
    if (g_gamesStarted) return;
    g_gamesStarted = true;
    {
        std::lock_guard<std::mutex> l(g_storeMu);
        g_games.looking = true;
        g_games.problem.clear();
    }
    store_worker().post([] { run_games(); });
}

FabStoreGames fab_store_games()
{
    std::lock_guard<std::mutex> l(g_storeMu);
    return g_games;
}

void fab_store_games_retry()
{
    std::lock_guard<std::mutex> l(g_storeMu);
    if (!g_games.looking) g_gamesStarted = false;
}

FabStoreList fab_store_list(App &a, const NexusQuery &q)
{
    if (!online_on(a) || q.domain.empty()) return {};
    const std::string key = list_key(q);
    std::lock_guard<std::mutex> l(g_storeMu);
    if (key != g_listKey) {
        g_listKey = key;
        g_list = FabStoreList();
        g_list.q = q;
        auto it = g_lists.find(key);
        if (it != g_lists.end() && SDL_GetTicks() - it->second.at < kStoreCacheMs) {
            g_list.answered = true;
            g_list.mods = it->second.mods;
            g_list.total = it->second.total;
        } else {
            g_list.loading = true;
            store_worker().post([key, q] { run_list(key, q); });
        }
    }
    return g_list;
}

FabStorePage fab_store_page(App &a, const std::string &domain, int game_id, int64_t mod_id)
{
    if (!online_on(a) || domain.empty() || game_id <= 0 || mod_id <= 0) return {};
    const std::string key = domain + '\x1f' + std::to_string(mod_id);
    std::lock_guard<std::mutex> l(g_storeMu);
    if (key != g_pageKey) {
        g_pageKey = key;
        g_page = FabStorePage();
        g_page.domain = domain;
        g_page.mod_id = mod_id;
        auto it = g_pages.find(key);
        if (it != g_pages.end() && SDL_GetTicks() - it->second.at < kStoreCacheMs) {
            g_page.answered = true;
            g_page.mod = it->second.mod;
            g_page.files = it->second.files;
            g_page.files_ok = true;
        } else {
            g_page.loading = true;
            store_worker().post([key, game_id, mod_id] { run_page(key, game_id, mod_id); });
        }
    }
    return g_page;
}

void fab_store_forget_failures()
{
    std::lock_guard<std::mutex> l(g_storeMu);
    if (!g_list.loading && (!g_list.problem.empty() || !g_list.answered)) g_listKey.clear();
    if (!g_page.loading && (!g_page.problem.empty() || !g_page.files_ok)) g_pageKey.clear();
    if (!g_games.looking && !g_games.problem.empty()) g_gamesStarted = false;
}

SDL_Texture *fab_store_picture(App &a, const std::string &url, int *w, int *h)
{
    if (url.empty() || !online_on(a) || !nexus_host_allowed(url)) return nullptr;
    auto it = g_pics.find(url);
    if (it != g_pics.end()) {
        it->second.used = g_picFrame;
        if (w) *w = it->second.w;
        if (h) *h = it->second.h;
        return it->second.tex;
    }
    if (g_pics.size() >= kPicturesMax) {   // the least recently drawn one makes room (never one drawn this frame)
        auto old = g_pics.end();
        for (auto i = g_pics.begin(); i != g_pics.end(); ++i)
            if (i->second.used != g_picFrame && (old == g_pics.end() || i->second.used < old->second.used)) old = i;
        if (old == g_pics.end()) return nullptr;
        if (old->second.tex) SDL_DestroyTexture(old->second.tex);
        g_pics.erase(old);
    }
    Picture p;
    p.used = g_picFrame;
    g_pics[url] = p;
    const int gen = g_picGen;
    picture_worker().post([url, gen] { run_picture(url, gen); });
    return nullptr;
}

FabNexusAccountState fab_nexus_account_state()
{
    static const bool available = !nexus_client_id().empty();
    std::lock_guard<std::mutex> l(g_storeMu);
    FabNexusAccountState s = g_acc.s;
    s.available = available;
    s.allow_adult = s.signed_in && g_acc.adult.has_value() && *g_acc.adult;
    return s;
}

void fab_nexus_sign_in(App &a)
{
    if (!online_on(a) || nexus_client_id().empty()) return;
    std::lock_guard<std::mutex> l(g_storeMu);
    if (g_acc.s.signing_in || g_acc.s.signing_out) return;   // one sign-in at a time (port 28654)
    g_acc.s.signing_in = true;
    g_acc.s.problem.clear();
    auto c = std::make_shared<std::atomic<bool>>(false);
    g_acc.cancel = c;
    std::thread([c] {
        NexusAccount acc;
        const std::string e = nexus_sign_in(*c, &acc);
        std::lock_guard<std::mutex> l(g_storeMu);
        g_acc.s.signing_in = false;
        if (g_acc.cancel == c) g_acc.cancel.reset();
        if (e.empty()) {
            g_acc.s.signed_in = true;
            g_acc.s.name = acc.name;
            g_acc.adult.reset();
            g_acc.pref_asked = false;
            g_acc.gen++;
            g_acc.missing = 0;
            g_acc.msg = 1;
        } else if (!*c) g_acc.s.problem = e;   // (Cancel pressed: nothing to say)
    }).detach();
}

void fab_nexus_sign_in_cancel()
{
    std::lock_guard<std::mutex> l(g_storeMu);
    if (g_acc.cancel) *g_acc.cancel = true;
}

void fab_nexus_sign_out(App &a)
{
    const bool online = online_on(a);
    std::lock_guard<std::mutex> l(g_storeMu);
    if (g_acc.s.signing_in || g_acc.s.signing_out || !g_acc.s.signed_in) return;
    g_acc.s.signing_out = true;
    std::thread([online] {
        if (online && g_online) nexus_sign_out();   // revoked at Nexus Mods (best effort), then deleted here
        else nexus_delete_tokens();                 // Online play off: no request, deleted here only
        std::lock_guard<std::mutex> l(g_storeMu);
        g_acc.s.signing_out = false;
        g_acc.s.signed_in = false;
        g_acc.s.name.clear();
        g_acc.s.problem.clear();
        g_acc.adult.reset();
        g_acc.pref_asked = false;
        g_acc.gen++;
        g_acc.missing = 0;
        g_acc.msg = 2;
    }).detach();
}
#endif  // COGBOX_FAB_NEXUS
