import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import worker from "../src/index.ts";
import { sha256Hex, encodePublisherCode } from "../src/auth.ts";
import { buildBlobKey, buildLatestKey, buildRevisionKey } from "../src/validation.ts";

type Row = Record<string, any>;
type RunResult = { success: boolean; meta: { changes: number } };

class MockD1 {
  publishers = new Map<string, Row>();
  publisher_codes = new Map<string, Row>();
  packs = new Map<string, Row>();
  publish_sessions = new Map<string, Row>();
  publish_session_blobs = new Map<string, Row>();

  prepare(sql: string) {
    const database = this;
    return {
      bind(...params: any[]) {
        return {
          async all<T = Row>() {
            if (sql.includes("FROM publish_sessions") && sql.includes("expires_at <= ?")) {
              const [now] = params;
              return {
                success: true,
                results: [...database.publish_sessions.values()]
                  .filter((session) => session.expires_at <= now &&
                    ["active", "aborted", "cleaning"].includes(session.status))
                  .slice(0, 10) as T[],
              };
            }
            if (sql.includes("FROM publish_sessions") && sql.includes("status = 'committing' AND status_updated_at <= ?")) {
              const [staleBefore] = params;
              return {
                success: true,
                results: [...database.publish_sessions.values()]
                  .filter((session) => session.status === "committing" && session.status_updated_at <= staleBefore) as T[],
              };
            }
            return { success: true, results: [] as T[] };
          },
          async first<T = Row>(): Promise<T | null> {
            if (sql.includes("FROM publishers WHERE token_hash =")) {
              const [hash] = params;
              return [...database.publishers.values()].find((row) => row.token_hash === hash && row.revoked_at === null) as T ?? null;
            }
            if (sql.includes("json_array_length(s.blob_sizes_json)")) {
              const [sha512, sessionId, publisherId] = params;
              const session = database.publish_sessions.get(sessionId);
              const pack = session ? database.packs.get(session.pack_id) : undefined;
              if (!session || session.publisher_id !== publisherId || !pack || pack.publisher_id !== session.publisher_id) return null;
              const blob = database.publish_session_blobs.get(`${sessionId}:${sha512}`);
              let blobCount = 0;
              try { blobCount = (JSON.parse(session.blob_sizes_json) as unknown[]).length; } catch { /* damaged row */ }
              return {
                status: session.status, expires_at: session.expires_at, next_blob_offset: session.next_blob_offset,
                blob_count: blobCount, storage_token: pack.storage_token, prefix: pack.prefix,
                size_bytes: blob?.size_bytes ?? null, blob_status: blob?.status ?? null,
              } as T;
            }
            if (sql.includes("SELECT status, status_updated_at FROM publish_session_blobs")) {
              return (database.publish_session_blobs.get(`${params[0]}:${params[1]}`) ?? null) as T | null;
            }
            if (sql.includes("COUNT(*) AS count FROM publish_session_blobs")) {
              const count = [...database.publish_session_blobs.values()]
                .filter((blob) => blob.session_id === params[0] && blob.status !== "uploaded").length;
              return { count } as T;
            }
            if (sql.includes("SELECT * FROM packs WHERE read_token =")) {
              return [...database.packs.values()].find((row) => row.read_token === params[0]) as T ?? null;
            }
            if (sql.includes("SELECT label FROM publishers WHERE id =")) {
              return (database.publishers.get(params[0]) ?? null) as T | null;
            }
            if (sql.includes("SELECT * FROM packs WHERE pack_id =")) {
              return (database.packs.get(params[0]) ?? null) as T | null;
            }
            if (sql.includes("FROM publish_sessions WHERE session_id =")) {
              const [sessionId, packOrPublisherId, publisherId] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session) return null;
              if (sql.includes("pack_id = ? AND publisher_id = ?")) {
                return session.pack_id === packOrPublisherId && session.publisher_id === publisherId ? session as T : null;
              }
              return session.publisher_id === packOrPublisherId ? session as T : null;
            }
            if (sql.includes("SELECT current_rev FROM packs WHERE pack_id =")) {
              const pack = database.packs.get(params[0]);
              return pack ? { current_rev: pack.current_rev } as T : null;
            }
            return null;
          },
          async run(): Promise<RunResult> {
            if (sql.includes("UPDATE publishers") && sql.includes("rate_window_start")) {
              const [cutoff, now, resetCutoff, publisherId, whereCutoff, limit] = params;
              const publisher = database.publishers.get(publisherId);
              if (!publisher || publisher.revoked_at !== null ||
                  !(publisher.rate_window_start <= whereCutoff || publisher.request_count < limit)) {
                return result(0);
              }
              if (publisher.rate_window_start <= cutoff) {
                publisher.rate_window_start = now;
                publisher.request_count = 1;
              } else {
                publisher.request_count++;
              }
              return result(1);
            }
            if (sql.includes("UPDATE publish_sessions SET status = 'cleaning'") &&
                sql.includes("status = 'committing' AND status_updated_at <= ?")) {
              const [updatedAt, sessionId, staleBefore] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.status !== "committing" || session.status_updated_at > staleBefore) return result(0);
              session.status = "cleaning";
              session.status_updated_at = updatedAt;
              return result(1);
            }
            if (sql.includes("UPDATE publish_sessions SET status = 'cleaning'")) {
              const [updatedAt, sessionId, now, retryBefore] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.expires_at > now ||
                  !(["active", "aborted"].includes(session.status) ||
                    (session.status === "cleaning" && session.status_updated_at <= retryBefore))) return result(0);
              session.status = "cleaning";
              session.status_updated_at = updatedAt;
              return result(1);
            }
            if (sql.includes("INSERT OR IGNORE INTO packs")) {
              const [packId, publisherId, secretHash, readToken, storageToken, prefix, createdAt, countPublisherId, maxPacks] = params;
              if (database.packs.has(packId)) return result(0);
              const publisherPackCount = [...database.packs.values()].filter((pack) =>
                pack.publisher_id === countPublisherId
              ).length;
              if (publisherPackCount >= maxPacks) return result(0);
              if ([...database.packs.values()].some((pack) => pack.read_token === readToken)) return result(0);
              database.packs.set(packId, {
                pack_id: packId,
                publisher_id: publisherId,
                pack_secret_hash: secretHash,
                read_token: readToken,
                previous_read_token: null,
                storage_token: storageToken,
                prefix,
                current_rev: 0,
                stored_bytes: 0,
                created_at: createdAt,
              });
              return result(1);
            }
            if (sql.includes("INSERT INTO publish_sessions")) {
              const [sessionId, revision, reservedBytes, blobSizes, revisionSize, expiresAt,
                packId, publisherId, secretHash, readToken, prefix, baseline, publisherReserve, globalReserve, globalLimit] = params;
              const pack = database.packs.get(packId);
              const publisher = database.publishers.get(publisherId);
              const existingSession = [...database.publish_sessions.values()].some((session) => session.pack_id === packId);
              const publisherStored = [...database.packs.values()].filter((row) => row.publisher_id === publisherId)
                .reduce((total, row) => total + row.stored_bytes, 0);
              const publisherReserved = [...database.publish_sessions.values()].filter((row) => row.publisher_id === publisherId)
                .reduce((total, row) => total + row.reserved_bytes, 0);
              const globalStored = [...database.packs.values()].reduce((total, row) => total + row.stored_bytes, 0);
              const globalReserved = [...database.publish_sessions.values()].reduce((total, row) => total + row.reserved_bytes, 0);
              if (!pack || !publisher || pack.publisher_id !== publisherId || pack.pack_secret_hash !== secretHash ||
                  pack.read_token !== readToken || pack.prefix !== prefix || pack.current_rev !== baseline ||
                  existingSession || publisherStored + publisherReserved + publisherReserve > publisher.quota_bytes ||
                  globalStored + globalReserved + globalReserve > globalLimit) return result(0);
              database.publish_sessions.set(sessionId, {
                session_id: sessionId, pack_id: packId, publisher_id: publisherId, revision,
                baseline, reserved_bytes: reservedBytes, blob_sizes_json: blobSizes,
                missing_blobs_json: "[]", revision_size: revisionSize, expires_at: expiresAt,
                next_blob_offset: 0,
                status: "active", status_updated_at: 0,
              });
              return result(1);
            }
            if (sql.includes("UPDATE publish_sessions") && sql.includes("SET next_blob_offset")) {
              const [nextOffset, releasedBytes, missingJson, sessionId, publisherId, offset] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.publisher_id !== publisherId || session.status !== "active" ||
                  session.next_blob_offset !== offset || session.reserved_bytes < releasedBytes) return result(0);
              session.next_blob_offset = nextOffset;
              session.reserved_bytes -= releasedBytes;
              session.missing_blobs_json = missingJson;
              return result(1);
            }
            if (sql.includes("INSERT INTO publish_session_blobs")) {
              const [sessionId, sha512, size, sessionCheckId, nextOffset] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.session_id !== sessionCheckId || session.next_blob_offset !== nextOffset) return result(0);
              database.publish_session_blobs.set(`${sessionId}:${sha512}`, {
                session_id: sessionId, sha512, size_bytes: size, status: "pending", status_updated_at: 0,
              });
              return result(1);
            }
            if (sql.includes("UPDATE publish_session_blobs SET status = 'uploading'")) {
              const [updatedAt, sessionId, sha512, retryBefore] = params;
              const blob = database.publish_session_blobs.get(`${sessionId}:${sha512}`);
              if (!blob || !(blob.status === "pending" ||
                  (blob.status === "uploading" && blob.status_updated_at <= retryBefore))) return result(0);
              blob.status = "uploading";
              blob.status_updated_at = updatedAt;
              return result(1);
            }
            if (sql.includes("UPDATE publish_session_blobs SET status = 'pending'")) {
              const [sessionId, sha512] = params;
              const blob = database.publish_session_blobs.get(`${sessionId}:${sha512}`);
              if (!blob || blob.status !== "uploading") return result(0);
              blob.status = "pending";
              blob.status_updated_at = 0;
              return result(1);
            }
            if (sql.includes("UPDATE publish_session_blobs SET status = 'uploaded'")) {
              const [updatedAt, sessionId, sha512] = params;
              const blob = database.publish_session_blobs.get(`${sessionId}:${sha512}`);
              if (!blob || blob.status !== "uploading") return result(0);
              blob.status = "uploaded";
              blob.status_updated_at = updatedAt;
              return result(1);
            }
            if (sql.includes("UPDATE packs") && sql.includes("SET previous_read_token = read_token")) {
              const [newToken, packId, publisherId, secretHash, oldToken, sessionPackId] = params;
              const pack = database.packs.get(packId);
              const hasSession = [...database.publish_sessions.values()].some((session) => session.pack_id === sessionPackId);
              if (!pack || pack.publisher_id !== publisherId || pack.pack_secret_hash !== secretHash ||
                  pack.read_token !== oldToken || hasSession) return result(0);
              pack.previous_read_token = pack.read_token;
              pack.read_token = newToken;
              return result(1);
            }
            if (sql.includes("UPDATE publish_sessions SET status = 'committing'")) {
              const [updatedAt, sessionId, packId, now] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.pack_id !== packId || session.status !== "active" || session.expires_at < now) {
                return result(0);
              }
              session.status = "committing";
              session.status_updated_at = updatedAt;
              return result(1);
            }
            if (sql.includes("UPDATE publish_sessions SET status = 'aborted'")) {
              const [updatedAt, graceExpiry, sessionId, publisherId, now] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.publisher_id !== publisherId || session.expires_at <= now ||
                  session.status !== "active") return result(0);
              session.status = "aborted";
              session.status_updated_at = updatedAt;
              session.expires_at = Math.min(session.expires_at, graceExpiry);
              return result(1);
            }
            if (sql.includes("UPDATE packs SET current_rev =")) {
              const [revision, storedBytes, packId, publisherId, baseline, sessionId, sessionPackId] = params;
              const pack = database.packs.get(packId);
              const session = database.publish_sessions.get(sessionId);
              if (!pack || pack.publisher_id !== publisherId || pack.current_rev !== baseline ||
                  !session || session.status !== "committing" || sessionPackId !== packId) return result(0);
              pack.current_rev = revision;
              pack.stored_bytes = storedBytes;
              return result(1);
            }
            if (sql.includes("DELETE FROM publish_sessions") && sql.includes("status = 'cleaning'")) {
              const [sessionId] = params;
              const session = database.publish_sessions.get(sessionId);
              if (!session || session.status !== "cleaning") return result(0);
              database.publish_sessions.delete(sessionId);
              for (const key of database.publish_session_blobs.keys()) {
                if (key.startsWith(`${sessionId}:`)) database.publish_session_blobs.delete(key);
              }
              return result(1);
            }
            if (sql.includes("DELETE FROM publish_sessions") && sql.includes("EXISTS (SELECT 1 FROM packs")) {
              const [sessionId, packId, packCheckId, revision] = params;
              const session = database.publish_sessions.get(sessionId);
              const pack = database.packs.get(packCheckId);
              if (!session || session.pack_id !== packId || pack?.current_rev !== revision) return result(0);
              database.publish_sessions.delete(sessionId);
              for (const key of database.publish_session_blobs.keys()) {
                if (key.startsWith(`${sessionId}:`)) database.publish_session_blobs.delete(key);
              }
              return result(1);
            }
            if (sql.includes("DELETE FROM packs")) {
              const [packId, sessionPackId] = params;
              const pack = database.packs.get(packId);
              const active = [...database.publish_sessions.values()].some((session) => session.pack_id === sessionPackId);
              if (!pack || pack.current_rev !== 0 || pack.stored_bytes !== 0 || active) return result(0);
              database.packs.delete(packId);
              return result(1);
            }
            if (sql.includes("INSERT INTO publishers")) {
              const [id, tokenHash, quotaBytes, maxFileBytes, maxPacks, createdAt, codeHash] = params;
              const code = database.publisher_codes.get(codeHash);
              if (!code || code.used_at !== null || code.expires_at < params[7]) return result(0);
              database.publishers.set(id, {
                id, label: code.label, token_hash: tokenHash, quota_bytes: quotaBytes,
                max_file_bytes: maxFileBytes, max_packs: maxPacks,
                created_at: createdAt, rate_window_start: 0, request_count: 0, revoked_at: null,
              });
              return result(1);
            }
            if (sql.includes("UPDATE publisher_codes SET used_at")) {
              const [usedAt, publisherId, codeHash] = params;
              const code = database.publisher_codes.get(codeHash);
              if (!code || code.used_at !== null || !database.publishers.has(publisherId)) return result(0);
              code.used_at = usedAt;
              code.publisher_id = publisherId;
              return result(1);
            }
            return result(0);
          },
        };
      },
    };
  }

  async batch(statements: Array<{ run(): Promise<RunResult> }>): Promise<RunResult[]> {
    const results: RunResult[] = [];
    for (const statement of statements) results.push(await statement.run());
    return results;
  }
}

