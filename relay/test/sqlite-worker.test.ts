import assert from "node:assert/strict";
import test from "node:test";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { DatabaseSync } from "node:sqlite";
import worker from "../src/index.ts";
import { sha256Hex } from "../src/auth.ts";
import { buildBlobKey, buildLatestKey, buildRevisionKey } from "../src/validation.ts";

// The Worker runs here against REAL SQL (SQLite via node:sqlite, foreign keys ON like D1) and the real
// schema.sql. R2 is still a mock. D1's own batch/transaction behavior is emulated with BEGIN/COMMIT/ROLLBACK.

class SqliteD1 {
  db = new DatabaseSync(":memory:");
  sqlLog: string[] = [];
  constructor() {
    this.db.exec("PRAGMA foreign_keys = ON");
    this.db.exec(readFileSync(new URL("../schema.sql", import.meta.url), "utf8"));
  }
  prepare(sql: string) {
    const db = this.db;
    this.sqlLog.push(sql);
    return {
      bind(...params: unknown[]) {
        const run = async () => {
          // Real D1 reports total changes, including rows removed by ON DELETE CASCADE (verified on D1: deleting a
          // publish session with 3 blob rows reported changes = 4). sqlite3_changes() would hide that, so count
          // with total_changes() like D1 does.
          const total = () => Number((db.prepare("SELECT total_changes() AS n").get() as { n: number }).n);
          const before = total();
          db.prepare(sql).run(...(params as any[]));
          return { success: true, meta: { changes: total() - before } };
        };
        return {
          run,
          first: async () => (db.prepare(sql).get(...(params as any[])) as any) ?? null,
          all: async () => ({ success: true, results: db.prepare(sql).all(...(params as any[])) as any[] }),
        };
      },
    };
  }
  async batch(statements: Array<{ run(): Promise<unknown> }>) {
    this.db.exec("BEGIN");
    try {
      const results = [];
      for (const statement of statements) results.push(await statement.run());
      this.db.exec("COMMIT");
      return results;
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }
  }
  one<T = any>(sql: string, ...params: unknown[]): T { return this.db.prepare(sql).get(...(params as any[])) as T; }
  exec(sql: string, ...params: unknown[]) { this.db.prepare(sql).run(...(params as any[])); }
}

class R2 {
  objects = new Map<string, Uint8Array>();
  failPuts = 0;
  failDeletes = 0;
  listPageSize = 1000;
  // Counts calls so tests can assert how many storage operations (subrequests) a request makes.
  calls = { head: 0, get: 0, put: 0, delete: 0, list: 0 };
  async head(key: string) { this.calls.head++; const o = this.objects.get(key); return o ? { key, size: o.byteLength } : null; }
  async get(key: string) {
    this.calls.get++;
    const o = this.objects.get(key);
    if (!o) return null;
    return {
      size: o.byteLength, body: new Blob([o]).stream(),
      async arrayBuffer() { return o.slice().buffer; },
      async text() { return new TextDecoder().decode(o); },
      writeHttpMetadata(headers: Headers) { headers.set("Content-Type", "application/octet-stream"); },
    };
  }
  async put(key: string, body: string | Uint8Array | ReadableStream<Uint8Array>, options?: any) {
    this.calls.put++;
    if (this.failPuts > 0) { this.failPuts -= 1; throw new Error("put: Internal error. (10001)"); }
    const bytes = typeof body === "string" ? new TextEncoder().encode(body)
      : body instanceof Uint8Array ? body : new Uint8Array(await new Response(body).arrayBuffer());
    if (options?.sha512 && createHash("sha512").update(bytes).digest("hex") !== options.sha512) throw new Error("SHA-512 mismatch");
    this.objects.set(key, bytes);
    return { key };
  }
  async delete(key: string | string[]) {
    this.calls.delete++;
    if (this.failDeletes > 0) { this.failDeletes -= 1; throw new Error("delete: Internal error. (10001)"); }
    for (const k of Array.isArray(key) ? key : [key]) this.objects.delete(k);
  }
  // Like R2: keys in order, at most listPageSize per call, and a cursor to continue.
  async list(options: { prefix?: string; cursor?: string; limit?: number }) {
    this.calls.list++;
    const limit = Math.min(options.limit ?? 1000, this.listPageSize);
    const rest = [...this.objects.keys()].sort()
      .filter((key) => key.startsWith(options.prefix ?? "") && (options.cursor === undefined || key > options.cursor));
    const page = rest.slice(0, limit);
    const truncated = rest.length > page.length;
    return {
      objects: page.map((key) => ({ key, size: this.objects.get(key)!.byteLength })),
      truncated,
      cursor: truncated ? page[page.length - 1] : undefined,
    };
  }
}

