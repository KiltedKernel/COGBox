// COGBox - the FABRICATOR screen's state (the local mod manager): one worker thread for the long parts, the jobs and
// their progress, and the LIBRARY's view of each game's mods folder. Everything is local: nothing is fetched or sent.
// (COGBOX_FAB_NEXUS builds add the Nexus Mods jobs below: their requests run on the worker, with Online play on.)
//
// The hand-off: the UI thread makes a job and posts it to the worker. The worker touches only the job and the files it
// names (a temporary zip in <exe dir>\fabricator\work, a <mods>\<folder>.partial, the zip a creator chose) - never App,
// mods_list or CogBox.ini - and then sets the job's phase. fab_state_poll (the UI thread, once a frame) moves each job
// on: an install's folder is chosen there (fab_install_prepare) and its short final moves are made there
// (fab_install_commit), so the mods folders' order and on/off list are only ever written by the UI thread.
// One install at a time (two adds of one name would choose the same folder).
#pragma once
#include "app.h"
#include "fabricator_install.h"
#include "fabricator_model.h"
#include "fabricator_pack.h"
#ifdef COGBOX_FAB_NEXUS
#include "nexus_api.h"
#include "nexus_download.h"

#include <map>
#endif

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct FabJob {
    enum Kind { Add, Pack, Check };
    enum Phase {
        Queued,
        Packing,     // Add (a folder): packed to a temporary zip first; Pack: the Creator's zip
        Checking,    // Add: the zip checked (fab_check_zip) and hashed; Check: the Creator's folder checked
        Checked,     // Add: the UI thread decides - a new install, or asks about an update
        Asking,      // Add: "Update <name> <old> to <new>?" is open
        Waiting,     // Add: ready to install; waits for the game to close and for another install to finish
        Unpacking,   // Add: unpacked into <folder>.partial on the worker
        Unpacked,    // Add: the UI thread moves it into place (waits while the game runs)
        Done, Failed, Cancelled,
#ifdef COGBOX_FAB_NEXUS
        Downloading,   // Nexus: the Premium download into work_dir on the worker (then Checking)
        Pickup,        // Nexus: the file's page is open in the browser; the UI thread looks in Downloads every 2 s
                       // (30 min) for the file, then Queued -> Checking with zip = that file (never removed by COGBox)
#endif
    };
    int id = 0;
    Kind kind = Add;
    std::atomic<int> phase{Queued};
    FabProgress progress;
    std::string name;            // what the screen calls it (the file or folder name until mod.ini names it)

    // Add
    std::string source;          // the zip or folder the player gave
    bool from_folder = false;
    int want_game = -1;          // the LIBRARY's game; a mod.ini Game for another game wins
    std::string update_folder;   // a row's Update: the installed folder the zip must be a new version of
    std::string zip, work_dir;   // the zip installed; work_dir: COGBox's temporary folder for a packed one ("" none)
    FabZipInfo info;             // (worker, before Checked)
    bool other_game = false;     // (worker) mod.ini names another game than want_game
    std::string sha256;          // (worker)
    int game = -1;               // (UI) the game it goes to
    std::string old_version;     // (UI) an update: the installed version
    bool decided_update = false; // (UI) Checked found an installed version (and the player said Update)
    FabInstallPlan plan;         // (UI) written before Unpacking, read by the worker
    std::vector<std::string> left_out;   // (worker) program files the unpack left out
    std::vector<std::string> pack_left_out;   // (worker) a folder's program files the pack left out
    bool told_close = false;     // "Close the game first." said for this job
    bool said = false;           // (UI) its failure was said (a failed job leaves the list once it is)

    // Pack: source (the folder) into zip. Check: source (the folder) for game_key.
    std::string game_key;
    std::vector<std::string> problems;   // Failed: why (Add, Pack); Check: what the folder has to fix (empty = clean)

#ifdef COGBOX_FAB_NEXUS
    // Add from Nexus Mods: the record's source (kind "nexus", its game domain, mod and file); kind "file" otherwise
    FabSource nexus;
    // (UI) Pickup: the page opened yet, the Downloads folder and the expected names, until when (SDL ticks), the next
    // look, and the file seen at the last look with its size (taken once it is the same two looks running)
    bool pickup_opened = false;
    std::string pickup_folder;
    NexusPickup pickup;
    Uint64 pickup_until = 0, pickup_next = 0;
    std::string pickup_seen;
    int64_t pickup_size = -1;
#endif
};

