import { describe, expect, test } from "bun:test";
import {
    PERM,
    decide,
    loadConfig,
    makeRest,
    softban,
    type DecideConfig,
    type GuildFacts,
    type MessageFacts,
} from "./honeypot";

const GUILD_ID = "100000000000000000";
const OWNER_ID = "100000000000000001";
const BOT_ID = "100000000000000002";
const HONEYPOT = "900000000000000000";

const rolePerms = new Map<string, bigint>([
    [GUILD_ID, 0n], // @everyone
    ["role-admin", PERM.ADMINISTRATOR],
    ["role-ban", PERM.BAN_MEMBERS],
    ["role-manage-messages", PERM.MANAGE_MESSAGES],
    ["role-plain", 0n],
    ["role-player", 0n],
]);
const guild: GuildFacts = { id: GUILD_ID, ownerId: OWNER_ID, rolePerms };
const cfg: DecideConfig = { channelId: HONEYPOT, botId: BOT_ID, testSelf: false };

const message = (over: Partial<MessageFacts> = {}): MessageFacts => ({
    channelId: HONEYPOT,
    authorId: "200000000000000000",
    authorIsBot: false,
    webhookId: null,
    memberRoleIds: ["role-plain"],
    ...over,
});

describe("decide", () => {
    test("softbans a plain member who posts in the honeypot", () => {
        expect(decide(message(), guild, cfg)).toEqual({ action: "softban", reason: "member" });
    });

    test("softbans a member with no roles and one with several harmless roles", () => {
        expect(decide(message({ memberRoleIds: [] }), guild, cfg).action).toBe("softban");
        const many = message({ memberRoleIds: ["role-plain", "role-player"] });
        expect(decide(many, guild, cfg).action).toBe("softban");
    });

    test("never touches the guild owner", () => {
        const owner = message({ authorId: OWNER_ID, memberRoleIds: [] });
        expect(decide(owner, guild, cfg)).toEqual({ action: "ignore", reason: "owner" });
    });

    test("exempts a member whose role grants Administrator", () => {
        const admin = message({ memberRoleIds: ["role-plain", "role-admin"] });
        expect(decide(admin, guild, cfg)).toEqual({ action: "ignore", reason: "privileged" });
    });

    test("exempts a member whose role grants Ban Members", () => {
        const mod = message({ memberRoleIds: ["role-ban"] });
        expect(decide(mod, guild, cfg)).toEqual({ action: "ignore", reason: "privileged" });
    });

    test("exempts a member whose role grants Manage Messages", () => {
        const mod = message({ memberRoleIds: ["role-manage-messages"] });
        expect(decide(mod, guild, cfg)).toEqual({ action: "ignore", reason: "privileged" });
    });

    test("counts permissions that come from @everyone", () => {
        const loose: GuildFacts = {
            ...guild,
            rolePerms: new Map(rolePerms).set(GUILD_ID, PERM.MANAGE_MESSAGES),
        };
        expect(decide(message(), loose, cfg).action).toBe("ignore");
    });

    test("ignores bots", () => {
        const bot = message({ authorIsBot: true, memberRoleIds: [] });
        expect(decide(bot, guild, cfg)).toEqual({ action: "ignore", reason: "bot" });
    });

    test("ignores webhooks, even when flagged as bots", () => {
        const hook = message({ webhookId: "300000000000000000", authorIsBot: true });
        expect(decide(hook, guild, cfg)).toEqual({ action: "ignore", reason: "webhook" });
        const hookNoBotFlag = message({
            webhookId: "300000000000000000",
            memberRoleIds: undefined,
        });
        expect(decide(hookNoBotFlag, guild, cfg).reason).toBe("webhook");
    });

    test("ignores messages in other channels", () => {
        const elsewhere = message({ channelId: "900000000000000001" });
        expect(decide(elsewhere, guild, cfg)).toEqual({
            action: "ignore",
            reason: "other-channel",
        });
    });

    test("skips when Discord sent no member data instead of guessing", () => {
        const noMember = message({ memberRoleIds: undefined });
        expect(decide(noMember, guild, cfg)).toEqual({
            action: "ignore",
            reason: "no-member-data",
        });
    });

    test("skips when a role is missing from the cache instead of guessing", () => {
        const stale = message({ memberRoleIds: ["role-plain", "role-created-while-offline"] });
        expect(decide(stale, guild, cfg)).toEqual({ action: "ignore", reason: "unknown-role" });
    });

    describe("testSelf (dry-run proof mode)", () => {
        const self = { ...cfg, testSelf: true };
        const ownMessage = message({
            authorId: BOT_ID,
            authorIsBot: true,
            memberRoleIds: ["role-admin"], // the real bot holds Administrator
        });

        test("without it the bot's own message is ignored", () => {
            expect(decide(ownMessage, guild, cfg)).toEqual({ action: "ignore", reason: "bot" });
        });

        test("with it the bot's own account is treated as a plain member", () => {
            expect(decide(ownMessage, guild, self)).toEqual({
                action: "softban",
                reason: "test-self",
            });
        });

        test("with it other bots, the owner and other channels stay exempt", () => {
            const otherBot = message({ authorId: "400000000000000000", authorIsBot: true });
            expect(decide(otherBot, guild, self).action).toBe("ignore");
            expect(decide(message({ authorId: OWNER_ID }), guild, self).action).toBe("ignore");
            const elsewhere = { ...ownMessage, channelId: "900000000000000001" };
            expect(decide(elsewhere, guild, self).action).toBe("ignore");
        });
    });
});

