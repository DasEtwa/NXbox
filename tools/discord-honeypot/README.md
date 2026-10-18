# NXbox Discord honeypot

A small daemon that guards a trap channel in the NXbox Discord server. Spam bots running on hijacked
accounts post in every channel they can see, so they land in the honeypot channel
(`⛔︱dont-post-in-here`). Whoever posts there is softbanned: banned with one day of their messages
deleted, then unbanned right away. The spam disappears from the whole server and the real owner of
the account can still come back.

Never softbanned: the guild owner, bots, webhooks, and any member whose roles grant Administrator,
Ban Members or Manage Messages.

## How it works

- Connects to the Discord Gateway (v10) with the `GUILDS | GUILD_MESSAGES` intents (513). The
  Message Content intent is not needed: only the author, the member roles and the channel are read.
- Keeps the guild owner and the permissions of every role in memory (`GUILD_CREATE` plus the role
  events), so deciding whether a poster is exempt needs no REST call.
- On a message in the honeypot channel it calls `PUT /guilds/{guild}/bans/{user}` with
  `delete_message_seconds: 86400` and the audit log reason `honeypot softban`, then
  `DELETE /guilds/{guild}/bans/{user}`. A 429 waits for `retry_after`. The unban is retried for
  minutes, because a failed unban would leave a real person banned.
- When the facts are incomplete (no member object in the event, a role the cache does not know) the
  message is skipped rather than guessed at.
- Handles heartbeats, resume and reconnects with exponential backoff. Fatal close codes (bad token,
  disallowed intents) exit with status 78, and the systemd unit does not restart on that status.
- On `SIGTERM` it lets a softban that is between its ban and its unban finish before exiting.
- Logs one line per action to stdout (user id and outcome). Message content is never read or logged.

## Configuration

| Variable              | Required | Meaning                                                                 |
| --------------------- | -------- | ----------------------------------------------------------------------- |
| `NXBOX_DISCORD_TOKEN` | yes      | Bot token.                                                              |
| `HONEYPOT_CHANNEL_ID` | yes      | Id of the honeypot channel.                                             |
| `DRY_RUN`             | no       | `1` logs `would softban <id>` instead of banning anyone.                |
| `HONEYPOT_TEST_SELF`  | no       | `1` stops exempting the bot's own account. Only valid with `DRY_RUN=1`. |

`DRY_RUN` and `HONEYPOT_TEST_SELF` accept only `1` or `0`; anything else is a configuration error,
so a typo can never switch a dry run to live mode unnoticed.

## Run and test

```sh
NXBOX_DISCORD_TOKEN=... HONEYPOT_CHANNEL_ID=... DRY_RUN=1 bun tools/discord-honeypot/honeypot.ts
bun test tools/discord-honeypot/
```

`honeypot.test.ts` covers the exemption decision (owner, admin role, Ban Members role, Manage
Messages role, bot, webhook, plain member, other channel, incomplete data), the environment checks,
and the ban/unban sequence against a fake `fetch` (rate limits, network errors, refused ban, unban
that keeps failing).

`gateway.test.ts` drives the daemon against a local mock of the Gateway: identify and READY, the
role cache, resume with the last sequence number, reconnect (op 7) and invalid session (op 9)
requests, missed heartbeat acknowledgements, fatal close codes, and an unreachable Gateway. It takes
about ten seconds because the reconnect delays are real.

## Discord setup

The bot needs View Channel, Send Messages, Manage Channels (to create the channel), Manage Messages
(to pin the warning) and Ban Members. Its top role must stay above the roles of ordinary members, or
bans of those members are refused with 403. Check this again whenever roles are reordered.

The channel is a text channel outside any category, at the top of the list, with slowmode off. The
`@everyone` overwrite explicitly allows View Channel, Send Messages and Read Message History, so the
trap keeps working even if the server-wide defaults are tightened later. The only message is this
warning, pinned:

```
# ⛔ NÃO POSTE AQUI ⛔
# DO NOT POST HERE

**Este canal é uma armadilha (honeypot) que pega spam de contas invadidas.**
Quem enviar uma mensagem aqui será __banido e desbanido automaticamente__ (softban) e terá as mensagens recentes apagadas do servidor. **Este é o seu único aviso.**

**This channel is a honeypot that catches spam from compromised accounts.**
Anyone who posts here is __automatically banned and unbanned__ (softban), and their recent messages are deleted. **This is your only warning.**
```

## Deployment

It runs on the Raspberry Pi as the systemd service `nxbox-honeypot`, under the same user and layout
as the other Bun service there:

- `/srv/nxbox-honeypot/honeypot.ts` is this file.
- `/srv/nxbox-honeypot/honeypot.env` holds `NXBOX_DISCORD_TOKEN` and `HONEYPOT_CHANNEL_ID`,
  mode 600. Write it from stdin so the token never shows up in a command line or a log.
- `/etc/systemd/system/nxbox-honeypot.service`:

```ini
[Unit]
Description=NXbox Discord honeypot (softbans spam from hijacked accounts)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=keel
WorkingDirectory=/srv/nxbox-honeypot
EnvironmentFile=/srv/nxbox-honeypot/honeypot.env
ExecStart=/home/keel/.bun/bin/bun run /srv/nxbox-honeypot/honeypot.ts
Restart=always
RestartSec=10
RestartPreventExitStatus=78
MemoryMax=96M
NoNewPrivileges=true
ProtectSystem=strict
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

`MemoryMax` is declarative on this Pi: its kernel boots with `cgroup_disable=memory`, so no memory
limit is enforced for any service. The honeypot idles at about 25 MB resident.

Update: copy the new `honeypot.ts` over, `sudo systemctl restart nxbox-honeypot`, then check
`journalctl -u nxbox-honeypot` for the `READY` line. The journal on the Pi is volatile (RAM), so
logging costs the SD card nothing.

To prove the event path without banning anyone, add `DRY_RUN=1` and `HONEYPOT_TEST_SELF=1` to the
env file and restart. Post a test message as the bot in the honeypot channel and look for
`would softban <bot id>` in the journal. Delete the test message, remove the two lines and restart
to go live.

## Limits

- Messages in threads started under the honeypot channel are not watched.
- A softbanned account can rejoin through any invite. The softban removes the spam, it does not keep
  the account out.
- If the host dies between the ban and the unban, the user stays banned. The window is a few hundred
  milliseconds, and a failed unban is logged as `UNBAN FAILED`.