// A dropped or chosen .zip or folder: added to the LIBRARY's game (or the game its mod.ini names).
void fab_add_path(App &a, const std::string &path);
// A row's Update: the zip must be a new version of the mod installed in folder (game g).
void fab_update_from(App &a, int g, const std::string &folder, const std::string &zip);
// The Creator: folder packed into zip_path (the job's progress, Cancel), and folder checked for game_key.
std::shared_ptr<FabJob> fab_pack_start(const std::string &folder, const std::string &zip_path, const std::string &name);
std::shared_ptr<FabJob> fab_check_start(const std::string &folder, const std::string &game_key);
void fab_cancel(const std::shared_ptr<FabJob> &j);   // Queued, Packing, Checked, Asking, Waiting, Unpacked
bool fab_can_cancel(const FabJob &j);
// The jobs running (adds and the Creator's zips), oldest first; a finished one leaves the list once fab_state_poll said
// how it went.
std::vector<std::shared_ptr<FabJob>> fab_jobs();
// The first running job's percent for the front screen's button: 0-100, -2 running without a figure, -1 none.
int fab_job_percent();
bool fab_job_running(const FabJob &j);

// The LIBRARY: one game's mods in load order, each with COGBox's record (none: added by hand) and its mod.ini Type.
struct FabLibRow {
    ModItem m;
    std::optional<FabInstallRecord> rec;
    std::string type;
};
int fab_library_game(const App &a);   // the game filter (GameId); the game in front at first
void fab_library_set_game(int g);
const std::vector<FabLibRow> &fab_library(App &a, int g);   // read again every 2 s and after fab_library_changed
void fab_library_changed();

#ifdef COGBOX_FAB_NEXUS
// Nexus Mods (each does nothing with Online play off but say so; every request runs on the worker):
// fab_nexus_get: file f of mod m for game g. The Premium download first (signed in); on "not premium" the file's page
// (nexus_mod_url) opens in the browser and the job waits up to 30 min for the file in Downloads (FabJob::Pickup). Then
// the usual check and install (an update when that Nexus mod is installed), recorded with the nexus source.
void fab_nexus_get(App &a, int g, const NexusMod &m, const NexusFile &f);
// The manual fallback ("Choose the file..."): the zip the player chose is file f of mod m. A Pickup job of that file
// takes it; else a new add with the nexus source. No request.
void fab_nexus_choose_file(App &a, int g, const NexusMod &m, const NexusFile &f, const std::string &zip);
// "Check for updates": each LIBRARY row of game g with a nexus source, its mod's files asked for on the worker and
// compared with nexus_find_update. fab_nexus_updates(g) has the answers by mod folder; fab_nexus_update(g, folder)
// gets the update found for that row (fab_nexus_get).
void fab_nexus_check_updates(App &a, int g);
struct FabNexusUpdates {
    bool checking = false;
    std::string problem;                             // "" or why a check could not finish (the first problem)
    std::map<std::string, NexusUpdate> by_folder;    // the rows checked (available = an update was found)
};
FabNexusUpdates fab_nexus_updates(int g);
void fab_nexus_update(App &a, int g, const std::string &folder);
#endif

// The Fabricator's questions (ask tags "fabupdate", "fabuninstall"), from main.cpp's modal answers.
bool fab_answer(App &a, const Modal &m);   // false: not one of them
void fab_uninstall_ask(App &a, int g, const ModItem &m);

// Once a frame on the UI thread (main.cpp): jobs moved on, messages said.
void fab_state_poll(App &a);
// At exit: packs cancelled, the worker given a moment, unfinished installs' .partial folders and temporary zips removed
// (an unpack still running leaves its .partial: the next start's sweep removes it).
void fab_state_shutdown();