describe("loadConfig", () => {
    const base = { NXBOX_DISCORD_TOKEN: "not-a-real-token", HONEYPOT_CHANNEL_ID: HONEYPOT };

    test("reads the token and the channel; both flags default to off", () => {
        expect(loadConfig(base)).toEqual({
            token: "not-a-real-token",
            channelId: HONEYPOT,
            dryRun: false,
            testSelf: false,
        });
    });

    test("DRY_RUN=1 turns on dry-run", () => {
        expect(loadConfig({ ...base, DRY_RUN: "1" }).dryRun).toBe(true);
        expect(loadConfig({ ...base, DRY_RUN: "0" }).dryRun).toBe(false);
    });

    test("HONEYPOT_TEST_SELF=1 is only valid together with DRY_RUN=1", () => {
        expect(() => loadConfig({ ...base, HONEYPOT_TEST_SELF: "1" })).toThrow(/DRY_RUN=1/);
        const both = loadConfig({ ...base, DRY_RUN: "1", HONEYPOT_TEST_SELF: "1" });
        expect(both).toMatchObject({ dryRun: true, testSelf: true });
    });

    test("refuses a flag that is not exactly 1 or 0, so a typo cannot go live by accident", () => {
        expect(() => loadConfig({ ...base, DRY_RUN: "true" })).toThrow(/DRY_RUN/);
    });

    test("requires the token and a valid channel id", () => {
        expect(() => loadConfig({ HONEYPOT_CHANNEL_ID: HONEYPOT })).toThrow(/NXBOX_DISCORD_TOKEN/);
        expect(() => loadConfig({ ...base, HONEYPOT_CHANNEL_ID: "general" })).toThrow(/channel id/);
        expect(() => loadConfig({ NXBOX_DISCORD_TOKEN: base.NXBOX_DISCORD_TOKEN })).toThrow(
            /channel id/,
        );
    });

    test("error messages never contain the token", () => {
        const bad = { ...base, HONEYPOT_CHANNEL_ID: "nope", DRY_RUN: "x" };
        for (const env of [bad, { ...base, HONEYPOT_CHANNEL_ID: "nope" }]) {
            try {
                loadConfig(env);
            } catch (e) {
                expect((e as Error).message).not.toContain(base.NXBOX_DISCORD_TOKEN);
            }
        }
    });
});