function result(changes: number): RunResult {
  return { success: true, meta: { changes } };
}

class MockR2 {
  objects = new Map<string, { body: Uint8Array; metadata: unknown }>();

  async head(key: string) {
    const object = this.objects.get(key);
    return object ? { key, size: object.body.byteLength } : null;
  }

  async get(key: string) {
    const object = this.objects.get(key);
    if (!object) return null;
    const body = object.body;
    return {
      size: body.byteLength,
      body: new Blob([body]).stream(),
      async arrayBuffer() {
        return body.slice().buffer;
      },
      async text() {
        return new TextDecoder().decode(body);
      },
      writeHttpMetadata(headers: Headers) {
        headers.set("Content-Type", "application/octet-stream");
      },
    };
  }

  failPuts = 0;

  async put(key: string, body: string | Uint8Array | ReadableStream<Uint8Array>, metadata?: any) {
    if (this.failPuts > 0) {
      this.failPuts -= 1;
      throw new Error("put: Internal error. (10001)");
    }
    const bytes = typeof body === "string"
      ? new TextEncoder().encode(body)
      : body instanceof Uint8Array
        ? body
        : new Uint8Array(await new Response(body).arrayBuffer());
    if (metadata?.sha512) {
      const digest = createHash("sha512").update(bytes).digest("hex");
      if (digest !== metadata.sha512) throw new Error("R2 SHA-512 integrity mismatch");
    }
    this.objects.set(key, { body: bytes, metadata });
    return { key };
  }

