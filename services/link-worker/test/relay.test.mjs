// node --test services/link-worker/test   (no packages needed)
import assert from "node:assert/strict";
import { test } from "node:test";
import worker, { TTL_MS, MAX_MESSAGES } from "../src/index.js";
import { fakeEnv } from "./fake-env.mjs";

const BASE = "https://link.sculptforge.app";
const TOPIC = "sculptforge-0123456789abcdef01234567";
const PAGE = "https://sculptforge.app";

const post = (env, topic, body, origin = PAGE) =>
    worker.fetch(new Request(`${BASE}/relay/${topic}`, { method: "POST", body, headers: origin ? { Origin: origin } : {} }), env);
const poll = (env, topic, since = 0) => worker.fetch(new Request(`${BASE}/relay/${topic}/json?poll=1&since=${since}`), env);
const lines = async (response) => (await response.text()).split("\n").filter(Boolean).map((l) => JSON.parse(l));

test("a posted message comes back to the desktop's poll in ntfy's format", async () => {
    const env = fakeEnv();
    const since = Math.floor(Date.now() / 1000) - 5;
    const r = await post(env, TOPIC, "c2VjcmV0X2FuZF9lbmNyeXB0ZWQ");
    assert.equal(r.status, 200);
    assert.equal(r.headers.get("Access-Control-Allow-Origin"), PAGE); // the page may read the reply
    const p = await poll(env, TOPIC, since);
    assert.equal(p.status, 200);
    const events = await lines(p);
    assert.equal(events.length, 1);
    assert.equal(events[0].event, "message");
    assert.equal(events[0].topic, TOPIC);
    assert.equal(events[0].message, "c2VjcmV0X2FuZF9lbmNyeXB0ZWQ");
    assert.ok(events[0].id.length > 0);
    // Older than `since`: not returned.
    assert.equal((await lines(await poll(env, TOPIC, events[0].time + 1))).length, 0);
    // Other topics are separate.
    assert.equal((await lines(await poll(env, "sculptforge-ffffffffffffffffffffffff"))).length, 0);
});

test("topics, messages, sizes and origins are checked", async () => {
    const env = fakeEnv();
    assert.equal((await post(env, "anything-else", "abc")).status, 400);
    assert.equal((await post(env, TOPIC, "not base64url!")).status, 400);
    assert.equal((await post(env, TOPIC, "")).status, 400);
    assert.equal((await post(env, TOPIC, "a".repeat(8193))).status, 413);
    assert.equal((await post(env, TOPIC, "abc", "https://evil.example")).status, 403);
    assert.equal((await post(env, TOPIC, "abc", null)).status, 200); // no Origin: not a browser (curl, tests)
    const get = await worker.fetch(new Request(`${BASE}/relay/${TOPIC}`), env);
    assert.equal(get.status, 405);
    assert.equal((await worker.fetch(new Request(`${BASE}/elsewhere`), env)).status, 404);
    assert.equal((await worker.fetch(new Request(`${BASE}/relay/${TOPIC}/json?since=x`), env)).status, 400);
    const pre = await worker.fetch(new Request(`${BASE}/relay/${TOPIC}`, { method: "OPTIONS", headers: { Origin: PAGE } }), env);
    assert.equal(pre.status, 204);
    assert.equal(pre.headers.get("Access-Control-Allow-Origin"), PAGE);
    assert.equal((await worker.fetch(new Request(`${BASE}/`), env)).status, 200);
});

test("a topic holds a few messages, then refuses", async () => {
    const env = fakeEnv();
    for (let i = 0; i < MAX_MESSAGES; ++i) assert.equal((await post(env, TOPIC, `m${i}`)).status, 200);
    assert.equal((await post(env, TOPIC, "one-more")).status, 429);
    assert.equal((await lines(await poll(env, TOPIC))).length, MAX_MESSAGES);
});

test("messages are deleted ten minutes after the first", async () => {
    const env = fakeEnv();
    await post(env, TOPIC, "abc");
    await env.fireAlarms(Date.now() + TTL_MS - 1000);
    assert.equal((await lines(await poll(env, TOPIC))).length, 1);
    await env.fireAlarms(Date.now() + TTL_MS + 1000);
    assert.equal((await lines(await poll(env, TOPIC))).length, 0);
    assert.equal((await post(env, TOPIC, "again")).status, 200); // the topic works again (a new alarm)
});
