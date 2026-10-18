import { afterEach, describe, expect, test } from "bun:test";
import { PERM, startDaemon, type DaemonConfig, type Rest } from "./honeypot";

/**
 * Drives the daemon against a local server that speaks just enough of the Discord Gateway:
 * Hello, Heartbeat/ACK, Identify -> READY + GUILD_CREATE, Resume -> RESUMED.
 */

const GUILD = "100000000000000000";
const OWNER = "100000000000000001";
const BOT = "100000000000000002";
const CHANNEL = "900000000000000000";
const cfg: DaemonConfig = {
    token: "not-a-real-token",
    channelId: CHANNEL,
    dryRun: true,
    testSelf: false,
};

interface Frame {
    op: number;
    d: any;
}

function mockGateway(onIdentify?: (ws: any) => void, heartbeatMs = 150) {
    const received: Frame[] = [];
    const sockets: any[] = [];
    const state = { ackHeartbeats: true, seq: 0 };
    let url = ""; // known once the server is listening
    const send = (ws: any, op: number, d: unknown, t?: string) =>
        ws.send(JSON.stringify({ op, d, s: t ? ++state.seq : null, t: t ?? null }));
    const server = Bun.serve({
        port: 0,
        fetch: (req, srv) =>
            srv.upgrade(req) ? undefined : new Response("upgrade required", { status: 426 }),
        websocket: {
            open(ws) {
                sockets.push(ws);
                send(ws, 10, { heartbeat_interval: heartbeatMs });
            },
            message(ws, raw) {
                const frame: Frame = JSON.parse(String(raw));
                received.push(frame);
                if (frame.op === 1 && state.ackHeartbeats) send(ws, 11, null);
                if (frame.op === 6) send(ws, 0, {}, "RESUMED");
                if (frame.op === 2) {
                    if (onIdentify) return onIdentify(ws);
                    send(
                        ws,
                        0,
                        {
                            user: { id: BOT },
                            session_id: "sess-1",
                            resume_gateway_url: url,
                            guilds: [{ id: GUILD, unavailable: true }],
                        },
                        "READY",
                    );
                    send(
                        ws,
                        0,
                        {
                            id: GUILD,
                            owner_id: OWNER,
                            roles: [
                                { id: GUILD, permissions: "0" },
                                { id: "role-admin", permissions: String(PERM.ADMINISTRATOR) },
                                { id: "role-plain", permissions: "0" },
                            ],
                        },
                        "GUILD_CREATE",
                    );
                }
            },
        },
    });
    url = `ws://127.0.0.1:${server.port}`;
    return {
        url,
        received,
        sockets,
        state,
        /** Pushes a dispatch event to the newest connection. */
        emit: (t: string, d: unknown) => send(sockets[sockets.length - 1], 0, d, t),
        raw: (op: number, d: unknown) => send(sockets[sockets.length - 1], op, d),
        stop: () => server.stop(true),
    };
}

const post = (userId: string, roles: string[], extra: Record<string, unknown> = {}) => ({
    id: "m1",
    channel_id: CHANNEL,
    guild_id: GUILD,
    author: { id: userId },
    member: { roles },
    ...extra,
});

async function until(what: string, check: () => boolean, timeoutMs = 5000) {
    const start = Date.now();
    while (!check()) {
        if (Date.now() - start > timeoutMs) throw new Error(`timed out waiting for: ${what}`);
        await new Promise((r) => setTimeout(r, 20));
    }
}
const pause = (ms: number) => new Promise((r) => setTimeout(r, ms));

let cleanup: (() => void)[] = [];
afterEach(() => {
    for (const fn of cleanup.reverse()) fn();
    cleanup = [];
});