  async delete(key: string | string[]) {
    if (Array.isArray(key)) key.forEach((item) => this.objects.delete(item));
    else this.objects.delete(key);
  }

  // Like R2: keys in order, at most `listPageSize` per call, and a cursor to continue.
  listPageSize = 1000;
  async list(options: { prefix?: string; cursor?: string; limit?: number }) {
    const limit = Math.min(options.limit ?? 1000, this.listPageSize);
    const rest = [...this.objects.keys()].sort()
      .filter((key) => key.startsWith(options.prefix ?? "") && (options.cursor === undefined || key > options.cursor));
    const page = rest.slice(0, limit);
    const truncated = rest.length > page.length;
    return {
      objects: page.map((key) => ({ key, size: this.objects.get(key)!.body.byteLength })),
      truncated,
      cursor: truncated ? page[page.length - 1] : undefined,
    };
  }
}

function createTestEnv() {
  const db = new MockD1();
  const bucket = new MockR2();
  return {
    env: {
      DB: db as any,
      BUCKET: bucket as any,
    },
    db,
    bucket,
  };
}

async function addPublisher(db: MockD1, token: string, id = "pub_test") {
  db.publishers.set(id, {
    id, label: "Test publisher", token_hash: await sha256Hex(token),
    quota_bytes: 10_000_000, max_file_bytes: 5_000_000, max_packs: 5,
    created_at: Date.now(), rate_window_start: 0, request_count: 0, revoked_at: null,
  });
}