const TOKEN = "test-publisher-token-abcdefghijklmnop";
const SECRET = "test-pack-secret-abcdefghijklmnop";
const enc = (s: string) => new TextEncoder().encode(s);
const sha = (s: string) => createHash("sha512").update(s).digest("hex");

async function setup(quota = 10_000_000, extraEnv: Record<string, string> = {}) {
  const db = new SqliteD1();
  const bucket = new R2();
  db.exec(
    "INSERT INTO publishers (id,label,token_hash,quota_bytes,max_file_bytes,max_packs,created_at) VALUES (?,?,?,?,?,?,?)",
    "pub_test", "Test", await sha256Hex(TOKEN), quota, 5_000_000, 5, Date.now(),
  );
  const env = { DB: db as any, BUCKET: bucket as any, ...extraEnv };
  const call = (path: string, body: unknown, token = TOKEN, method = "POST") => worker.fetch(new Request(`https://relay.test${path}`, {
    method, headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` }, body: JSON.stringify(body),
  }), env);
  const put = (path: string, data: Uint8Array | string) => {
    const bytes = typeof data === "string" ? enc(data) : data;
    return worker.fetch(new Request(`https://relay.test${path}`, {
      method: "PUT", headers: { Authorization: `Bearer ${TOKEN}`, "Content-Length": String(bytes.length) }, body: bytes,
    }), env);
  };
  const read = (token: string, rel: string) => worker.fetch(new Request(`https://relay.test/read/${token}/${rel}`), env);
  return { db, bucket, env, call, put, read };
}

type Ctx = Awaited<ReturnType<typeof setup>>;

function manifestFor(packId: string, revision: number, data: string[]) {
  return JSON.stringify({
    formatVersion: 1, packId, name: "Test Pack", revision,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: data.map((d, i) => ({ path: `mods/f${i}.jar`, size: enc(d).length, sha512: sha(d) })),
  });
}

async function begin(ctx: Ctx, packId: string, readToken: string, revision: number, data: string[]) {
  const manifest = manifestFor(packId, revision, data);
  const response = await ctx.call("/publish/begin", {
    packId, revision, packSecret: SECRET, readToken, prefix: "", manifestBytes: enc(manifest).length,
    blobs: data.map((d) => ({ sha512: sha(d), size: enc(d).length })),
  });
  return { response, manifest };
}

// Runs begin -> prepare pages -> uploads -> revision -> (optionally) commit. Returns what the server said.
async function publish(ctx: Ctx, packId: string, readToken: string, revision: number, data: string[], commit = true) {
  const { response, manifest } = await begin(ctx, packId, readToken, revision, data);
  assert.equal(response.status, 200, await response.clone().text());
  const sessionId = (await response.json() as any).sessionId as string;
  const missing: string[] = [];
  for (let offset = 0; ;) {
    const page = await (await ctx.call("/publish/prepare", { sessionId, offset })).json() as any;
    missing.push(...page.missingBlobs);
    offset = page.nextOffset;
    if (page.complete) break;
  }
  for (const d of data) {
    if (!missing.includes(sha(d))) continue;
    assert.equal((await ctx.put(`/publish/upload/${sessionId}/${sha(d)}`, d)).status, 200);
  }
  assert.equal((await ctx.put(`/publish/revision/${sessionId}`, manifest)).status, 200);
  const result = commit
    ? await ctx.call("/publish/commit", { packId, revision, packSecret: SECRET, sessionId })
    : null;
  return { sessionId, missing, commit: result, manifest };
}

const count = (ctx: Ctx, table: string) => ctx.db.one<{ n: number }>(`SELECT COUNT(*) AS n FROM ${table}`).n;

