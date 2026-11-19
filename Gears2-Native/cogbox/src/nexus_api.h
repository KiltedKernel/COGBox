// CogBox - Nexus Mods for the Fabricator (COGBOX_FAB_NEXUS builds only): the GraphQL v2 request bodies for browsing
// (https://api.nexusmods.com/v2/graphql), the parsers for their answers, the REST v1 download link, deep links and the
// game domains. No network here (nexus_net.h sends). The parsers treat the server's data as hostile: wrong types take
// defaults, numbers clamp, text becomes plain text of bounded size, picture addresses outside Nexus Mods' https hosts
// are dropped. test/nexus_api_test.cpp.
#pragma once
#ifdef COGBOX_FAB_NEXUS

#include "games.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

constexpr const char *kNexusGraphqlUrl = "https://api.nexusmods.com/v2/graphql";

struct NexusMod {
    int64_t mod_id = 0;
    int game_id = 0;
    std::string domain, name, summary, description, author, uploader, version, picture, thumbnail, category, updated_at;
    int64_t endorsements = 0, downloads = 0;
    bool adult = false;
    std::vector<std::string> tags;
};

struct NexusFile {
    int64_t file_id = 0;
    std::string name, version, category, uploaded_at, description;   // category: MAIN, UPDATE, OPTIONAL, OLD_VERSION ...
    int64_t size_bytes = 0;
    bool primary = false;
};

struct NexusQuery {
    std::string domain, text, sort = "endorsements";   // sort: endorsements | downloads | updatedAt | createdAt | name
    bool cogbox_only = false, allow_adult = false;
    int offset = 0, count = 20;
};

// GraphQL request bodies: JSON {"query": ..., "variables": {...}} (the player's text goes in variables only).
// mods: gameDomainName EQUALS domain; adultContent EQUALS false unless allow_adult; tag EQUALS "COGBox" when
// cogbox_only; nameStemmed MATCHES text when there is text. Sorted DESC (name: ASC); count 1..100.
std::string nexus_gql_mods(const NexusQuery &q);
std::string nexus_gql_mod(int64_t mod_id, int game_id);     // one mod, with its description
std::string nexus_gql_files(int64_t mod_id, int game_id);   // its files
std::string nexus_gql_game(const std::string &domain);      // game(domainName:) { id domainName name }
std::string nexus_gql_preferences();                         // preferences { adult } (needs the player signed in)

// Parsers: nullopt for a body that is not a JSON object, or a GraphQL errors list without the asked-for data.
std::optional<std::vector<NexusMod>> nexus_parse_mods(const std::string &body, int *total);
std::optional<NexusMod> nexus_parse_mod(const std::string &body);   // also nullopt when there is no such mod
std::optional<std::vector<NexusFile>> nexus_parse_files(const std::string &body);   // files without an id are left out
std::optional<int> nexus_parse_game_id(const std::string &body);    // nullopt: no such game
std::optional<bool> nexus_parse_adult_pref(const std::string &body); // nullopt: unknown (signed out, missing, mistyped)

// REST v1: GET this with Authorization: Bearer <token> for a Premium download. "" for a bad domain or id.
std::string nexus_rest_download_link_url(const std::string &domain, int64_t mod_id, int64_t file_id);
// Its answer: the URI of each entry, only https on allowed hosts (nexus_host_allowed). nullopt: not a JSON list.
std::optional<std::vector<std::string>> nexus_parse_download_links(const std::string &body);

// https://www.nexusmods.com/{domain}/mods/{mod_id}, plus ?tab=files&file_id={file_id} when file_id > 0. "" for a bad
// domain or id.
std::string nexus_mod_url(const std::string &domain, int64_t mod_id, int64_t file_id = 0);
// HTML or BBCode as plain text: tags and BBCode removed (script / style / img / youtube content too), basic entities
// decoded, whitespace collapsed (at most one empty line), cut to max_bytes without splitting a UTF-8 character.
std::string nexus_plain_text(const std::string &html_or_bbcode, size_t max_bytes);
// COGBox game (GameId) <-> Nexus Mods game domain: gearsofwar, gearsofwar2, gearsofwar3, gearsofwarjudgement.
const char *nexus_domain_for_game(int g);    // "" for none
int nexus_game_for_domain(const std::string &domain);   // -1 for none

#endif  // COGBOX_FAB_NEXUS