function authorizedRequest(url: string, token: string, body: unknown): Request {
  return new Request(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${token}` },
    body: JSON.stringify(body),
  });
}

function authorizedUpload(url: string, token: string, body: string): Request {
  return new Request(url, {
    method: "PUT",
    headers: { Authorization: `Bearer ${token}`, "Content-Length": String(new TextEncoder().encode(body).length) },
    body,
  });
}

async function prepareSession(env: unknown, publisherToken: string, sessionId: string) {
  return worker.fetch(
    authorizedRequest("https://relay.test/publish/prepare", publisherToken, { sessionId, offset: 0 }),
    env as any,
  );
}

test("GET /health returns 200 OK", async () => {
  const { env } = createTestEnv();
  const response = await worker.fetch(new Request("https://relay.test/health"), env);
  assert.equal(response.status, 200);
  const body = await response.json() as Record<string, unknown>;
  assert.equal(body.status, "ok");
  assert.equal(typeof body.time, "number");
});

test("publisher code is redeemed once and returns a publisher token", async () => {
  const { env, db } = createTestEnv();
  const code = "secret_code_1234567890";
  const codeHash = await sha256Hex(code);
  db.publisher_codes.set(codeHash, {
    code_hash: codeHash, label: "Alice", expires_at: Date.now() + 100_000, used_at: null, publisher_id: null,
  });
  const invite = encodePublisherCode({
    worker: "https://relay.test", code, label: "Alice", exp: Date.now() + 100_000,
  });

  const response = await worker.fetch(
    new Request("https://relay.test/publisher/redeem", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ code: invite }),
    }),
    env,
  );
  assert.equal(response.status, 200);
  const data = await response.json() as Record<string, any>;
  assert.equal(data.label, "Alice");
  assert.match(data.publisherId, /^pub_/);
  assert.ok(data.publisherToken);
  assert.notEqual(db.publisher_codes.get(codeHash)?.used_at, null);

  const secondResponse = await worker.fetch(
    new Request("https://relay.test/publisher/redeem", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ code }),
    }),
    env,
  );
  assert.equal(secondResponse.status, 400);
});

test("read-only publish checks report only blobs with the declared size", async () => {
  const { env, db, bucket } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const readToken = "read-token-checkabcdefghijklmnopqrst";
  const present = "1".repeat(128);
  const wrongSize = "2".repeat(128);
  const missing = "3".repeat(128);
  await addPublisher(db, publisherToken);
  await bucket.put(buildBlobKey(present, readToken), "right");
  await bucket.put(buildBlobKey(wrongSize, readToken), "short");

  const response = await worker.fetch(
    authorizedRequest("https://relay.test/publish/check", publisherToken, {
      readToken,
      blobs: [
        { sha512: present, size: 5 },
        { sha512: wrongSize, size: 9 },
        { sha512: missing, size: 4 },
      ],
    }),
    env,
  );
  assert.equal(response.status, 200);
  assert.deepEqual((await response.json() as Record<string, string[]>).presentBlobs, [present]);
  assert.equal(db.packs.size, 0);
  assert.equal(db.publish_sessions.size, 0);
});

test("begin pages missing blobs, verifies streamed SHA-512 uploads, and commits the revision", async () => {
  const { env, db, bucket } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const packId = "Pack_test_01";
  const packSecret = "test-pack-secret-abcdefghijklmnop";
  const readToken = "read-token-abcdefghijklmnopqrstuv";
  const blobBody = "data";
  const blobSha = createHash("sha512").update(blobBody).digest("hex");
  const manifest = JSON.stringify({
    formatVersion: 1,
    packId,
    name: "Test Pack",
    revision: 1,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: [{ path: "mods/test.jar", size: blobBody.length, sha512: blobSha }],
  });
  await addPublisher(db, publisherToken);

  const beginResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/begin", publisherToken, {
      packId,
      revision: 1,
      packSecret,
      readToken,
      prefix: "",
      manifestBytes: new TextEncoder().encode(manifest).byteLength,
      blobs: [{ sha512: blobSha, size: blobBody.length }],
    }),
    env,
  );
  assert.equal(beginResponse.status, 200);
  const begin = await beginResponse.json() as Record<string, any>;
  assert.ok(begin.sessionId);
  assert.equal(begin.blobCount, 1);
  assert.equal(db.publish_sessions.size, 1);

  const prepare = await prepareSession(env, publisherToken, begin.sessionId);
  assert.equal(prepare.status, 200);
  assert.deepEqual((await prepare.json() as Record<string, unknown>).missingBlobs, [blobSha]);

  const badDigest = await worker.fetch(
    authorizedUpload(`https://relay.test/publish/upload/${begin.sessionId}/${blobSha}`, publisherToken, "fail"),
    env,
  );
  assert.equal(badDigest.status, 400);
  const upload = await worker.fetch(
    authorizedUpload(`https://relay.test/publish/upload/${begin.sessionId}/${blobSha}`, publisherToken, blobBody),
    env,
  );
  assert.equal(upload.status, 200);

  const manifestBytes = new TextEncoder().encode(manifest);
  const revisionUpload = await worker.fetch(
    new Request(`https://relay.test/publish/revision/${begin.sessionId}`, {
      method: "PUT",
      headers: { Authorization: `Bearer ${publisherToken}`, "Content-Length": String(manifestBytes.length) },
      body: manifestBytes,
    }),
    env,
  );
  assert.equal(revisionUpload.status, 200);

  const commitResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/commit", publisherToken, {
      packId, revision: 1, packSecret, sessionId: begin.sessionId,
    }),
    env,
  );
  assert.equal(commitResponse.status, 200);
  assert.deepEqual(await commitResponse.json(), { ok: true, revision: 1 });
  assert.equal(db.packs.get(packId)?.current_rev, 1);
  assert.equal(db.publish_sessions.size, 0);
  const latest = await bucket.get(buildLatestKey(readToken));
  assert.ok(latest);
  assert.equal(JSON.parse(await latest.text()).revision, 1);
});