test("[real SQL] full publish, second revision reuses stored blobs, readers see the new revision", async () => {
  const ctx = await setup();
  const read1 = "read-token-sql-flowabcdefghijklmnop";
  const first = await publish(ctx, "Pack_sql_01", read1, 1, ["alpha", "beta"]);
  assert.equal(first.commit!.status, 200);
  assert.equal(ctx.db.one("SELECT current_rev AS r FROM packs WHERE pack_id = 'Pack_sql_01'").r, 1);
  assert.equal(count(ctx, "publish_sessions"), 0);
  assert.equal(count(ctx, "publish_session_blobs"), 0);
  assert.equal(JSON.parse(await (await ctx.read(read1, "latest.json")).text()).revision, 1);
  assert.equal(await (await ctx.read(read1, `files/${sha("alpha")}`)).text(), "alpha");

  const second = await publish(ctx, "Pack_sql_01", read1, 2, ["alpha", "gamma"]);
  assert.deepEqual(second.missing, [sha("gamma")]); // alpha already stored: not requested again
  assert.equal(second.commit!.status, 200);
  const pack = ctx.db.one("SELECT current_rev, stored_bytes FROM packs WHERE pack_id = 'Pack_sql_01'");
  assert.equal(pack.current_rev, 2);
  assert.ok(pack.stored_bytes > 0);
  assert.equal(JSON.parse(await (await ctx.read(read1, "latest.json")).text()).revision, 2);
});

test("[real SQL] one session per pack, cancel grace, expiry cleanup, and FK cascade", async () => {
  const ctx = await setup();
  const readToken = "read-token-sql-abortabcdefghijklmn";
  const first = await begin(ctx, "Pack_sql_02", readToken, 1, ["alpha"]);
  assert.equal(first.response.status, 200);
  const sessionId = (await first.response.json() as any).sessionId;
  await ctx.call("/publish/prepare", { sessionId, offset: 0 });
  assert.equal(count(ctx, "publish_session_blobs"), 1);
  assert.equal((await begin(ctx, "Pack_sql_02", readToken, 1, ["alpha"])).response.status, 409); // already active

  assert.equal((await ctx.call("/publish/abort", { sessionId })).status, 200);
  assert.ok(ctx.db.one("SELECT expires_at AS e FROM publish_sessions").e <= Date.now() + 2 * 60_000);
  assert.equal((await begin(ctx, "Pack_sql_02", readToken, 1, ["alpha"])).response.status, 409); // still in grace

  ctx.db.exec("UPDATE publish_sessions SET expires_at = 1");
  assert.equal((await begin(ctx, "Pack_sql_02", readToken, 1, ["alpha"])).response.status, 200);
  assert.equal(count(ctx, "publish_sessions"), 1);
  assert.equal(count(ctx, "publish_session_blobs"), 0); // ON DELETE CASCADE removed the old session's rows
});

test("[real SQL] rotation revokes the old token, is retry-safe, and is blocked while a publish is open", async () => {
  const ctx = await setup();
  const oldToken = "read-token-sql-rotateabcdefghijklmn";
  await publish(ctx, "Pack_sql_03", oldToken, 1, ["alpha"]);
  const open = await begin(ctx, "Pack_sql_03", oldToken, 2, ["alpha", "beta"]);
  assert.equal(open.response.status, 200);
  const rotate = () => ctx.call("/publish/rotate", { packId: "Pack_sql_03", packSecret: SECRET, oldReadToken: oldToken });
  assert.equal((await rotate()).status, 409);
  assert.equal((await ctx.read(oldToken, "latest.json")).status, 200);

  const sessionId = (await open.response.json() as any).sessionId;
  await ctx.call("/publish/abort", { sessionId });
  ctx.db.exec("UPDATE publish_sessions SET expires_at = 1");
  // Cleanup runs on begin; an unrelated pack's begin clears the expired session.
  assert.equal((await begin(ctx, "Pack_sql_other", "read-token-sql-otherabcdefghijklmnop", 1, ["x"])).response.status, 200);
  const first = await rotate();
  assert.equal(first.status, 200);
  const newToken = (await first.json() as any).readToken as string;
  assert.equal((await ctx.read(oldToken, "latest.json")).status, 404);
  assert.equal((await ctx.read(newToken, "latest.json")).status, 200);
  const retry = await rotate();
  assert.equal(retry.status, 200);
  assert.equal((await retry.json() as any).readToken, newToken);
});

