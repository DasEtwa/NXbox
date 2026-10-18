/**
 * NXbox Discord honeypot.
 *
 * Anyone who posts in the honeypot channel is softbanned: banned with one day of their messages
 * deleted, then unbanned right away. Spam from a hijacked account disappears from the whole server
 * and the real owner of the account can still come back.
 *
 * Gateway v10 (intents GUILDS | GUILD_MESSAGES, no Message Content) plus two REST calls. A single
 * file with no dependencies; run it with Bun.
 */

const API = "https://discord.com/api/v10";
const GATEWAY = "wss://gateway.discord.gg";
const USER_AGENT = "DiscordBot (https://github.com/NspxMiguel/NXbox, 1.0)";
const REASON = "honeypot softban";
/** GUILDS | GUILD_MESSAGES. Only author, member roles and channel are read, never the content. */
const INTENTS = (1 << 0) | (1 << 9);
const DELETE_MESSAGE_SECONDS = 86400;
const MAX_RETRIES = 5;
const UNBAN_ATTEMPTS = 8;
/** A user who just posted is not processed again for this long (bots post in bursts). */
const COOLDOWN_MS = 60_000;
/** Exit status for configuration errors; the systemd unit does not restart on it. */
const EX_CONFIG = 78;
/** Close codes that retrying cannot fix: bad token, bad intents, bad sharding, bad version. */
const FATAL_CLOSE = new Set([4004, 4010, 4011, 4012, 4013, 4014]);
/** Close codes after which the session is gone and a fresh Identify is required. */
const NEW_SESSION_CLOSE = new Set([4007, 4009]);

// ---------------------------------------------------------------------------------------------
// Pure decision logic
// ---------------------------------------------------------------------------------------------

export const PERM = {
    BAN_MEMBERS: 1n << 2n,
    ADMINISTRATOR: 1n << 3n,
    MANAGE_MESSAGES: 1n << 13n,
} as const;

/** Anyone holding one of these permissions is a moderator and is never softbanned. */
const EXEMPT = PERM.ADMINISTRATOR | PERM.BAN_MEMBERS | PERM.MANAGE_MESSAGES;

export interface MessageFacts {
    channelId: string;
    authorId: string;
    authorIsBot: boolean;
    webhookId?: string | null;
    /** Role ids of the author. Undefined when Discord sent no member object. */
    memberRoleIds?: readonly string[] | null;
}

export interface GuildFacts {
    /** The guild id, which is also the id of the @everyone role. */
    id: string;
    ownerId: string;
    /** Permission bits of every role of the guild, @everyone included. */
    rolePerms: ReadonlyMap<string, bigint>;
}

export interface DecideConfig {
    channelId: string;
    botId: string;
    /** Dry-run proof mode: treat the bot's own account as a plain member. */
    testSelf: boolean;
}

export interface Decision {
    action: "softban" | "ignore";
    reason: string;
}

const ignore = (reason: string): Decision => ({ action: "ignore", reason });

/**
 * Decides what to do with one message. Whenever the facts are incomplete (no member data, a role
 * the cache does not know) it ignores the message: skipping one spammer is cheap, banning a
 * moderator is not.
 */
export function decide(msg: MessageFacts, guild: GuildFacts, cfg: DecideConfig): Decision {
    if (msg.channelId !== cfg.channelId) return ignore("other-channel");
    if (cfg.testSelf && msg.authorId === cfg.botId)
        return { action: "softban", reason: "test-self" };
    if (msg.webhookId) return ignore("webhook");
    if (msg.authorIsBot) return ignore("bot");
    if (msg.authorId === guild.ownerId) return ignore("owner");
    if (!msg.memberRoleIds) return ignore("no-member-data");

    let perms = guild.rolePerms.get(guild.id) ?? 0n;
    for (const roleId of msg.memberRoleIds) {
        const rolePerms = guild.rolePerms.get(roleId);
        if (rolePerms === undefined) return ignore("unknown-role");
        perms |= rolePerms;
    }
    if (perms & EXEMPT) return ignore("privileged");
    return { action: "softban", reason: "member" };
}

export interface DaemonConfig {
    token: string;
    channelId: string;
    dryRun: boolean;
    testSelf: boolean;
}

function flag(name: string, value: string | undefined): boolean {
    if (value === undefined || value === "" || value === "0") return false;
    if (value === "1") return true;
    throw new Error(`${name} must be 1 or 0`);
}