test("rotating a read token immediately revokes the old URL and is retry-safe", async () => {
  const { env, db, bucket } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const packId = "Pack_rotate_01";
  const packSecret = "test-pack-secret-abcdefghijklmnop";
  const oldToken = "read-token-before-rotation-abcdefghijkl";
  await addPublisher(db, publisherToken);
  db.packs.set(packId, {
    pack_id: packId,
    publisher_id: "pub_test",
    pack_secret_hash: await sha256Hex(packSecret),
    read_token: oldToken,
    previous_read_token: null,
    storage_token: "stable-storage-token-abcdefghijkl",
    prefix: "",
    current_rev: 1,
    stored_bytes: 0,
  });
  await bucket.put(buildLatestKey("stable-storage-token-abcdefghijkl"), JSON.stringify({ packId, revision: 1 }));

  const oldRead = await worker.fetch(new Request(`https://relay.test/read/${oldToken}/latest.json`), env);
  assert.equal(oldRead.status, 200);
  const rotation = async () => worker.fetch(
    authorizedRequest("https://relay.test/publish/rotate", publisherToken, { packId, packSecret, oldReadToken: oldToken }),
    env,
  );
  const first = await rotation();
  assert.equal(first.status, 200);
  const newToken = (await first.json() as Record<string, string>).readToken;
  assert.ok(newToken);
  assert.equal((await worker.fetch(new Request(`https://relay.test/read/${oldToken}/latest.json`), env)).status, 404);
  assert.equal((await worker.fetch(new Request(`https://relay.test/read/${newToken}/latest.json`), env)).status, 200);

  const retry = await rotation();
  assert.equal(retry.status, 200);
  assert.deepEqual(await retry.json(), { ok: true, readToken: newToken });
});

test("commit rejects an incomplete blob upload", async () => {
  const { env, db, bucket } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const packId = "Pack_test_02";
  const packSecret = "test-pack-secret-abcdefghijklmnop";
  const readToken = "read-token-zyxwvutsrqponmlkjihgfedcba";
  const blobSha = "d".repeat(128);
  const manifest = JSON.stringify({
    formatVersion: 1, packId, name: "Missing File", revision: 1,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: [{ path: "mods/missing.jar", size: 4, sha512: blobSha }],
  });
  await addPublisher(db, publisherToken);
  const beginResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/begin", publisherToken, {
      packId, revision: 1, packSecret, readToken, prefix: "",
      manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs: [{ sha512: blobSha, size: 4 }],
    }),
    env,
  );
  const begin = await beginResponse.json() as Record<string, any>;
  await prepareSession(env, publisherToken, begin.sessionId);
  const manifestBytes = new TextEncoder().encode(manifest);
  const revisionUpload = await worker.fetch(
    new Request(`https://relay.test/publish/revision/${begin.sessionId}`, {
      method: "PUT",
      headers: { Authorization: `Bearer ${publisherToken}`, "Content-Length": String(manifestBytes.length) },
      body: manifestBytes,
    }),
    env,
  );
  assert.equal(revisionUpload.status, 200);

  const response = await worker.fetch(
    authorizedRequest("https://relay.test/publish/commit", publisherToken, {
      packId, revision: 1, packSecret, sessionId: begin.sessionId,
    }),
    env,
  );
  assert.equal(response.status, 409);
  assert.equal(db.packs.get(packId)?.current_rev, 0);
  assert.equal(db.publish_sessions.size, 1);
});

test("aborting blocks commit and keeps the reservation until upload URLs expire", async () => {
  const { env, db } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const packId = "Pack_abort_01";
  const packSecret = "test-pack-secret-abcdefghijklmnop";
  const readToken = "read-token-abortabcdefghijklmnopqrst";
  const blobSha = "e".repeat(128);
  const manifest = JSON.stringify({
    formatVersion: 1, packId, name: "Cancelled", revision: 1,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: [{ path: "mods/file.jar", size: 4, sha512: blobSha }],
  });
  await addPublisher(db, publisherToken);
  const beginResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/begin", publisherToken, {
      packId, revision: 1, packSecret, readToken, prefix: "",
      manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs: [{ sha512: blobSha, size: 4 }],
    }),
    env,
  );
  const begin = await beginResponse.json() as Record<string, any>;
  await prepareSession(env, publisherToken, begin.sessionId);

  const abortResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/abort", publisherToken, { sessionId: begin.sessionId }),
    env,
  );
  assert.equal(abortResponse.status, 200);
  assert.equal(db.publish_sessions.get(begin.sessionId)?.status, "aborted");
  assert.deepEqual(await abortResponse.json(), { ok: true, reservationHeldUntilExpiry: true });

  const commitResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/commit", publisherToken, {
      packId, revision: 1, packSecret, sessionId: begin.sessionId,
    }),
    env,
  );
  assert.equal(commitResponse.status, 409);
  assert.equal(db.publish_sessions.size, 1);
});

test("expired sessions clean up only their newly reserved objects before retry", async () => {
  const { env, db, bucket } = createTestEnv();
  const publisherToken = "test-publisher-token-abcdefghijklmnop";
  const packId = "Pack_cleanup_01";
  const packSecret = "test-pack-secret-abcdefghijklmnop";
  const readToken = "read-token-cleanupabcdefghijklmnop";
  const blobSha = "f".repeat(128);
  const manifest = JSON.stringify({
    formatVersion: 1, packId, name: "Retry", revision: 1,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: [{ path: "mods/file.jar", size: 4, sha512: blobSha }],
  });
  await addPublisher(db, publisherToken);
  const beginResponse = await worker.fetch(
    authorizedRequest("https://relay.test/publish/begin", publisherToken, {
      packId, revision: 1, packSecret, readToken, prefix: "",
      manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs: [{ sha512: blobSha, size: 4 }],
    }),
    env,
  );
  const begin = await beginResponse.json() as Record<string, any>;
  await prepareSession(env, publisherToken, begin.sessionId);
  await bucket.put(buildBlobKey(blobSha, readToken), "data");
  await bucket.put(buildRevisionKey(readToken, 1), manifest);
  db.publish_sessions.get(begin.sessionId)!.expires_at = Date.now() - 1;

  const retry = await worker.fetch(
    authorizedRequest("https://relay.test/publish/begin", publisherToken, {
      packId, revision: 1, packSecret, readToken, prefix: "",
      manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs: [{ sha512: blobSha, size: 4 }],
    }),
    env,
  );
  assert.equal(retry.status, 200);
  assert.equal(db.publish_sessions.size, 1);
  assert.equal(bucket.objects.has(buildBlobKey(blobSha, readToken)), false);
  assert.equal(bucket.objects.has(buildRevisionKey(readToken, 1)), false);
});

