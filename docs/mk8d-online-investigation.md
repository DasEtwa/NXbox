# MK8D online disconnect investigation (2026-10-19)

The supplied captures establish that matchmaking and bidirectional peer UDP work.
They do **not** identify a timed-out NEX method, prove a NAT/session timeout, or
establish the cause of the communication-error dialog. No networking workaround
is justified by these logs alone. An active-driving console reproduction remains
necessary before claiming this disconnect is fixed.

## Evidence read

All paths below are relative to
`/Users/miguel/.local/share/nxbox/artifacts/` and were read directly:

- `nextendo-2026-10-19/online-nat/eden_log.txt`
- `nextendo-2026-10-19/online-rc/eden_log.txt` and `eden_uwp_diag.txt`
- `nextendo-2026-10-19/final105/eden_log.txt` and screenshots
- `nextendo-2026-10-19/{dpo,dpo2,oww,ow50}/eden_log.txt`
- `nextendo-2026-10-19/log{,2,3,4,5,6}/eden_log.txt`
- `nextendo-2026-10-19/online-rc.log`
- `compat-2026-10-18/online-final105.log`

The exact diagnostic identifier `39213302587` was not found in the diagnostic
text files searched under artifacts. The supplied commit `83bf6102ef` is a D3D12
compute subresource change. The TLS Peek / friend 10600 and 10702 fix is actually
`e1fcbf5878`, already included in this worktree, as is the later SSL descriptor
ownership fix `18093df598`.

## Findings

1. The older `online-nat` run repeatedly reports SSL error 5 immediately after
   Peek, including at 325.971732 s. This is consistent with the already-fixed
   zero-length TLS Peek. In `online-rc`, the corresponding calls use
   `SSL_peek_ex`; these internal errors disappear. The trace cannot identify a
   particular NEX RMC method because successful TLS reads/writes and decrypted
   messages are not recorded.
2. In `online-rc`, a race peer starts exchanging UDP at 402.565083 s. The
   412.573592 s interval reports 370 sends / 365 receives; at 437.604596 s it
   still reports 328 sends / 342 receives. This is not evidence of a lost peer
   keep-alive. The NAT/port-keeper endpoint has separate periodic outbound
   traffic; lack of replies in its aggregate alone does not prove failure.
3. In `final105`, TLS Peek / read calls continue at roughly nine-second intervals
   even after the race error, through 811.835895 s. `DeclareCloseOnlinePlaySession`
   occurs only at 349.546864 s, after the peer packet rate has dropped. Neither
   a TLS error nor a failed friend call is recorded at the race drop. Successful
   presence or game-server responses cannot be inferred from WANT_READ alone.
4. `final105/race-120.png` shows green starting lights and the local kart on the
   starting grid. `race-150.png` shows the communication-error dialog with the
   kart still on that grid. The run transcript records menu presses only, then
   evidence collection. The same transcript pattern occurs in `online-rc.log`.
   Inactivity is therefore an unexcluded explanation, not a proven service bug.
5. `UdpPeerTrace::Record` previously counted a failed `SendTo` as one sent packet
   of zero bytes. Consequently, a reported send rate does not establish how
   many calls the host actually accepted. The correction passes BSD errno into
   the trace, counts successful datagrams separately (including valid empty
   datagrams), and reports `send_errors` and `last_errno` per interval. This is
   an accounting fix, not a fix for the game's disconnect.

## Required console reproduction

Use the console ownership lock described in `tools/nxbox/compat/README.md` and
`NXBOX_SOCKET_TRACE=1`. Keep the same game version, server configuration and
renderer settings for both runs. Archive logs and captures separately.

- Control: reproduce the stationary start and record the first green light,
  the error dialog and the final UDP counters with their timestamps.
- Active run: accelerate from the green light and steer regularly. Record visible
  progress around the track, pass the former failure time, finish the race and
  enter the next lobby. A menu-only input script is insufficient. Script times
  use the first input poll as their epoch, not entry into the online race, so
  fixed boot-relative presses cannot reliably follow variable matchmaking waits.
- Compare successful peer packets and `send_errors`, not attempted sends. If the
  active run fails, collect a game-side error code or protocol-aware NEX trace
  before assigning a keep-alive, RMC, presence, NAT or session timeout. Do not log
  credentials, token values or raw decrypted authentication payloads.

No Xbox/UWP build, live race, active-input A/B run, or server-side trace was
performed in this worktree. Host regressions validate the trace accounting only.

## Host validation

For each pattern below, run
`python3 -B -m unittest discover -s tests/port -p '<pattern>'`:

| Pattern | Output |
| --- | --- |
| `test_udp_peer_trace.py` | `Ran 2 tests` / `OK` |
| `test_bsd_blocking_send.py` | `Ran 2 tests` / `OK` |
| `test_ssl_socket_ownership.py` | `Ran 8 tests` / `OK` |
| `test_t22_nextendo.py` | `Ran 23 tests` / `OK` |

The two new UDP regressions failed before the correction: the executable
identified a failed send as `first=send`, and the call-site check found no errno
forwarded to the trace. After the correction, the executable covers successful,
failed and empty sends, receive bytes, independent descriptors, interval reset,
and an interval containing errors alone. It compiles the production trace class
with host clang++, `-Wall -Wextra -Werror -Wshadow`, and a deterministic clock.
This does not execute Winsock or validate network delivery.

Changed C++ lines were formatted with clang-format and the new Python file with
Ruff. `git diff --check` passed. The requested commit could not be created:
`git add` was denied when creating the shared repository's worktree `index.lock`
outside the writable directory. Nothing was pushed.
