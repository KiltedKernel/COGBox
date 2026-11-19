# Nexus Mods integration (COGBox Fabricator)

This page describes exactly what COGBox does with Nexus Mods' services. It is written for Nexus Mods (for the app
registration) and for COGBox maintainers.

**Status:** the integration is in the source behind a build switch (`COGBOX_FAB_NEXUS`). This is the test build sent
with COGBox's API registration request. Released COGBox builds do not contain it, and sign-in stays disabled until Nexus
Mods issues a client ID (someone who builds with the switch can browse without one). COGBox is not made, endorsed or
sponsored by Nexus Mods.

## What it is

The **FABRICATOR** screen of the COGBox launcher is a local mod manager for four Xbox 360 game ports (Gears of War,
Gears of War 2, Gears of War 3 and Gears of War: Judgment). Its **STORE** tab, in the test build, lets a player browse
the mods listed for those four games on Nexus Mods, open a mod's page, and get a file. COGBox is a Windows desktop
program, not a web service. Each player runs it on their own PC, and every request below comes from that PC.

Game domains used: `gearsofwar`, `gearsofwar2`, `gearsofwar3`, `gearsofwarjudgement`.

## Rules COGBox follows

- **Online play must be on.** The STORE works only when the player has turned on COGBox's "Online play" setting. With it
  off, COGBox makes no Nexus Mods request of any kind (the tab says "Turn on Online play to browse Nexus Mods."). If the
  player turns it off while something runs, downloads waiting for the browser are cancelled, a sign-in is cancelled,
  and an update check stops before its next request.
- **Player-started requests only**, plus the games lookup (below) when the STORE tab is first opened in a run, and the
  pictures of the cards on screen. There are no background or scheduled requests, and none at startup.
- **https only**, to `api.nexusmods.com`, `users.nexusmods.com`, and hosts ending in `.nexusmods.com` or
  `.nexus-cdn.com` (pictures and file downloads). Any other address, any port other than 443, an address with a user
  name in it, and any redirect are refused: COGBox does not follow redirects. The one exception is the sign-in
  callback, a loopback listener on the player's own PC (see "Sign-in").
- **Every request carries** `Application-Name: COGBox` and `Application-Version: <the COGBox version>`, and the user
  agent `COGBox/<version>`.
- **Answers are capped at 8 MiB** (file downloads at 4 GiB, streamed to disk). Every API and picture request has an
  overall time limit; a file download has a stall limit instead (30 seconds without data on one read), and the player
  can cancel it.
- **The `Authorization` header goes only to `api.nexusmods.com` and `users.nexusmods.com`.** The transport drops it for
  any other host (CDN and picture hosts), whatever the caller passes.
- **Hostile data is not trusted.** Names, summaries and descriptions are turned into plain text (HTML and BBCode are
  removed, lengths are capped) and are never used as formats or addresses. Picture and download addresses that are not
  https on the hosts above are ignored.
- **No scraping, no uploads, no `nxm://` links.** COGBox does not read nexusmods.com web pages, does not upload,
  endorse, track or comment on anything, and does not register an `nxm://` protocol handler.
- **No disk cache.** List and page answers are kept in memory for 5 minutes (at most 64 of each) and are lost when
  COGBox closes. The only things written to disk are the sign-in tokens (Credential Manager), downloaded files
  (temporarily, until installed), and, in an installed mod's `fabricator.json`, the Nexus game domain, mod ID and file
  ID it came from (used by "Check for updates").

## Every request

All GraphQL requests are `POST https://api.nexusmods.com/v2/graphql` with a JSON body `{query, variables}`. A player's
search text goes only in `variables`.

| Operation | When | Auth |
|---|---|---|
| `query Game($domain)`: `game(domainName)` { id domainName name } | Once per game domain per COGBox run (four, for the four games) when the STORE tab is first opened with Online play on, to find the games' IDs. Asked again only if the answer failed. Also used by "Check for updates" if the ID is not known yet. | none |
| `query Mods(...)`: `mods(filter, sort, offset, count)` | When the STORE shows a list that is not in the 5-minute memory cache: on opening a game, after the search box changes (sent 400 ms after typing stops), and when the sort, the "Made for COGBox" switch or the page changes. 12 mods per page. The first list for a game is asked with the tag `COGBox`; if that has no mods, it is asked again without the tag. | none |
| `query Mod(...)`: `mod(modId, gameId)` | When the player opens a mod's page (unless cached). | none |
| `query ModFiles(...)`: `modFiles(modId, gameId)` | Right after `Mod`, for the same page. | none |
| `query ModFiles(...)` | When the player presses "Check for updates" in the LIBRARY: one request for each mod COGBox itself added from Nexus Mods (at most 100). | none |
| `query Preferences`: `preferences { adult }` | Once per COGBox run when the STORE is first opened while signed in, and once after each sign-in, to read the player's adult-content setting. Getting the token for it can also send a token refresh (below) when the token has less than 5 minutes left. | `Authorization: Bearer <access token>` |