// ---- Failure, retry and concurrency guarantees --------------------------------------------------
// These run against the in-memory mocks above: they prove the Worker's logic and SQL intent, not
// real D1 / R2 behavior.

const PUBLISHER_TOKEN = "test-publisher-token-abcdefghijklmnop";
const PACK_SECRET = "test-pack-secret-abcdefghijklmnop";
const sha = (value: string) => createHash("sha512").update(value).digest("hex");
type BlobSpec = { sha512: string; size: number };

function manifestFor(packId: string, blobs: BlobSpec[]): string {
  return JSON.stringify({
    formatVersion: 1, packId, name: "Test Pack", revision: 1,
    components: [{ uid: "net.minecraft", version: "1.21.1" }],
    files: blobs.map((blob, index) => ({ path: `mods/f${index}.jar`, size: blob.size, sha512: blob.sha512 })),
  });
}

async function beginWith(env: unknown, packId: string, readToken: string, blobs: BlobSpec[]) {
  const manifest = manifestFor(packId, blobs);
  const response = await worker.fetch(authorizedRequest("https://relay.test/publish/begin", PUBLISHER_TOKEN, {
    packId, revision: 1, packSecret: PACK_SECRET, readToken, prefix: "",
    manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs,
  }), env as any);
  return { response, manifest };
}

async function prepareAt(env: unknown, sessionId: string, offset: number) {
  return worker.fetch(authorizedRequest("https://relay.test/publish/prepare", PUBLISHER_TOKEN, { sessionId, offset }), env as any);
}

function putRequest(url: string, body: BodyInit, contentLength: number, extra: RequestInit = {}): Request {
  return new Request(url, {
    method: "PUT",
    headers: { Authorization: `Bearer ${PUBLISHER_TOKEN}`, "Content-Length": String(contentLength) },
    body,
    ...extra,
  });
}

const blobStatuses = (db: MockD1) => [...db.publish_session_blobs.values()].map((row) => row.status);

test("rejected or interrupted uploads store nothing, stay retryable, and a clean retry succeeds", async () => {
  const { env, db, bucket } = createTestEnv();
  const packId = "Pack_upload_01";
  const readToken = "read-token-uploadabcdefghijklmnopq";
  await addPublisher(db, PUBLISHER_TOKEN);
  const blobSha = sha("data");
  const { response } = await beginWith(env, packId, readToken, [{ sha512: blobSha, size: 4 }]);
  assert.equal(response.status, 200);
  const sessionId = (await response.json() as Record<string, string>).sessionId;
  assert.equal((await prepareAt(env, sessionId, 0)).status, 200);
  const url = `https://relay.test/publish/upload/${sessionId}/${blobSha}`;
  const key = buildBlobKey(blobSha, readToken);
  const expectNothingStored = () => {
    assert.equal(bucket.objects.has(key), false);
    assert.deepEqual(blobStatuses(db), ["pending"]);
  };

  // Declared Content-Length differs from the manifest size.
  assert.equal((await worker.fetch(putRequest(url, "datas", 5), env)).status, 400);
  expectNothingStored();
  // Header matches the manifest but the body is longer / shorter than declared.
  assert.equal((await worker.fetch(putRequest(url, "datas", 4), env)).status, 400);
  expectNothingStored();
  assert.equal((await worker.fetch(putRequest(url, "dat", 4), env)).status, 400);
  expectNothingStored();
  // Right size, wrong content: R2 rejects the SHA-512.
  assert.equal((await worker.fetch(putRequest(url, "fail", 4), env)).status, 400);
  expectNothingStored();
  // R2 outage is not the file's fault: retryable 502, not a 400.
  bucket.failPuts = 1;
  assert.equal((await worker.fetch(putRequest(url, "data", 4), env)).status, 502);
  expectNothingStored();
  // Connection dropped mid-body.
  const broken = new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(new TextEncoder().encode("da"));
      controller.error(new Error("client disconnected"));
    },
  });
  assert.equal((await worker.fetch(putRequest(url, broken, 4, { duplex: "half" } as RequestInit), env)).status, 502);
  expectNothingStored();

  assert.equal((await worker.fetch(putRequest(url, "data", 4), env)).status, 200);
  assert.deepEqual(blobStatuses(db), ["uploaded"]);
  assert.equal(await (await bucket.get(key))!.text(), "data");
  // A repeated upload of an already stored blob is accepted without rewriting it.
  assert.equal((await worker.fetch(putRequest(url, "data", 4), env)).status, 200);
});