test("[real SQL] interrupted commit: rolled back without a marker, rolled forward with one", async () => {
  const ctx = await setup();
  const rb = "read-token-sql-rollbackabcdefghijklm";
  const staged = await publish(ctx, "Pack_sql_04", rb, 1, ["alpha"], false);
  const commit = () => ctx.call("/publish/commit", { packId: "Pack_sql_04", revision: 1, packSecret: SECRET, sessionId: staged.sessionId });
  ctx.bucket.failPuts = 1;
  assert.equal((await commit()).status, 500);
  assert.equal(ctx.db.one("SELECT status AS s FROM publish_sessions").s, "committing");
  ctx.db.exec("UPDATE publish_sessions SET status_updated_at = 1");
  assert.equal((await begin(ctx, "Pack_sql_05", "read-token-sql-nextabcdefghijklmnopqrs", 1, ["x"])).response.status, 200);
  assert.equal(ctx.db.one("SELECT COUNT(*) AS n FROM packs WHERE pack_id = 'Pack_sql_04'").n, 0);
  assert.equal(ctx.bucket.objects.has(buildBlobKey(sha("alpha"), rb)), false);
  assert.equal(ctx.bucket.objects.has(buildRevisionKey(rb, 1)), false);

  const fw = "read-token-sql-forwardabcdefghijklmn";
  const second = await publish(ctx, "Pack_sql_06", fw, 1, ["beta"], false);
  ctx.bucket.failPuts = 1;
  assert.equal((await ctx.call("/publish/commit", { packId: "Pack_sql_06", revision: 1, packSecret: SECRET, sessionId: second.sessionId })).status, 500);
  await ctx.bucket.put(buildLatestKey(fw), JSON.stringify({ formatVersion: 1, packId: "Pack_sql_06", revision: 1 }));
  ctx.db.exec("UPDATE publish_sessions SET status_updated_at = 1 WHERE session_id = ?", second.sessionId);
  assert.equal((await begin(ctx, "Pack_sql_07", "read-token-sql-more1abcdefghijklmnopqr", 1, ["y"])).response.status, 200);
  assert.equal(ctx.db.one("SELECT current_rev AS r FROM packs WHERE pack_id = 'Pack_sql_06'").r, 1);
  assert.equal(ctx.db.one("SELECT COUNT(*) AS n FROM publish_sessions WHERE session_id = ?", second.sessionId).n, 0);
  assert.equal(ctx.bucket.objects.has(buildBlobKey(sha("beta"), fw)), true);
});

test("[real SQL] per-publisher quota and pack-count limits are enforced atomically in begin", async () => {
  const ctx = await setup(2_000);
  const big = "x".repeat(1_500);
  assert.equal((await begin(ctx, "Pack_sql_08", "read-token-sql-quota1abcdefghijklmnopq", 1, [big])).response.status, 200);
  // The first session reserved ~1,500 bytes + manifest, so a second pack cannot fit under the 2,000-byte quota.
  const second = await begin(ctx, "Pack_sql_09", "read-token-sql-quota2abcdefghijklmnopq", 1, [big]);
  assert.ok([403, 409].includes(second.response.status), `status ${second.response.status}`);
  assert.equal(ctx.db.one("SELECT COUNT(*) AS n FROM packs WHERE pack_id = 'Pack_sql_09'").n, 0); // no orphan pack row
  ctx.db.exec("UPDATE publishers SET quota_bytes = 10000000, max_packs = 1");
  assert.equal((await begin(ctx, "Pack_sql_10", "read-token-sql-quota3abcdefghijklmnopq", 1, ["z"])).response.status, 403);
});