Other requests:

| Request | When | Auth |
|---|---|---|
| `GET https://api.nexusmods.com/v1/games/{domain}/mods/{mod_id}/files/{file_id}/download_link.json` | The player presses **Get** on a file while signed in. A 401 or 403 (no Premium) means the free flow below. | `Authorization: Bearer <access token>` |
| `GET <download link>` on an allowed Nexus Mods CDN host (as returned by the request above) | Straight after that, for Premium accounts. Streamed to a temporary `.part` file, then renamed. If a link fails, the next one in the answer is tried (one more request per failed link). | none (the file download drops the Authorization header for every host except `api.nexusmods.com`) |
| `GET <picture address>` on an allowed Nexus Mods host | For the thumbnails of the cards on screen, and a mod's picture on its page. Pictures that are not shown are not fetched. Up to 12 per page of cards. Kept as textures while the FABRICATOR is open and freed when it closes. | none |
| `POST https://users.nexusmods.com/oauth/token` | Sign-in (code exchange), and later a refresh when the access token has less than 5 minutes left and COGBox needs it. | client ID in the form body, no secret |
| `GET https://users.nexusmods.com/oauth/userinfo` | Once, right after sign-in, to show the account name. | `Authorization: Bearer <access token>` |
| `POST https://users.nexusmods.com/oauth/revoke` | When the player presses Sign out (best effort). | client ID and the token in the form body |

Opening a page in the player's web browser is not a request from COGBox. COGBox opens
`https://www.nexusmods.com/{domain}/mods/{mod_id}` (with `?tab=files&file_id={file_id}` for a file) when the player
presses "View on Nexus Mods", or "Get" without Premium, and only addresses beginning with `https://www.nexusmods.com/`.
The sign-in page `https://users.nexusmods.com/oauth/authorize?...` is opened the same way.

## Free (non-Premium) downloads

COGBox does not go around Nexus Mods' download page. Without Premium (or when signed out), "Get" opens the file's page on
nexusmods.com in the player's browser, and the player downloads the file there as for any mod. COGBox then looks in the
player's Downloads folder (every 2 seconds, for up to 30 minutes, listing file names only) for a finished file whose
name fits the mod's file pattern (`<name>-<mod id>-<version>-<time>.<ext>`), skips partial downloads, and asks "Add
this file?". The player can also choose the file by hand. COGBox never deletes a file from the Downloads folder.

COGBox installs `.zip` files only. A `.7z` or `.rar` file is recognised and refused with a message that asks the player
to get a `.zip` from the mod's author, or to unpack it and add the folder.

## Sign-in (OAuth 2.0)

- **Client type:** public client with PKCE (`S256`). No client secret exists or is stored.
- **Endpoints:** `https://users.nexusmods.com/oauth/authorize`, `/oauth/token`, `/oauth/revoke`, `/oauth/userinfo`.
- **Redirect URI:** `http://127.0.0.1:28654/callback`. COGBox listens on that loopback address and port (exclusive,
  127.0.0.1 only) only while a sign-in is waiting, for at most 5 minutes. It accepts only `GET /callback` with the
  expected `state`; anything else gets an error page and the wait goes on. If the port is in use, COGBox says so and does
  not open the browser.
- **Scopes:** `openid public`.
- **Flow:** the browser opens the authorize page; the player approves; the callback gives COGBox a code; COGBox
  exchanges it (with the PKCE verifier) for tokens and reads the account name from userinfo.
- **Token storage:** Windows Credential Manager, generic credentials named `COGBox/NexusMods/0`, `/1`, ... (the data is
  split in chunks of 2000 bytes), kept for this Windows user on this PC. They hold the access token, refresh token, id
  token, expiry and account name. Tokens, codes, PKCE verifiers and download links are never written to a log, an error
  message or the screen.