test("preparation pages through 250 blobs in order, skips stored blobs, replays a lost response, and gates upload and commit", async () => {
  const { env, db, bucket } = createTestEnv();
  const packId = "Pack_pages_01";
  const readToken = "read-token-pagesabcdefghijklmnopqr";
  await addPublisher(db, PUBLISHER_TOKEN);
  const blobs = Array.from({ length: 250 }, (_, index) => ({ sha512: sha(`blob-${index}`), size: 1 }));
  await bucket.put(buildBlobKey(blobs[5].sha512, readToken), "x");     // stored with the right size
  await bucket.put(buildBlobKey(blobs[150].sha512, readToken), "x");   // stored with the right size
  await bucket.put(buildBlobKey(blobs[7].sha512, readToken), "xx");    // wrong size: must be treated as missing
  const { response, manifest } = await beginWith(env, packId, readToken, blobs);
  assert.equal(response.status, 200);
  const sessionId = (await response.json() as Record<string, string>).sessionId;
  const manifestBytes = new TextEncoder().encode(manifest).byteLength;
  const session = db.publish_sessions.get(sessionId)!;
  assert.equal(session.reserved_bytes, manifestBytes + 250);

  // Preparation is not complete: uploads and commit are refused, and rotation is blocked by the live session.
  assert.equal((await worker.fetch(putRequest(`https://relay.test/publish/upload/${sessionId}/${blobs[0].sha512}`, "x", 1), env)).status, 409);
  const commit = () => worker.fetch(authorizedRequest("https://relay.test/publish/commit", PUBLISHER_TOKEN, {
    packId, revision: 1, packSecret: PACK_SECRET, sessionId,
  }), env);
  assert.equal((await commit()).status, 409);
  const rotate = () => worker.fetch(authorizedRequest("https://relay.test/publish/rotate", PUBLISHER_TOKEN, {
    packId, packSecret: PACK_SECRET, oldReadToken: readToken,
  }), env);
  assert.equal((await rotate()).status, 409);
  assert.equal(db.packs.get(packId)!.read_token, readToken);

  assert.equal((await prepareAt(env, sessionId, 100)).status, 409); // skipping ahead
  const page = async (offset: number) => await (await prepareAt(env, sessionId, offset)).json() as { missingBlobs: string[]; nextOffset: number; complete: boolean };

  const first = await page(0);
  assert.equal(first.nextOffset, 100);
  assert.equal(first.complete, false);
  assert.equal(first.missingBlobs.length, 99);
  assert.ok(first.missingBlobs.includes(blobs[7].sha512));
  assert.ok(!first.missingBlobs.includes(blobs[5].sha512));
  assert.deepEqual(await page(0), first); // the response was lost; the identical retry gets the identical answer
  assert.equal((await prepareAt(env, sessionId, 200)).status, 409);

  const second = await page(100);
  assert.equal(second.nextOffset, 200);
  assert.equal(second.missingBlobs.length, 99);
  assert.ok(!second.missingBlobs.includes(blobs[150].sha512));
  assert.equal((await prepareAt(env, sessionId, 0)).status, 409); // only the most recent page can be replayed
  assert.deepEqual(await page(100), second);

  const third = await page(200);
  assert.equal(third.nextOffset, 250);
  assert.equal(third.complete, true);
  assert.equal(third.missingBlobs.length, 50);
  assert.deepEqual(await page(200), third);
  assert.equal(db.publish_sessions.get(sessionId)!.reserved_bytes, manifestBytes + 250 - 2);
  // Preparation is complete but nothing is uploaded yet.
  assert.equal((await commit()).status, 409);
});

test("a commit interrupted by an R2 error can be retried with the same session and then replays idempotently", async () => {
  const { env, db, bucket } = createTestEnv();
  const packId = "Pack_commit_01";
  const readToken = "read-token-commitabcdefghijklmnopq";
  await addPublisher(db, PUBLISHER_TOKEN);
  const blobSha = sha("data");
  const { response, manifest } = await beginWith(env, packId, readToken, [{ sha512: blobSha, size: 4 }]);
  const sessionId = (await response.json() as Record<string, string>).sessionId;
  await prepareAt(env, sessionId, 0);
  assert.equal((await worker.fetch(putRequest(`https://relay.test/publish/upload/${sessionId}/${blobSha}`, "data", 4), env)).status, 200);
  const manifestBytes = new TextEncoder().encode(manifest);
  assert.equal((await worker.fetch(putRequest(`https://relay.test/publish/revision/${sessionId}`, manifestBytes, manifestBytes.length), env)).status, 200);

  const commit = () => worker.fetch(authorizedRequest("https://relay.test/publish/commit", PUBLISHER_TOKEN, {
    packId, revision: 1, packSecret: PACK_SECRET, sessionId,
  }), env);
  bucket.failPuts = 1; // the pack marker write fails
  assert.equal((await commit()).status, 500);
  assert.equal(db.packs.get(packId)!.current_rev, 0);
  assert.equal(bucket.objects.has(buildLatestKey(readToken)), false);
  assert.equal(db.publish_sessions.get(sessionId)!.status, "committing");

  assert.equal((await commit()).status, 200);
  assert.equal(db.packs.get(packId)!.current_rev, 1);
  assert.equal(db.publish_sessions.size, 0);
  assert.equal((await commit()).status, 200); // lost response: the retry is acknowledged without a session
});

test("a damaged expired session is skipped and does not block other publishers from beginning a publish", async () => {
  const { env, db } = createTestEnv();
  await addPublisher(db, PUBLISHER_TOKEN);
  await addPublisher(db, "other-publisher-token-abcdefghijklmnop", "pub_other");
  db.packs.set("Pack_broken_01", {
    pack_id: "Pack_broken_01", publisher_id: "pub_test", pack_secret_hash: await sha256Hex(PACK_SECRET),
    read_token: "read-token-brokenabcdefghijklmnopq", previous_read_token: null,
    storage_token: "read-token-brokenabcdefghijklmnopq", prefix: "", current_rev: 0, stored_bytes: 0,
  });
  db.publish_sessions.set("sess_broken", {
    session_id: "sess_broken", pack_id: "Pack_broken_01", publisher_id: "pub_test", revision: 1, baseline: 0,
    reserved_bytes: 5, blob_sizes_json: "[]", missing_blobs_json: "not json", revision_size: 10, next_blob_offset: 0,
    expires_at: Date.now() - 1, status: "active", status_updated_at: 0,
  });
  const blobSha = sha("data");
  const manifest = manifestFor("Pack_ok_01", [{ sha512: blobSha, size: 4 }]);
  const response = await worker.fetch(authorizedRequest("https://relay.test/publish/begin", "other-publisher-token-abcdefghijklmnop", {
    packId: "Pack_ok_01", revision: 1, packSecret: PACK_SECRET, readToken: "read-token-okabcdefghijklmnopqrstu", prefix: "",
    manifestBytes: new TextEncoder().encode(manifest).byteLength, blobs: [{ sha512: blobSha, size: 4 }],
  }), env);
  assert.equal(response.status, 200);
  assert.equal(db.publish_sessions.has("sess_broken"), true); // left for an operator, not silently destroyed
});