/** Starts a mock Gateway and a daemon pointed at it. */
function setup(
    over: Partial<DaemonConfig> = {},
    opts: { rest?: Rest; onIdentify?: (ws: any) => void; heartbeatMs?: number } = {},
) {
    const gw = mockGateway(opts.onIdentify, opts.heartbeatMs);
    const logs: string[] = [];
    const exits: number[] = [];
    const daemon = startDaemon(
        { ...cfg, ...over },
        { gateway: gw.url, rest: opts.rest, log: (l) => logs.push(l), exit: (c) => exits.push(c) },
    );
    cleanup.push(gw.stop, daemon.close);
    const ready = () => until("READY", () => logs.some((l) => l.startsWith(`READY user=${BOT}`)));
    const has = (line: string) => logs.includes(line);
    return { gw, logs, exits, daemon, ready, has };
}

describe("gateway session", () => {
    test("identifies with intents 513, caches the guild, and reacts to honeypot posts", async () => {
        const { gw, logs, ready, has } = setup();
        await ready();
        const identify = gw.received.find((f) => f.op === 2)!;
        expect(identify.d.intents).toBe(513);
        expect(identify.d.token).toBe("not-a-real-token");
        await until("guild cached", () => has(`guild ${GUILD}: 3 roles cached`));

        gw.emit("MESSAGE_CREATE", post("200", ["role-plain"]));
        await until("dry-run line", () => has("would softban 200"));

        // The same user again inside the cooldown is not processed twice.
        gw.emit("MESSAGE_CREATE", post("200", ["role-plain"]));
        // A moderator and an owner are ignored and logged as such.
        gw.emit("MESSAGE_CREATE", post("201", ["role-admin"]));
        gw.emit("MESSAGE_CREATE", post(OWNER, []));
        // Another channel is not even logged.
        gw.emit("MESSAGE_CREATE", post("202", [], { channel_id: "900000000000000001" }));
        await until("owner ignored", () => has(`ignored ${OWNER} (owner)`));
        expect(has("ignored 201 (privileged)")).toBe(true);
        expect(logs.filter((l) => l === "would softban 200")).toHaveLength(1);
        expect(logs.some((l) => l.includes("202"))).toBe(false);
    });

    test("keeps the role cache current from role events", async () => {
        const { gw, ready, has } = setup();
        await ready();
        await pause(50);

        gw.emit("GUILD_ROLE_UPDATE", {
            guild_id: GUILD,
            role: { id: "role-plain", permissions: String(PERM.MANAGE_MESSAGES) },
        });
        gw.emit("MESSAGE_CREATE", post("210", ["role-plain"]));
        await until("promoted role exempt", () => has("ignored 210 (privileged)"));

        gw.emit("GUILD_ROLE_DELETE", { guild_id: GUILD, role_id: "role-plain" });
        gw.emit("MESSAGE_CREATE", post("211", ["role-plain"]));
        await until("deleted role is unknown", () => has("ignored 211 (unknown-role)"));

        gw.emit("GUILD_ROLE_CREATE", {
            guild_id: GUILD,
            role: { id: "role-plain", permissions: "0" },
        });
        gw.emit("MESSAGE_CREATE", post("212", ["role-plain"]));
        await until("recreated role is plain", () => has("would softban 212"));
    });

    test("live mode bans and unbans through the REST client", async () => {
        const calls: string[] = [];
        const rest: Rest = async (method, path, body) => {
            calls.push(`${method} ${path} ${body ? JSON.stringify(body) : ""}`.trim());
            return { ok: true, status: 204 };
        };
        const { gw, ready, has } = setup({ dryRun: false }, { rest });
        await ready();
        await pause(50);
        gw.emit("MESSAGE_CREATE", post("220", []));
        await until("softban logged", () => has("softban 220: ok"));
        expect(calls).toEqual([
            `PUT /guilds/${GUILD}/bans/220 {"delete_message_seconds":86400}`,
            `DELETE /guilds/${GUILD}/bans/220`,
        ]);
    });

    test("drain waits for a softban that is between its ban and its unban", async () => {
        const calls: string[] = [];
        const rest: Rest = async (method) => {
            if (method === "PUT") await pause(300);
            calls.push(method);
            return { ok: true, status: 204 };
        };
        const { gw, ready, daemon } = setup({ dryRun: false }, { rest });
        await ready();
        await pause(50);
        gw.emit("MESSAGE_CREATE", post("230", []));
        await until("softban started", () => daemon.pending() === 1);
        await daemon.drain();
        expect(calls).toEqual(["PUT", "DELETE"]);
        expect(daemon.pending()).toBe(0);
    });

    test("testSelf softbans the bot's own post even though the bot holds Administrator", async () => {
        const { gw, ready, has } = setup({ testSelf: true });
        await ready();
        await pause(50);
        gw.emit("MESSAGE_CREATE", post(BOT, ["role-admin"], { author: { id: BOT, bot: true } }));
        await until("own message", () => has(`would softban ${BOT}`));
    });
});