/** Reads and validates the environment. Error messages never contain the token. */
export function loadConfig(env: Record<string, string | undefined>): DaemonConfig {
    const token = env.NXBOX_DISCORD_TOKEN;
    const channelId = env.HONEYPOT_CHANNEL_ID;
    const dryRun = flag("DRY_RUN", env.DRY_RUN);
    const testSelf = flag("HONEYPOT_TEST_SELF", env.HONEYPOT_TEST_SELF);
    if (!token) throw new Error("NXBOX_DISCORD_TOKEN is not set");
    if (!channelId || !/^\d{15,25}$/.test(channelId)) {
        throw new Error("HONEYPOT_CHANNEL_ID must be a channel id");
    }
    if (testSelf && !dryRun)
        throw new Error("HONEYPOT_TEST_SELF=1 is only valid together with DRY_RUN=1");
    return { token, channelId, dryRun, testSelf };
}

// ---------------------------------------------------------------------------------------------
// REST: the softban itself
// ---------------------------------------------------------------------------------------------

type Sleep = (ms: number) => Promise<void>;
const realSleep: Sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export interface RestResult {
    ok: boolean;
    /** HTTP status, 0 when the request never got an answer. */
    status: number;
    /** Discord error code from the body, when there is one. */
    code?: number;
}
export type Rest = (method: "PUT" | "DELETE", path: string, body?: unknown) => Promise<RestResult>;

/**
 * REST client for the two idempotent calls the honeypot needs. Retries 429 after `retry_after`
 * and 5xx or network errors with exponential backoff.
 */
export function makeRest(
    token: string,
    doFetch: typeof fetch = fetch,
    sleep: Sleep = realSleep,
): Rest {
    return async (method, path, body) => {
        for (let attempt = 0; ; attempt++) {
            let res: Response | undefined;
            try {
                res = await doFetch(API + path, {
                    method,
                    headers: {
                        Authorization: `Bot ${token}`,
                        "User-Agent": USER_AGENT,
                        "X-Audit-Log-Reason": REASON,
                        ...(body ? { "Content-Type": "application/json" } : {}),
                    },
                    body: body ? JSON.stringify(body) : undefined,
                });
            } catch {
                // Network error: handled like a 5xx below.
            }

            const text = res ? await res.text().catch(() => "") : "";
            let data: { code?: number; retry_after?: number } | null = null;
            try {
                data = text ? JSON.parse(text) : null;
            } catch {
                // Not JSON (for example an HTML error page): no error code to report.
            }

            if (res?.status === 429 && attempt < MAX_RETRIES) {
                const seconds = Number(data?.retry_after ?? res.headers.get("retry-after")) || 1;
                await sleep(Math.min(60, seconds) * 1000 + 50);
                continue;
            }
            if ((!res || res.status >= 500) && attempt < MAX_RETRIES) {
                await sleep(1000 * 2 ** attempt);
                continue;
            }
            if (!res) return { ok: false, status: 0 };
            return { ok: res.ok, status: res.status, code: data?.code };
        }
    };
}

/**
 * Bans the user (deleting a day of their messages) and unbans them right away. Returns the
 * outcome for the log. The unban is retried for minutes because a failed one leaves a real
 * person banned.
 */
export async function softban(
    rest: Rest,
    guildId: string,
    userId: string,
    sleep: Sleep = realSleep,
): Promise<string> {
    const path = `/guilds/${guildId}/bans/${userId}`;
    const ban = await rest("PUT", path, { delete_message_seconds: DELETE_MESSAGE_SECONDS });
    if (!ban.ok) return `ban failed status=${ban.status} code=${ban.code ?? "-"}`;

    for (let attempt = 0; attempt < UNBAN_ATTEMPTS; attempt++) {
        const unban = await rest("DELETE", path);
        if (unban.ok || unban.status === 404) return "ok";
        await sleep(Math.min(60_000, 2000 * 2 ** attempt));
    }
    return "UNBAN FAILED, the user is still banned";
}

// ---------------------------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------------------------

interface Payload {
    op: number;
    d: any;
    s: number | null;
    t: string | null;
}

export interface DaemonOptions {
    rest?: Rest;
    /** Gateway base URL; only tests change it. */
    gateway?: string;
    log?: (line: string) => void;
    exit?: (code: number) => void;
}