async function stageFullyUploaded(env: unknown, bucket: MockR2, packId: string, readToken: string) {
  const blobSha = sha("data");
  const { response, manifest } = await beginWith(env, packId, readToken, [{ sha512: blobSha, size: 4 }]);
  assert.equal(response.status, 200);
  const sessionId = (await response.json() as Record<string, string>).sessionId;
  await prepareAt(env, sessionId, 0);
  assert.equal((await (env as any).__fetch(putRequest(`https://relay.test/publish/upload/${sessionId}/${blobSha}`, "data", 4))).status, 200);
  const bytes = new TextEncoder().encode(manifest);
  assert.equal((await (env as any).__fetch(putRequest(`https://relay.test/publish/revision/${sessionId}`, bytes, bytes.length))).status, 200);
  const commit = () => (env as any).__fetch(authorizedRequest("https://relay.test/publish/commit", PUBLISHER_TOKEN, {
    packId, revision: 1, packSecret: PACK_SECRET, sessionId,
  }));
  return { sessionId, blobSha, commit, bucket };
}

function withFetch<T extends { env: any }>(ctx: T): T {
  ctx.env.__fetch = (request: Request) => worker.fetch(request, ctx.env);
  return ctx;
}

test("an abandoned commit is rolled back when no pack marker was written, freeing the pack", async () => {
  const ctx = withFetch(createTestEnv());
  const { env, db, bucket } = ctx;
  await addPublisher(db, PUBLISHER_TOKEN);
  const readToken = "read-token-rollbackabcdefghijklmnopq";
  const staged = await stageFullyUploaded(env, bucket, "Pack_roll_01", readToken);
  bucket.failPuts = 1;
  assert.equal((await staged.commit()).status, 500);
  assert.equal(db.publish_sessions.get(staged.sessionId)!.status, "committing");

  // Not stale yet: the same session is still the only way forward.
  assert.equal((await beginWith(env, "Pack_roll_01", readToken, [{ sha512: staged.blobSha, size: 4 }])).response.status, 409);
  assert.equal(db.publish_sessions.get(staged.sessionId)!.status, "committing");

  db.publish_sessions.get(staged.sessionId)!.status_updated_at = Date.now() - 11 * 60_000;
  const retry = await beginWith(env, "Pack_roll_01", readToken, [{ sha512: staged.blobSha, size: 4 }]);
  assert.equal(retry.response.status, 200);
  assert.equal(db.publish_sessions.has(staged.sessionId), false);
  assert.equal(db.packs.get("Pack_roll_01")!.current_rev, 0);
  assert.equal(bucket.objects.has(buildBlobKey(staged.blobSha, readToken)), false);
  assert.equal(bucket.objects.has(buildRevisionKey(readToken, 1)), false);
});

test("an abandoned commit is rolled forward when the pack marker already names its revision", async () => {
  const ctx = withFetch(createTestEnv());
  const { env, db, bucket } = ctx;
  await addPublisher(db, PUBLISHER_TOKEN);
  const readToken = "read-token-forwardabcdefghijklmnopq";
  const staged = await stageFullyUploaded(env, bucket, "Pack_fwd_01", readToken);
  bucket.failPuts = 1;
  assert.equal((await staged.commit()).status, 500);
  // The marker landed even though the call failed (ambiguous R2 failure): readers already see revision 1.
  await bucket.put(buildLatestKey(readToken), JSON.stringify({ formatVersion: 1, packId: "Pack_fwd_01", revision: 1 }));
  db.publish_sessions.get(staged.sessionId)!.status_updated_at = Date.now() - 11 * 60_000;

  const other = await beginWith(env, "Pack_other_01", "read-token-otherabcdefghijklmnopqrs", [{ sha512: staged.blobSha, size: 4 }]);
  assert.equal(other.response.status, 200);
  assert.equal(db.packs.get("Pack_fwd_01")!.current_rev, 1);
  assert.equal(db.publish_sessions.has(staged.sessionId), false);
  assert.equal(bucket.objects.has(buildBlobKey(staged.blobSha, readToken)), true); // published data is never deleted
  assert.equal(bucket.objects.has(buildRevisionKey(readToken, 1)), true);
});

test("cancel frees the pack after a short grace instead of the full session lifetime", async () => {
  const ctx = withFetch(createTestEnv());
  const { env, db, bucket } = ctx;
  await addPublisher(db, PUBLISHER_TOKEN);
  const readToken = "read-token-abortgraceabcdefghijklmn";
  const blobSha = sha("data");
  const { response } = await beginWith(env, "Pack_abort_01", readToken, [{ sha512: blobSha, size: 4 }]);
  const sessionId = (await response.json() as Record<string, string>).sessionId;
  await prepareAt(env, sessionId, 0);
  assert.equal((await worker.fetch(putRequest(`https://relay.test/publish/upload/${sessionId}/${blobSha}`, "data", 4), env)).status, 200);
  const abort = await worker.fetch(authorizedRequest("https://relay.test/publish/abort", PUBLISHER_TOKEN, { sessionId }), env);
  assert.equal(abort.status, 200);
  assert.ok(db.publish_sessions.get(sessionId)!.expires_at <= Date.now() + 2 * 60_000);

  // During the grace the reservation is held (an upload could still be landing).
  assert.equal((await beginWith(env, "Pack_abort_01", readToken, [{ sha512: blobSha, size: 4 }])).response.status, 409);
  db.publish_sessions.get(sessionId)!.expires_at = Date.now() - 1; // grace elapsed
  assert.equal((await beginWith(env, "Pack_abort_01", readToken, [{ sha512: blobSha, size: 4 }])).response.status, 200);
  assert.equal(bucket.objects.has(buildBlobKey(blobSha, readToken)), false);
});