describe("startDaemon guards", () => {
    test("refuses testSelf without dryRun, so the bot can never ban itself", () => {
        expect(() => startDaemon({ ...cfg, dryRun: false, testSelf: true })).toThrow(/dryRun/);
    });
});

describe("gateway resilience", () => {
    test("resumes the same session, with the last sequence number, after an abrupt drop", async () => {
        // Heartbeats this slow can never detect the drop, so only the close handler can.
        const { gw, ready, has } = setup({}, { heartbeatMs: 60_000 });
        await ready();
        await pause(50);
        gw.sockets[0].close(1011, "boom");
        await until("resume frame", () => gw.received.some((f) => f.op === 6), 8000);
        const resume = gw.received.find((f) => f.op === 6)!;
        expect(resume.d.session_id).toBe("sess-1");
        expect(resume.d.seq).toBe(2); // READY was 1, GUILD_CREATE was 2
        expect(resume.d.token).toBe("not-a-real-token");
        await until("RESUMED", () => has("RESUMED"));
        expect(gw.received.filter((f) => f.op === 2)).toHaveLength(1);
    }, 15000);

    test("obeys a reconnect request (op 7) by resuming", async () => {
        const { gw, ready } = setup();
        await ready();
        await pause(50);
        gw.raw(7, null);
        await until("resume after op 7", () => gw.received.some((f) => f.op === 6), 8000);
        expect(gw.received.filter((f) => f.op === 2)).toHaveLength(1);
    }, 15000);

    test("starts a fresh session after a non-resumable invalid session (op 9)", async () => {
        const { gw, ready } = setup();
        await ready();
        await pause(50);
        gw.raw(9, false);
        await until(
            "second identify",
            () => gw.received.filter((f) => f.op === 2).length === 2,
            9000,
        );
        expect(gw.received.some((f) => f.op === 6)).toBe(false);
    }, 15000);

    test("reconnects when heartbeats stop being acknowledged", async () => {
        const { gw, logs, ready } = setup();
        await ready();
        gw.state.ackHeartbeats = false;
        await until(
            "zombie detected",
            () => logs.some((l) => l.includes("heartbeat not acknowledged")),
            8000,
        );
        gw.state.ackHeartbeats = true;
        await until("resume after zombie", () => gw.received.some((f) => f.op === 6), 8000);
    }, 20000);

    test("exits with status 78 on a fatal close code and does not reconnect", async () => {
        const { gw, exits, logs } = setup(
            {},
            { onIdentify: (ws) => ws.close(4004, "Authentication failed.") },
        );
        await until("fatal exit", () => exits.length === 1);
        expect(exits).toEqual([78]);
        await pause(1500);
        expect(gw.sockets).toHaveLength(1);
        expect(logs.some((l) => l.includes("fatal close 4004"))).toBe(true);
    }, 10000);

    test("keeps retrying with backoff while the Gateway is unreachable", async () => {
        const dead = mockGateway();
        const url = dead.url;
        dead.stop();
        const logs: string[] = [];
        const daemon = startDaemon(cfg, { gateway: url, log: (l) => logs.push(l) });
        cleanup.push(daemon.close);
        await until(
            "two failed attempts",
            () => logs.filter((l) => l.includes("reconnecting in")).length >= 2,
            8000,
        );
        expect(
            logs.filter((l) => l.startsWith("gateway: connecting")).length,
        ).toBeGreaterThanOrEqual(2);
    }, 15000);
});