export function startDaemon(cfg: DaemonConfig, opts: DaemonOptions = {}) {
    if (cfg.testSelf && !cfg.dryRun) throw new Error("testSelf is only valid together with dryRun");
    const rest = opts.rest ?? makeRest(cfg.token);
    const gateway = opts.gateway ?? GATEWAY;
    const log = opts.log ?? ((line: string) => console.log(line));
    const exit = opts.exit ?? ((code: number) => process.exit(code));
    const mode = cfg.dryRun ? "dry-run" : "live";
    const guilds = new Map<string, GuildFacts & { rolePerms: Map<string, bigint> }>();
    const handled = new Map<string, number>();
    const inflight = new Set<Promise<unknown>>();
    let botId = "";
    let sock: WebSocket | undefined;
    let connId = 0; // bumped whenever a socket is abandoned, so its late events are ignored
    let seq: number | null = null;
    let sessionId: string | undefined;
    let resumeUrl: string | undefined;
    let beat: ReturnType<typeof setTimeout> | undefined;
    let acked = true;
    let failures = 0;
    let stopped = false;
    let retry: ReturnType<typeof setTimeout> | undefined;

    const backoff = () => Math.min(30_000, 1000 * 2 ** failures++) * (0.5 + Math.random() / 2);
    const send = (s: WebSocket, payload: unknown) => {
        if (s.readyState === WebSocket.OPEN) s.send(JSON.stringify(payload));
    };

    function connect() {
        if (stopped) return;
        const id = ++connId;
        log(`gateway: connecting (${sessionId ? "resume" : "identify"})`);
        let s: WebSocket;
        try {
            s = new WebSocket(`${sessionId ? resumeUrl : gateway}/?v=10&encoding=json`);
        } catch (e) {
            log(`gateway: cannot connect: ${e instanceof Error ? e.message : String(e)}`);
            return reconnect(id, { resume: false, why: "connect failed" });
        }
        sock = s;
        s.onmessage = (ev) => {
            if (id !== connId) return;
            try {
                onPayload(s, id, JSON.parse(String(ev.data)));
            } catch (e) {
                log(`gateway: handler error: ${e instanceof Error ? e.message : String(e)}`);
            }
        };
        s.onclose = (ev) => {
            if (id !== connId) return;
            if (FATAL_CLOSE.has(ev.code)) {
                log(`gateway: fatal close ${ev.code}, check the token and the intents`);
                close();
                return exit(EX_CONFIG);
            }
            reconnect(id, { resume: !NEW_SESSION_CLOSE.has(ev.code), why: `closed (${ev.code})` });
        };
        s.onerror = () => reconnect(id, { resume: true, why: "socket error" });
    }

    /** Stops everything for good: no more reconnects, no more heartbeats. */
    function close() {
        stopped = true;
        connId++;
        clearTimeout(beat);
        clearTimeout(retry);
        try {
            sock?.close(1000);
        } catch {
            // Already closed.
        }
    }

    /** Abandons socket `id` (once) and opens a new one after a delay. */
    function reconnect(id: number, how: { resume: boolean; why: string; delayMs?: number }) {
        if (id !== connId || stopped) return;
        connId++;
        clearTimeout(beat);
        if (!how.resume) {
            sessionId = resumeUrl = undefined;
            seq = null;
        }
        try {
            sock?.close(how.resume ? 4000 : 1000);
        } catch {
            // Already closed.
        }
        const delay = how.delayMs ?? backoff();
        log(`gateway: ${how.why}, reconnecting in ${Math.round(delay / 1000)}s`);
        retry = setTimeout(connect, delay);
    }

    function onPayload(s: WebSocket, id: number, p: Payload) {
        if (p.s != null) seq = p.s;
        switch (p.op) {
            case 0:
                return dispatch(p.t ?? "", p.d);
            case 1:
                return send(s, { op: 1, d: seq });
            case 7:
                return reconnect(id, { resume: true, why: "reconnect requested", delayMs: 1000 });
            case 9: // Invalid session: wait 1-5 s, then resume (d=true) or identify again
                return reconnect(id, {
                    resume: Boolean(p.d),
                    why: "invalid session",
                    delayMs: 1000 + Math.random() * 4000,
                });
            case 10:
                return onHello(s, id, p.d.heartbeat_interval);
            case 11:
                acked = true;
        }
    }

    function onHello(s: WebSocket, id: number, interval: number) {
        acked = true;
        const tick = () => {
            if (id !== connId) return;
            if (!acked) return reconnect(id, { resume: true, why: "heartbeat not acknowledged" });
            acked = false;
            send(s, { op: 1, d: seq });
            beat = setTimeout(tick, interval);
        };
        beat = setTimeout(tick, interval * Math.random());
        send(
            s,
            sessionId
                ? { op: 6, d: { token: cfg.token, session_id: sessionId, seq } }
                : {
                      op: 2,
                      d: {
                          token: cfg.token,
                          intents: INTENTS,
                          properties: {
                              os: "linux",
                              browser: "nxbox-honeypot",
                              device: "nxbox-honeypot",
                          },
                      },
                  },
        );
    }

    function dispatch(type: string, d: any) {
        switch (type) {
            case "READY":
                botId = d.user.id;
                sessionId = d.session_id;
                resumeUrl = d.resume_gateway_url;
                failures = 0;
                log(`READY user=${botId} guilds=${d.guilds.length} mode=${mode}`);
                break;
            case "RESUMED":
                failures = 0;
                log("RESUMED");
                break;
            case "GUILD_CREATE":
            case "GUILD_UPDATE":
                guilds.set(d.id, {
                    id: d.id,
                    ownerId: d.owner_id,
                    rolePerms: new Map(d.roles.map((r: any) => [r.id, BigInt(r.permissions)])),
                });
                if (type === "GUILD_CREATE") log(`guild ${d.id}: ${d.roles.length} roles cached`);
                break;
            case "GUILD_DELETE":
                if (!d.unavailable) guilds.delete(d.id);
                break;
            case "GUILD_ROLE_CREATE":
            case "GUILD_ROLE_UPDATE":
                guilds.get(d.guild_id)?.rolePerms.set(d.role.id, BigInt(d.role.permissions));
                break;
            case "GUILD_ROLE_DELETE":
                guilds.get(d.guild_id)?.rolePerms.delete(d.role_id);
                break;
            case "MESSAGE_CREATE":
                onMessage(d);
                break;
        }
    }

    function onMessage(m: any) {
        if (m.channel_id !== cfg.channelId) return;
        const guild = guilds.get(m.guild_id);
        if (!guild) return;
        const userId: string = m.author.id;
        const decision = decide(
            {
                channelId: m.channel_id,
                authorId: userId,
                authorIsBot: Boolean(m.author.bot),
                webhookId: m.webhook_id,
                memberRoleIds: m.member?.roles,
            },
            guild,
            { channelId: cfg.channelId, botId, testSelf: cfg.testSelf },
        );
        if (decision.action === "ignore") return log(`ignored ${userId} (${decision.reason})`);

        const now = Date.now();
        for (const [id, at] of handled) if (now - at > COOLDOWN_MS) handled.delete(id);
        if (handled.has(userId)) return;
        handled.set(userId, now);
        if (cfg.dryRun) return log(`would softban ${userId}`);

        const job: Promise<void> = softban(rest, m.guild_id, userId)
            .then((outcome) => log(`softban ${userId}: ${outcome}`))
            .catch((e) => log(`softban ${userId}: error ${e instanceof Error ? e.message : e}`))
            .finally(() => inflight.delete(job));
        inflight.add(job);
    }

    log(
        `starting honeypot mode=${mode} channel=${cfg.channelId}` +
            (cfg.testSelf ? " test-self=on" : ""),
    );
    connect();

    return {
        /** Softbans that are between their ban and their unban right now. */
        pending: () => inflight.size,
        /** Resolves when the running softbans are done, or after `timeoutMs`. */
        drain: async (timeoutMs = 15_000) => {
            await Promise.race([Promise.allSettled([...inflight]), realSleep(timeoutMs)]);
        },
        /** Closes the gateway connection for good. */
        close,
    };
}

if (import.meta.main) {
    let cfg: DaemonConfig;
    try {
        cfg = loadConfig(process.env);
    } catch (e) {
        console.error(`config error: ${e instanceof Error ? e.message : String(e)}`);
        process.exit(EX_CONFIG);
    }
    const daemon = startDaemon(cfg);

    // Let a softban that is between its ban and its unban finish before the process goes away.
    const shutdown = async (signal: string) => {
        console.log(`${signal}: finishing ${daemon.pending()} softban(s) and exiting`);
        await daemon.drain();
        daemon.close();
        process.exit(0);
    };
    process.on("SIGTERM", () => shutdown("SIGTERM"));
    process.on("SIGINT", () => shutdown("SIGINT"));
}
