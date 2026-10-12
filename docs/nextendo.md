# Nextendo Network integration (T22)

This branch adds an opt-in **Online: Nextendo Network** setting, disabled by
default. It implements the community service's client protocol in C++, without
Nintendo accounts, console certificates, PRODINFO, device keys, or Nintendo tokens.
Online races have **not** been validated on Xbox. A successful host test is not
proof that matchmaking, TLS, UDP NAT traversal, or a race lobby works.

## Sources and attribution

Studied on 2026-10-12:

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

## Operator setup

Register an NXbox public PKCE application at the Nextendo developer portal, with
redirect URI `http://127.0.0.1:17822/callback` and identity/friends/presence scopes.
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
HTTPS origin, normal certificate validation, and no automatic redirects. Only
identity/friends/presence scopes are requested.

The account server returns `nex_token` plus `account.pid`, `username`, and
`friend_code`. The complete session is stored only in PasswordVault resource
`NXbox.Nextendo.Account`, username `session`, and bound to the active local
profile's raw UUID. PID and token are exposed to account IPC only for that profile,
with online mode and valid routes. Other profiles cannot borrow that identity.
Credentials, codes, callbacks, JWTs, and HTTP bodies are never logged. HTTP failures
and vault errors become fixed localized messages, not server-provided error text.
There is no plaintext token file or Nintendo credential reader.

## Routing and service hooks

| Pattern / exact host | Destination |
| --- | --- |
| `nncs2-*.n.n.srv.nintendo.net` | NAT address |
| `g2b309e01-lp1.s.n.srv.nintendo.net` (MK8D secure NEX) | NAT address |
| `g2df33d01-lp1.p.srv.nintendo.net` (Splatoon 2 rendezvous) | NAT address |
| `*.nintendo.net`, `*.nintendo.com` | Main address |
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

Friends are read from `/api/friends`, limited to 300 entries and refreshed every
45 seconds. List IDs, 0x200-byte friend records, count, presence status, same-title
state, and the 0xc0-byte AppField are exposed through friend IPC. Favorite/history
filters return no matches because their metadata is not fetched. Presence updates
are queued from IPC and sent off the service/UI threads to `/api/presence`. A 401
on social calls removes the vault session. Disabling online clears the visible
friend/presence cache and prevents subsequent scheduled requests; an HTTP request
already in flight may finish.

## Measurement and remaining acceptance

Baseline `ea3244c9c`: 326 port tests, exit code 0, four platform skips. There was
no T22 suite or Nextendo feature: before coverage is absent, not an invented line
percentage. T22 adds 10 executable host tests for route precedence, normalization,
configuration rejection, PKCE RFC 7636, mock HTTP, CSRF, single-use exchange,
expiry/cancel, transport/server/storage failures, profile isolation, social
allowlisting, and friend wire layout.

Measured after implementation (LLVM instrumentation of the six portable C++ headers):
432/441 lines (**97.96%**), 308/352 branches (**87.50%**), and 33/33 functions
(**100%**). Routing alone is 110/110 lines and 112/118 branches; auth is 150/151
lines and 111/130 branches. The full port suite result and MSVC run are recorded
in the delivery report, with their real exit/conclusion.

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
