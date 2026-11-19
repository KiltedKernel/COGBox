// CogBox - getting a Nexus Mods file for the Fabricator (COGBOX_FAB_NEXUS builds only): the Premium download (REST v1
// download link, then the file streamed from the CDN), the free flow's pickup of the file the player's browser saved in
// Downloads, and the update check of an installed Nexus mod against the mod's files. Download addresses and tokens are
// never logged and never in problem text. The download blocks: call it from the worker. test/nexus_download_test.cpp.
#pragma once
#ifdef COGBOX_FAB_NEXUS

#include "fabricator_pack.h"   // FabProgress
#include "nexus_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Premium: the file's download link asked for with the signed-in player's token, then the first link (https on Nexus
// Mods' hosts only, without the token) streamed to dest_zip through dest_zip.part and a rename. "" when dest_zip is
// written; "not premium" on 401 / 403 or when nobody is signed in (the caller falls back to the free flow);
// "cancelled" (p.cancel); else a short problem for the player.
std::string nexus_try_premium_download(const std::string &domain, int64_t mod_id, int64_t file_id, const std::string &dest_zip,
                                       FabProgress &p);

// The free flow: the player downloads the file in their browser (nexus_mod_url's Files tab); COGBox looks for it in
// their Downloads folder (FOLDERID_Downloads, "" if Windows doesn't say).
std::string nexus_downloads_folder();

struct NexusPickup {
    std::string expect_name;   // the file name expected (compared without case)
    int64_t started_at = 0;    // Unix time the pickup started
    // Also matched: a name whose stem (before the extension and any " (n)") ends with this, e.g. "-27-1-0-1787649366"
    // (Nexus Mods names a download <archive>-<mod id>-<version>-<upload time>.<ext>). "" = exact names only.
    std::string expect_tail;
};
// The newest file in folder modified after started_at - 60 s whose name is expect_name (without case), or that name
// with the browser's " (n)" before the extension, or one ending in expect_tail. Folders, empty files and unfinished
// downloads (.crdownload / .part / .tmp / .partial / .download / .opdownload) are skipped; nothing is read. Only
// names matching the expected ones are listed (FindFirstFile patterns). An expect_name or tail with a path or wildcard
// character matches nothing. The caller waits until the file's size stays the same for 2 s before using it.
std::optional<std::string> nexus_find_download(const std::string &folder, const NexusPickup &p);
// The tail a Nexus Mods download of file f of mod mod_id ends with ("" when f has no upload time): "-<mod id>-<version
// with each character other than a letter or digit as '-'>-<upload Unix time>".
std::string nexus_download_tail(int64_t mod_id, const NexusFile &f);

// The update check: a file newer than the installed one. A candidate is in the MAIN category or has the installed
// file's name stem (its name without a trailing version), is not OLD_VERSION / ARCHIVED / DELETED, is not the installed
// file, and was uploaded after it or has a higher version than installed_version. The newest candidate wins.
// nullopt: no files to compare with.
struct NexusUpdate {
    bool available = false;
    NexusFile file;
};
std::optional<NexusUpdate> nexus_find_update(const std::vector<NexusFile> &files, int64_t installed_file_id,
                                             const std::string &installed_version);

// Version order: the numbers in a and b compared in turn ("1.10" > "1.9", "v2" = "2", "1.0" = "1"); letters are not
// counted. < 0, 0, > 0; 0 also when either has no digit (not comparable).
int nexus_version_compare(const std::string &a, const std::string &b);

#endif  // COGBOX_FAB_NEXUS
