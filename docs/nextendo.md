# Nextendo Network integration (T22)

This branch adds an opt-in **Online: Nextendo Network** setting, disabled by
default. It implements the community service's client protocol in C++, without
Nintendo accounts, console certificates, PRODINFO, device keys, or Nintendo tokens.
Online races have **not** been validated on Xbox. A successful host test is not
proof that matchmaking, TLS, UDP NAT traversal, or a race lobby works.

## Sources and attribution

OAuth contract aligned on 2026-10-18 with the
[official developer documentation](https://wiki.nextendo.network/developers).
The OAuth access token authenticates account APIs; the separate NEX token
authenticates games through the BAAS `nnex` claim.

Original interoperability sources studied on 2026-10-12:

- [Ryujinx-Nextendo](https://github.com/NextendoNetwork/Ryujinx-Nextendo/tree/f6095ad5ce543fc00987609af28c3823ca65e31a),
  commit `f6095ad5ce543fc00987609af28c3823ca65e31a`.
- [Comparison with Ryubing Canary 1.3.337](https://github.com/NextendoNetwork/Ryujinx-Nextendo/compare/deb5dca...f6095ad5ce543fc00987609af28c3823ca65e31a),
  upstream base `deb5dca`: 116 additional commits and 273 changed files reported
  by GitHub. The relevant changes are DNS-MITM, account JWT/PKCE, nifm, friend
  serialization/presence, and UDP port retention. The fork also has changes outside
  networking; this branch does not import its emulation core.
- [Prelude-Nextendo](https://github.com/NextendoNetwork/Prelude-Nextendo/tree/1453b8c9a202401a20a889aac436525bac27d736),
  commit `1453b8c9a202401a20a889aac436525bac27d736`. The older Prelude-Nro URL
  redirects here. Its host configuration confirms the two-server NAT requirement.
- [Nextendo Network profile](https://github.com/NextendoNetworkProfile/NextendoNetwork),
  [website](https://nextendo.network), and
  [developer registration](https://nextendo.network/developers).

Upstream Ryujinx is MIT (`LICENSE.txt`), but the Nextendo additions and Prelude
are **PolyForm Shield 1.0.0** (`LICENSE.md`), not MIT. The profile repository has
no declared software license. No implementation, assets, game patches, or bundled
binaries from those projects are copied here. The C++ code independently encodes
observed host mappings, request/response fields, and IPC wire layouts under
NXbox's GPL-3.0-or-later license. This attribution records the interoperability
sources; it does not relicense their code.

## Changed files

| Files | Responsibility |
| --- | --- |
| `src/core/hle/service/nextendo/auth.h`, `crypto.h`, `friends.h`, `policy.h`, `runtime.h`, `token.h` | Portable protocol, routing, signing, profile isolation, social ABI |
| `src/core/hle/service/acc/acc.cpp`, `friend/friend.cpp`, `nifm/nifm.cpp`, `sockets/sfdnsres.cpp`, `ssl/ssl.cpp` | Opt-in IPC/DNS/SSL hooks |
| `src/eden_uwp/nextendo_online.cpp`, `nextendo_online.h`, `CMakeLists.txt` | WinRT HTTP, PasswordVault, background social worker, build registration |
| `src/eden_uwp/ui/nextendo_screen.cpp`, `nextendo_screen.h`, `library_screen.cpp`, `strings.h`, `lang/en.inc`, `lang/pt_br.inc` | Controller settings/login and PT/EN strings |
| `tests/port/test_t22_nextendo.py` | Executable C++ policy and mocked HTTP tests, LLVM coverage |
| `docs/nextendo.md` | Attribution, configuration, measurements and console test plan |

The original T22 integration changed 22 files. Renderer, common settings, core build registration,
application manifests, and other worktrees are untouched. Existing-file formatting
is isolated in commit `0404d53100`.

## Operator setup

Register an NXbox public PKCE application at the Nextendo developer portal, with
redirect URI `http://127.0.0.1:17822/callback` and the space-separated scopes
`identity friends presence game.matchmaking`.
Do not reuse Ryujinx's application identity or release attestation secrets.

Provide **public configuration only** in Xbox LocalState `nextendo.json`:

```json
{
  "server_address": "192.0.2.1",
  "nat_address": "198.51.100.2",
  "client_id": "your-registered-public-application-id"
}
```

The addresses above are documentation examples, not working Nextendo endpoints.
Obtain current, approved IPv4 addresses from the Nextendo operator. No production
address is inferred from the account website's IP. The two addresses must differ.
Missing, malformed, loopback, multicast, and link-local configurations fail closed:
Nintendo hostnames never fall through to native DNS. Nonsecret configuration may
be deployed through Device Portal's application file manager. Enabling online is
persisted separately in ApplicationData LocalSettings (`nextendo_enabled`).

A Nextendo-issued RSA signing key, if required by the operator, belongs in
PasswordVault resource `NXbox.Nextendo.SigningKey`, username `baas`, provisioned
out of band. No key import file or key is included in git. Without that credential,
the client creates an ephemeral RSA key, matching the fork's unconfigured
behavior. **Splatoon 2's local JWKS check will not accept a throwaway key.** Do not
substitute Nintendo key material. Invalid or unavailable credentials cannot grant
Nintendo online access.

## Controller flow and account isolation

Settings > Online: Nextendo Network opens the existing themed controller UI:

- A toggles online routing/state. Default is off. Missing setup keeps game DNS
  blocked rather than opening Nintendo connectivity.
- X opens the Nextendo browser login when signed out, or removes the saved
  Nextendo credential when signed in.
- After browser authorization, Menu opens the on-screen keyboard for the full
  final callback URL. The loopback page can show a connection error: there is no
  loopback listener in this manual-return implementation. Recovering the final
  address in Xbox's browser must be tested before recommending this flow.
- Y refreshes friends. B returns to Settings.

The published client uses authorization-code PKCE, not a device-code endpoint.
NXbox does not invent a device grant, password API, QR endpoint, or anonymous guest
identity. Codes are single use, state is checked, duplicate parameters are rejected,
and authorization expires after five minutes. The HTTP exchange uses a fixed
HTTPS origin, normal certificate validation, and no automatic redirects. The requested scope is
`identity friends presence game.matchmaking`; `app=nxbox` and the random state
remain in the authorization URL.

The authorization-code exchange POSTs a form to `/api/oauth/token`, requiring
both `access_token` and `refresh_token` and a positive `expires_in`. NXbox then
GETs `/api/nex-token` with `Authorization: Bearer <access_token>`. That response
supplies `pid`, `nex_token`, `username`, `friend_code`, and its own `expires_in`.
Only HTTP 404 from this GET permits the legacy combined token response
(`nex_token` plus `account` identity) as a fallback. Other failures fail sign-in.
The legacy NEX token uses the documented 24-hour lifetime.

PasswordVault resource `NXbox.Nextendo.Account`, username `session`, keeps the
refresh token, PID, username, friend code, and the active local profile's raw
UUID. The access token and NEX token live only in memory: Credential Locker caps
a credential near 2.5 KB, and both are fetched again at start. Access tokens
normally last one hour, refresh tokens 30 days, and NEX tokens 24 hours; a
response without `expires_in` uses those lifetimes. Tokens are bounded to 16 KiB
each and reject CR/LF before storage or use. Old vault sessions without an OAuth
refresh token require sign-in.

The library never waits on the network. At launch the stored account shows as
linked at once, and the social worker's first pass POSTs
`grant_type=refresh_token`, `refresh_token`, and `client_id` to
`/api/oauth/token`. The rotated tokens are kept in memory before the vault write,
so a failed write cannot lose them. The previous refresh token becomes invalid a
minute after rotation. The previous NEX token stays usable until its own expiry
or a replacement arrives. When only the NEX token is stale and the access token
has more than five minutes left, NXbox requests `/api/nex-token` again without
rotating the refresh token. A failed attempt waits 60 seconds before the next.
Refresh HTTP 400/401 clears the vault session and runtime identity and requires
sign-in again. A 401 from a social API only forces a refresh. Other failures
retain credentials for retry. Reopening the library over a running game keeps
the live session. Going-online and manual social failures use the existing fixed
localized failure message; background failures never display server text.

Every social request checks session freshness first. Access tokens within five
minutes of expiry trigger rotation and a fresh NEX fetch; expired NEX tokens also
trigger renewal. Expired NEX tokens cannot be placed in a new BAAS id_token.
PID and game tokens are exposed to account IPC only for the bound profile, with
online mode and valid routes. Other profiles cannot borrow that identity.
Credentials, codes, callbacks, JWTs, and HTTP bodies are never logged. There is
no plaintext token file or Nintendo credential reader.

## Routing and service hooks

| Pattern / exact host | Destination |
| --- | --- |
| NAT discovery hosts (prefix `nncs2-`) | NAT address |
| MK8D NAT hostname (MK8D secure NEX) | NAT address |
| Splatoon 2 NAT hostname (Splatoon 2 rendezvous) | NAT address |
| Vendor online service domains | Main address |
| `*.nintendowifi.net`, `*.nintendo.co.jp` | Main address |
| `demonware.net`, `*.demonware.net` | Main address |
| Other known regional Nintendo domains | Blocked |
| Other valid hostnames / numeric IPv4 peer addresses | Existing networking policy |

Specific NAT exceptions precede broad domains. Matching ignores ASCII case and
one terminal DNS dot, requires a domain boundary, rejects malformed names, and
also protects bare Nintendo domain roots. Both DNS functions and their option
variants share the policy; the guest's original canonical hostname is retained.
Unlike the fork, NXbox does not let arbitrary hosts-file overrides redirect a
linked account token elsewhere. It does not reproduce Prelude's exemptions that
allow selected hosts to reach Nintendo.

nifm reports connected in Nextendo mode, respecting airplane mode. Its internet
request acceptance path uses the same opt-in gate. This is a virtual service state,
not a connectivity probe. SSL retains the original SNI for Nextendo's routing and
rejects imported client PKI and internal PKI registration while the mode is on.
Existing guest SSL verification behavior is otherwise unchanged; the native account
HTTP transport validates TLS independently.

The application account manager returns the Nextendo PID and an RS256 JWT with
`nnex`, membership flag, title version, and a three-hour expiry. The Nintendo-looking
issuer/JWKS values are protocol identifiers, resolved through Nextendo routes,
not direct HTTP requests. Buffer size is checked before returning a token. An
unlinked profile fails instead of receiving an all-zero token. This hook does not
implement official dauth, BAAS, NPLN, eShop, or Nintendo account authentication.

Friends, presence, online-status, and NEX-session requests use
`Authorization: Bearer <access_token>`, never the game token.
Friends are read from `/api/friends`, limited to 300 entries and refreshed every
45 seconds. List IDs, 0x200-byte friend records, count, presence status, same-title
state, and the 0xc0-byte AppField are exposed through friend IPC. Favorite/history
filters return no matches because their metadata is not fetched. Presence updates
are deduplicated and queued from IPC and sent off the service/UI threads to `/api/presence`. Presence is reposted on the 45-second social cycle, including idle presence
before the first game update, to stay within the required 90-second interval.
A session heartbeat is sent to `/api/nex-session`. A 401
on social calls removes only the session that sent the rejected request. A late
response cannot invalidate a replacement login; vault and in-memory updates, refresh rotation, and sign-out are
serialized. Rotated credentials also distinguish a session from an older in-flight request.
Token renewal preserves friend/presence caches for the same account/profile. Disabling online clears the visible
friend/presence cache and prevents subsequent scheduled requests; an HTTP request
already in flight may finish.

## Measurement and remaining acceptance

Baseline `ea3244c9c`: 326 port tests, exit code 0, four platform skips. There was
no T22 suite or Nextendo feature: before coverage is absent, not an invented line
percentage. T22 adds 12 executable host tests for route precedence, normalization,
configuration rejection, PKCE RFC 7636, mock HTTP, CSRF, single-use exchange,
expiry/cancel, transport/server/storage failures, profile isolation, social
allowlisting, and friend wire layout.

Historical measurement of the original implementation (LLVM instrumentation of the six portable C++ headers; not remeasured for the OAuth changes):
454/466 lines (**97.42%**), 326/374 branches (**87.17%**), and
35/36 functions (**97.22%**). Routing alone is 110/110 lines and 112/118 branches;
auth is 154/155 lines and 120/140 branches.

Original integration acceptance on 2026-10-12:

- Full port suite: **338 tests**, four existing platform skips, **exit code 0**.
- T22 coverage suite: **12 tests**, **exit code 0**. Before: zero T22 tests and
  no Nextendo implementation; after: the instrumented counts above.
- [UWP/MSVC workflow 38303937433](https://github.com/NspxMiguel/NXbox/actions/runs/38303937433):
  **success**, including Windows port tests and the UWP build, at code commit
  `340857b5426bc203bc03dbcf92aaa2f43134af1b` on `team/t22-nextendo`.
  Subsequent delivery changes are documentation only.
- No Xbox online lobby or live Nextendo authentication test was performed.
  Production activation still needs a registered public client ID, approved
  routing addresses, and the console checks below.

OAuth flow validation on 2026-10-18: `python3 tests/port/test_t22_nextendo.py`
passes **15 tests**. Mocked requests verify the matchmaking scope, separate NEX
GET and Bearer token, 404-only legacy fallback, token validation, five-minute
refresh threshold, immediate rotated-token persistence, failed NEX retry state,
refresh 400/401 session clearing, and expired game-token rejection. Source checks
cover WinRT restore, vault fields, and refresh wiring; they do not execute
PasswordVault or Windows HTTP. No UWP build or live account test was run for this
change.

Run from the repository:

```sh
cd tests/port
python3 -m unittest discover -s . -p 'test_*.py'
NXBOX_T22_COVERAGE=/tmp/nxbox-t22-coverage python3 -m unittest test_t22_nextendo -v
```

Coverage builds and transient test data live under `/tmp` and are cleaned by the
test fixture. The retained coverage directory contains only host coverage evidence,
not credentials. Coverage measures the portable C++ policy, not Xbox WinRT APIs,
full HLE IPC scheduling, or real servers. No local emulator build is required.

Console validation still required, with MK8D first:

1. Register the NXbox client and supply the two approved server addresses; confirm
   provider acceptance without Ryujinx release attestation.
2. Complete browser authorization and manually enter the return URL on Xbox;
   verify vault persistence, sign-out, failed storage, and profile switching.
3. With a network trace, confirm account HTTP goes only to Nextendo, all mapped
   game DNS returns the configured addresses, and Nintendo destinations receive
   no packets. Also test titles using raw IPs or a custom in-game DNS resolver:
   the current DNS/SSL hook is **not a destination-IP firewall**.
4. MK8D: enter online, pass the two-responder NAT probe, list/join an online race
   lobby, start a race with another Nextendo player, finish, reconnect, and join
   a friend's room. Repeat on restrictive home NAT and interrupted networking.
5. Splatoon 2: validate the operator-issued signing key against the published JWKS,
   stage/BCAT availability, AppField/private-room joins, and peer session setup.
6. Smash Ultimate: authenticate, create/join a battle arena, and sustain a match.
7. Toggle online off and airplane mode on; verify fail-closed behavior after
   disconnect and that credentials do not cross local profiles.

Not implemented/verified: device-code/QR grants (not in the studied client),
release client attestation, certificate trust provisioning or missing-SNI recovery,
UDP port-keeper parity, BCAT schedules/game patches, catalog version gating,
friend requests/mutations, invitations, avatars, server-provided NSA identities,
updated friend/viewer IPC variants, official account online, and a destination-IP
firewall for hardcoded addresses. No successful online lobby is claimed. Changes
needed in `src/core/internal_network`, BCAT, or application manifests must be
reviewed by their owning tracks; no such files or `src/video_core` are modified.