describe("softban against a fake Discord", () => {
    type Step = number | "throw" | [number, unknown];

    /** Answers with the given steps in order; the last one repeats forever. */
    function fakeFetch(steps: Step[]) {
        const calls: {
            method: string;
            url: string;
            headers: Record<string, string>;
            body?: string;
        }[] = [];
        let i = 0;
        const fn = (async (url: string, init: RequestInit) => {
            calls.push({
                method: String(init.method),
                url,
                headers: init.headers as Record<string, string>,
                body: init.body as string | undefined,
            });
            const step = steps[Math.min(i++, steps.length - 1)];
            if (step === "throw") throw new Error("network down");
            const [status, body] = typeof step === "number" ? [step, undefined] : step;
            return new Response(
                status === 204 || body === undefined ? null : JSON.stringify(body),
                {
                    status,
                },
            );
        }) as unknown as typeof fetch;
        return { fn, calls };
    }

    const sleeps: number[] = [];
    const sleep = async (ms: number) => {
        sleeps.push(ms);
    };
    const run = (steps: Step[]) => {
        sleeps.length = 0;
        const { fn, calls } = fakeFetch(steps);
        const rest = makeRest("test-token", fn, sleep);
        return { calls, outcome: softban(rest, "g1", "u1", sleep) };
    };
    const verbs = (calls: { method: string }[]) => calls.map((c) => c.method);

    test("bans with one day of message deletion, then unbans, with the audit log reason", async () => {
        const { calls, outcome } = run([204, 204]);
        expect(await outcome).toBe("ok");
        expect(calls.map((c) => `${c.method} ${c.url}`)).toEqual([
            "PUT https://discord.com/api/v10/guilds/g1/bans/u1",
            "DELETE https://discord.com/api/v10/guilds/g1/bans/u1",
        ]);
        expect(JSON.parse(calls[0].body as string)).toEqual({ delete_message_seconds: 86400 });
        expect(calls[1].body).toBeUndefined();
        for (const call of calls) {
            expect(call.headers["X-Audit-Log-Reason"]).toBe("honeypot softban");
            expect(call.headers.Authorization).toBe("Bot test-token");
        }
    });

    test("waits retry_after on a 429 and then carries on", async () => {
        const { calls, outcome } = run([[429, { retry_after: 1.5, global: false }], 204, 204]);
        expect(await outcome).toBe("ok");
        expect(verbs(calls)).toEqual(["PUT", "PUT", "DELETE"]);
        expect(sleeps).toEqual([1550]);
    });

    test("does not unban when the ban itself was refused", async () => {
        const { calls, outcome } = run([[403, { code: 50013, message: "Missing Permissions" }]]);
        expect(await outcome).toBe("ban failed status=403 code=50013");
        expect(verbs(calls)).toEqual(["PUT"]);
    });

    test("retries through a network error", async () => {
        const { calls, outcome } = run(["throw", 204, 204]);
        expect(await outcome).toBe("ok");
        expect(verbs(calls)).toEqual(["PUT", "PUT", "DELETE"]);
    });

    test("retries an unban that fails with a server error", async () => {
        const { calls, outcome } = run([204, 500, 204]);
        expect(await outcome).toBe("ok");
        expect(verbs(calls)).toEqual(["PUT", "DELETE", "DELETE"]);
    });

    test("treats 'unknown ban' on the unban as done", async () => {
        const { outcome } = run([204, [404, { code: 10026 }]]);
        expect(await outcome).toBe("ok");
    });

    test("reports loudly when the unban never goes through, and bans only once", async () => {
        const { calls, outcome } = run([204, 500]);
        expect(await outcome).toStartWith("UNBAN FAILED");
        expect(verbs(calls).filter((v) => v === "PUT")).toHaveLength(1);
        expect(verbs(calls).filter((v) => v === "DELETE").length).toBeGreaterThan(8);
    });
});