test("[real SQL] a publisher code redeems exactly once and the new publisher can publish", async () => {
  const ctx = await setup();
  const code = "secret_code_1234567890";
  ctx.db.exec("INSERT INTO publisher_codes (code_hash,label,expires_at) VALUES (?,?,?)", await sha256Hex(code), "Alice", Date.now() + 100_000);
  const redeem = () => worker.fetch(new Request("https://relay.test/publisher/redeem", {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ code }),
  }), ctx.env);
  const first = await redeem();
  assert.equal(first.status, 200);
  const body = await first.json() as any;
  assert.equal((await redeem()).status, 400);
  assert.equal(ctx.db.one("SELECT COUNT(*) AS n FROM publishers WHERE label = 'Alice'").n, 1);
  assert.equal(ctx.db.one("SELECT max_file_bytes AS m FROM publishers WHERE id = ?", body.publisherId).m, 100_000_000);
  const response = await ctx.call("/publish/check", { readToken: "read-token-sql-checkabcdefghijklmnopq", blobs: [] }, body.publisherToken);
  assert.equal(response.status, 200);
});


// ---- Scale and limit hardening ----

test("[real SQL] counted requests are limited per minute (default 120, configurable) and recover after the window", async () => {
  const check = (ctx: Ctx) => ctx.call("/publish/check", { readToken: "read-token-sql-limitabcdefghijklmnopq", blobs: [] });

  const defaults = await setup();
  defaults.db.exec("UPDATE publishers SET request_count = 119, rate_window_start = ?", Date.now());
  assert.equal((await check(defaults)).status, 200); // request 120 passes
  assert.equal((await check(defaults)).status, 429); // request 121 is refused
  defaults.db.exec("UPDATE publishers SET rate_window_start = 1");
  assert.equal((await check(defaults)).status, 200); // a new window starts

  const custom = await setup(10_000_000, { PUBLISHER_REQUESTS_PER_MINUTE: "3" });
  for (let i = 0; i < 3; i++) assert.equal((await check(custom)).status, 200);
  assert.equal((await check(custom)).status, 429);

  const invalid = await setup(10_000_000, { PUBLISHER_REQUESTS_PER_MINUTE: "0" });
  invalid.db.exec("UPDATE publishers SET request_count = 119, rate_window_start = ?", Date.now());
  assert.equal((await check(invalid)).status, 200); // an invalid value falls back to the default
  assert.equal((await check(invalid)).status, 429);
});

test("[real SQL] commit verifies blobs with paged list calls, not one head per blob", async () => {
  const ctx = await setup();
  const readToken = "read-token-sql-listabcdefghijklmnopqr";
  const data = ["a1", "b22", "c333", "d4444", "e55555"];
  const staged = await publish(ctx, "Pack_sql_20", readToken, 1, data, false);
  ctx.bucket.listPageSize = 2; // 5 blobs need 3 pages, so the cursor loop is exercised
  ctx.bucket.calls = { head: 0, get: 0, put: 0, delete: 0, list: 0 };
  const commit = await ctx.call("/publish/commit", { packId: "Pack_sql_20", revision: 1, packSecret: SECRET, sessionId: staged.sessionId });
  assert.equal(commit.status, 200);
  assert.equal(ctx.bucket.calls.list, 3);
  assert.ok(ctx.bucket.calls.head <= 1, `head calls: ${ctx.bucket.calls.head}`); // only the manifest, never per blob
});

test("[real SQL] commit rejects a blob that went missing or has the wrong size", async () => {
  const ctx = await setup();
  const readToken = "read-token-sql-verifyabcdefghijklmnopq";
  const commit = (packId: string, sessionId: string) => ctx.call("/publish/commit", { packId, revision: 1, packSecret: SECRET, sessionId });

  const missing = await publish(ctx, "Pack_sql_21", readToken, 1, ["alpha", "beta"], false);
  ctx.bucket.objects.delete(buildBlobKey(sha("beta"), readToken));
  assert.equal((await commit("Pack_sql_21", missing.sessionId)).status, 400);
  assert.equal(ctx.db.one("SELECT current_rev AS r FROM packs WHERE pack_id = 'Pack_sql_21'").r, 0);
  assert.equal(ctx.bucket.objects.has(buildLatestKey(readToken)), false);

  const otherToken = "read-token-sql-verify2abcdefghijklmnop";
  const wrongSize = await publish(ctx, "Pack_sql_22", otherToken, 1, ["alpha", "beta"], false);
  ctx.bucket.objects.set(buildBlobKey(sha("beta"), otherToken), enc("bet")); // same key, different size
  assert.equal((await commit("Pack_sql_22", wrongSize.sessionId)).status, 400);
  assert.equal(ctx.db.one("SELECT current_rev AS r FROM packs WHERE pack_id = 'Pack_sql_22'").r, 0);
});