- **Refresh:** when COGBox needs a token and it has less than 5 minutes left, it is refreshed once (refreshes are
  serialised) and a new refresh token is stored. If Nexus Mods refuses the refresh (a 4xx answer other than 408 or 429),
  COGBox deletes the stored tokens and tells the player they were signed out (also when it happens during Get or an
  Update: the action stops with that message instead of falling back to the browser). If Nexus Mods cannot be reached (5xx or
  no answer), the tokens are kept and the action fails with a message.
- **Sign-out:** the stored tokens are deleted first, then a revoke request is sent with the copy in memory (best
  effort; the answer does not matter, and the tokens are already gone if COGBox closes meanwhile). With Online play off,
  the tokens are only deleted.
- **Client ID:** COGBox has none yet. Until Nexus Mods issues one, the Sign in button is disabled and says "Sign in -
  coming soon", and nothing listens on the port. Browsing and free downloads do not need a sign-in.

## Adult content

Adult mods are hidden unless the player is signed in and the `adult` value from `preferences` is `true`. Signed out, or
with the value false, missing or unreadable, COGBox asks for `adultContent = false` in every list, drops any mod that
comes back marked adult (a mod whose flag is missing counts as adult), and shows no picture for one.

## Rate use per session

These are counts from the design and from a smoke run against the live API, not measurements over many sessions. A
typical STORE visit:

- Opening the STORE: 4 `Game` requests, 1 or 2 `Mods` requests, up to 12 picture requests, and, when the player is
  signed in, 1 `Preferences` request (plus 1 token refresh if the token is within 5 minutes of expiry). Opening it again
  in the same run reuses the game IDs and the preference.
- Each further page of cards, search or sort: 1 `Mods` request and up to 12 pictures. Going back to a list seen in the
  last 5 minutes makes no request.
- Opening a mod: 2 API requests (`Mod`, `ModFiles`) and 1 picture.
- Getting a file: 0 API requests when signed out (the browser does the rest); 1 when signed in without Premium (the
  link request, answered 403, plus a possible token refresh), then the browser; with Premium, the link request and then
  the file (plus a possible token refresh, and one more file request for each link that fails).
- Checking updates: up to 4 `Game` requests (usually already known) and 1 `ModFiles` request per Nexus mod, once per
  press.
- Signing in: 1 token, 1 userinfo and 1 `Preferences` request. A refresh adds 1 token request when a token is needed
  within 5 minutes of its expiry.

A ten-minute browse is therefore around 20 to 40 API requests and under 150 picture requests. A failed answer is not
cached and is asked again only when the player presses Try again or repeats the action; there is no retry loop.

## Building the test version

The Nexus code is compiled only when asked for. From `Gears2-Native/cogbox`:

```
COGBOX_FAB_NEXUS=1 sh build.sh
```

Without `COGBOX_FAB_NEXUS=1`, no `src/nexus_*.cpp` file is built and the launcher is the local Fabricator.

The OAuth client ID is not in the source. Set the environment variable `COGBOX_NEXUS_CLIENT_ID` when running `build.sh`
and it becomes `-DCOGBOX_NEXUS_CLIENT_ID="..."` (only letters, digits, `.`, `_` and `-` are accepted, otherwise the
build stops). At run time the environment variable `COGBOX_NEXUS_CLIENT_ID` overrides the built-in value. With no client
ID, sign-in is disabled.

Unit tests are in `Gears2-Native/cogbox/test/nexus_api_test.cpp`, `nexus_auth_test.cpp` and `nexus_download_test.cpp`
(each file's header comment has its build line). They run against a local mock server on 127.0.0.1; the hook that points
the API at a local server exists only in those test builds. The one exception is `nexus_api_test.exe --live`, which
sends two requests to the real API (`Game` and `Mods` for `gearsofwar3`) with `Application-Version: 0.0.0-test`; it
runs only when that option is given.

## What is not done

- No `nxm://` handler and no Nexus Mods app integration.
- No uploads, endorsements, comments, tracking or any other write to a Nexus Mods account.
- No scraping of web pages and no automation of the download page.
- No disk cache of Nexus Mods data (answers live in memory for 5 minutes).
- No `.7z` or `.rar` installs (`.zip` only).
- No Nexus Mods data is sent to COGBox's own servers (see `PRIVACY.md`).