test("[real SQL] an expired revision is cleaned with one batched delete, and a cleanup failure does not fail the commit", async () => {
  const ctx = await setup();
  const readToken = "read-token-sql-expireabcdefghijklmnopq";
  const pack = "Pack_sql_23";
  for (const [rev, files] of [[1, ["a1", "a2"]], [2, ["b1"]], [3, ["c1"]]] as const) {
    assert.equal((await publish(ctx, pack, readToken, rev, [...files])).commit!.status, 200);
  }
  ctx.bucket.calls.delete = 0;
  assert.equal((await publish(ctx, pack, readToken, 4, ["d1"])).commit!.status, 200);
  assert.equal(ctx.bucket.calls.delete, 1);
  assert.equal(ctx.bucket.objects.has(buildRevisionKey(readToken, 1)), false);
  assert.equal(ctx.bucket.objects.has(buildBlobKey(sha("a1"), readToken)), false);
  assert.equal(ctx.bucket.objects.has(buildBlobKey(sha("a2"), readToken)), false);
  assert.equal(ctx.bucket.objects.has(buildBlobKey(sha("b1"), readToken)), true); // still referenced by revision 2

  ctx.bucket.failDeletes = 1; // revision 2 expires now, but its cleanup fails
  const fifth = await publish(ctx, pack, readToken, 5, ["e1"]);
  assert.equal(fifth.commit!.status, 200);
  assert.equal(ctx.db.one("SELECT current_rev AS r FROM packs WHERE pack_id = ?", pack).r, 5);
  assert.equal(JSON.parse(await (await ctx.read(readToken, "latest.json")).text()).revision, 5);
});

test("[real SQL] blob upload checks go through one narrow query and keep their status codes", async () => {
  const ctx = await setup();
  const readToken = "read-token-sql-uploadabcdefghijklmnopq";
  const { response } = await begin(ctx, "Pack_sql_24", readToken, 1, ["alpha"]);
  const sessionId = (await response.json() as any).sessionId as string;
  const upload = (hash: string, body: string) => ctx.put(`/publish/upload/${sessionId}/${hash}`, body);

  assert.equal((await upload(sha("alpha"), "alpha")).status, 409); // preparation not finished
  await ctx.call("/publish/prepare", { sessionId, offset: 0 });
  assert.equal((await upload(sha("unknown"), "unknown")).status, 403); // not part of this session
  assert.equal((await upload(sha("alpha"), "alphaX")).status, 400); // size differs from the manifest

  ctx.db.sqlLog.length = 0;
  assert.equal((await upload(sha("alpha"), "alpha")).status, 200);
  const uploadSql = ctx.db.sqlLog.join("\n");
  assert.ok(!/SELECT \* FROM publish_sessions/.test(uploadSql), "upload must not load the whole session row");
  assert.ok(!uploadSql.includes("missing_blobs_json"), "upload must not read the missing-blob list");
  assert.equal((await upload(sha("alpha"), "alpha")).status, 200); // idempotent
  assert.equal(ctx.db.one("SELECT status AS s FROM publish_session_blobs").s, "uploaded");
});

test("an unexpected storage error returns 500 and logs only the error message", async () => {
  const ctx = await setup();
  const logged: unknown[][] = [];
  const original = console.error;
  console.error = (...args: unknown[]) => { logged.push(args); };
  try {
    ctx.env.DB = { prepare() { throw new Error("D1_ERROR: boom"); } } as any;
    const response = await ctx.call("/publish/check", { readToken: "read-token-sql-errorabcdefghijklmnopq", blobs: [] });
    assert.equal(response.status, 500);
  } finally {
    console.error = original;
  }
  assert.deepEqual(logged, [["relay error:", "D1_ERROR: boom"]]);
});
