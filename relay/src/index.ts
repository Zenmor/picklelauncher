// PickleLauncher Shared Packs Relay - Cloudflare Worker
import {
  isValidPackId,
  isValidRevision,
  isValidSha512,
  isValidToken,
  isValidPrefix,
  buildBlobKey,
  buildBlobPrefix,
  buildRevisionKey,
  buildLatestKey,
} from "./validation.ts";
import {
  sha256Hex,
  generateRandomId,
  generateRandomSecret,
  parsePublisherCode,
  type Publisher,
  type Pack,
} from "./auth.ts";
import { parsePackManifest } from "./manifest.ts";
import { checkUploadQuota, type BlobUploadRequest } from "./quota.ts";

const DEFAULT_PUBLISHER_QUOTA = 3 * 1024 * 1024 * 1024;
const DEFAULT_GLOBAL_QUOTA = 10 * 1024 * 1024 * 1024;
const DEFAULT_MAX_FILE_BYTES = 100_000_000;
const DEFAULT_MAX_PACKS = 5;
const MAX_FILES = 5000;
const BLOB_PAGE_SIZE = 100;
const MAX_MANIFEST_BYTES = 8 * 1024 * 1024;
const SESSION_TTL_MS = 15 * 60 * 1000;
// A re-claimed upload is harmless: R2 verifies the SHA-512, so two writers store identical bytes.
// Keep this short so an interrupted upload can be retried without a long stall.
const UPLOAD_CLAIM_STALE_MS = 60_000;
// A commit session untouched this long is considered abandoned and is recovered on the next begin.
const COMMIT_STALE_MS = 10 * 60_000;
// After Cancel the reservation is kept briefly (an upload may still be in flight), not for the full session TTL.
const ABORT_GRACE_MS = 2 * 60_000;
const MAX_REQUEST_BYTES = 2 * 1024 * 1024;
// A 5,000-file publish makes about 53 counted requests (begin, 50 prepare pages, revision, commit). Blob uploads
// are not counted. The default leaves room for one retry within the same minute.
const DEFAULT_REQUESTS_PER_MINUTE = 120;

export interface Env {
  DB: D1Database;
  BUCKET: R2Bucket;
  DEFAULT_QUOTA_BYTES?: string;
  GLOBAL_STORAGE_LIMIT_BYTES?: string;
  PUBLISHER_REQUESTS_PER_MINUTE?: string;
}

interface PublisherSession {
  session_id: string;
  pack_id: string;
  publisher_id: string;
  revision: number;
  baseline: number;
  reserved_bytes: number;
  blob_sizes_json: string;
  missing_blobs_json: string;
  revision_size: number;
  next_blob_offset: number;
  expires_at: number;
  status: "active" | "aborted" | "committing" | "cleaning";
  status_updated_at: number;
}

function jsonResponse(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, Authorization",
    },
  });
}

function errorResponse(error: string, status = 400): Response {
  return jsonResponse({ error }, status);
}

function positiveInteger(value: string | undefined, fallback: number): number {
  if (value === undefined) return fallback;
  const parsed = Number(value);
  return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : fallback;
}

async function readJson(request: Request): Promise<Record<string, unknown> | null> {
  const declaredLength = request.headers.get("Content-Length");
  if (declaredLength !== null && (!/^\d+$/.test(declaredLength) || Number(declaredLength) > MAX_REQUEST_BYTES)) return null;
  if (!request.body) return null;
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;
      total += chunk.value.byteLength;
      if (total > MAX_REQUEST_BYTES) return null;
      chunks.push(chunk.value);
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    const body: unknown = JSON.parse(text);
    return typeof body === "object" && body !== null && !Array.isArray(body) ? body as Record<string, unknown> : null;
  } catch {
    return null;
  }
}

function changes(result: D1Result): number {
  return result.meta.changes ?? 0;
}

async function authenticatePublisher(request: Request, env: Env, countRequest = true): Promise<Publisher | Response> {
  const authHeader = request.headers.get("Authorization");
  if (!authHeader || !authHeader.startsWith("Bearer ")) {
    return errorResponse("Missing or invalid Authorization header", 401);
  }
  const token = authHeader.slice(7).trim();
  if (!token) return errorResponse("Missing publisher token", 401);

  const tokenHash = await sha256Hex(token);
  const publisher = await env.DB.prepare(
    "SELECT * FROM publishers WHERE token_hash = ? AND revoked_at IS NULL"
  ).bind(tokenHash).first<Publisher>();
  if (!publisher) return errorResponse("Invalid or revoked publisher token", 401);

  if (!countRequest) return publisher;
  const now = Date.now();
  const limit = positiveInteger(env.PUBLISHER_REQUESTS_PER_MINUTE, DEFAULT_REQUESTS_PER_MINUTE);
  const rate = await env.DB.prepare(`
    UPDATE publishers
    SET rate_window_start = CASE WHEN rate_window_start <= ? THEN ? ELSE rate_window_start END,
        request_count = CASE WHEN rate_window_start <= ? THEN 1 ELSE request_count + 1 END
    WHERE id = ? AND revoked_at IS NULL
      AND (rate_window_start <= ? OR request_count < ?)
  `).bind(now - 60_000, now, now - 60_000, publisher.id, now - 60_000, limit).run();
  if (changes(rate) !== 1) return errorResponse("Too many publish requests from this publisher; wait a minute and try again", 429);
  return publisher;
}

function packPath(pack: Pack): { storageToken: string; prefix: string } {
  return { storageToken: pack.storage_token, prefix: pack.prefix };
}

async function discardEmptyPack(env: Env, packId: string, publisherId: string): Promise<void> {
  await env.DB.prepare(`
    DELETE FROM packs
    WHERE pack_id = ? AND publisher_id = ? AND current_rev = 0 AND stored_bytes = 0
      AND NOT EXISTS (SELECT 1 FROM publish_sessions WHERE pack_id = ?)
  `).bind(packId, publisherId, packId).run();
}

async function getManifest(
  env: Env,
  key: string,
  packId: string,
  revision: number,
): Promise<{ manifest: NonNullable<ReturnType<typeof parsePackManifest>>["manifest"]; requiredBlobs: Map<string, number>; size: number } | null> {
  const head = await env.BUCKET.head(key);
  if (!head || head.size > MAX_MANIFEST_BYTES) return null;
  const object = await env.BUCKET.get(key);
  if (!object) return null;
  const bytes = new Uint8Array(await object.arrayBuffer());
  const parsed = parsePackManifest(bytes, packId, revision);
  return parsed ? { manifest: parsed.manifest, requiredBlobs: parsed.requiredBlobs, size: bytes.byteLength } : null;
}

async function retainedStorage(
  env: Env,
  pack: Pack,
  revision: number,
  current: { requiredBlobs: Map<string, number>; size: number },
): Promise<{ retainedBlobSizes: Map<string, number>; retainedBytes: number }> {
  const retainedBlobSizes = new Map(current.requiredBlobs);
  let retainedManifestBytes = current.size;
  for (const priorRevision of [revision - 1, revision - 2]) {
    if (priorRevision < 1) continue;
    const prior = await getManifest(env, buildRevisionKey(pack.storage_token, priorRevision, pack.prefix), pack.pack_id, priorRevision);
    if (!prior) continue;
    retainedManifestBytes += prior.size;
    for (const [hash, size] of prior.requiredBlobs) retainedBlobSizes.set(hash, size);
  }
  const retainedBytes = [...retainedBlobSizes.values()].reduce((total, size) => total + size, retainedManifestBytes);
  return { retainedBlobSizes, retainedBytes };
}

// True when every required blob exists in R2 with exactly its expected size. Uses paged list() calls (1,000 keys
// each) instead of one head() per blob: a pack holds thousands of blobs and one Worker invocation may make only a
// limited number of subrequests (1,000 on the Free plan), so a per-blob loop would fail on large packs.
async function blobsAreStored(env: Env, pack: Pack, required: Map<string, number>): Promise<boolean> {
  const prefix = buildBlobPrefix(pack.storage_token, pack.prefix);
  const remaining = new Map(required);
  let cursor: string | undefined;
  do {
    const page = await env.BUCKET.list({ prefix, cursor, limit: 1000 });
    for (const object of page.objects) {
      const hash = object.key.slice(prefix.length);
      if (remaining.get(hash) === object.size) remaining.delete(hash);
    }
    cursor = page.truncated && remaining.size > 0 ? page.cursor : undefined;
  } while (cursor !== undefined);
  return remaining.size === 0;
}

async function markerRevision(env: Env, pack: Pack): Promise<number | null> {
  const latest = await env.BUCKET.get(buildLatestKey(pack.storage_token, pack.prefix));
  if (!latest) return null;
  try {
    const marker = JSON.parse(await latest.text()) as Record<string, unknown>;
    return marker.packId === pack.pack_id && typeof marker.revision === "number" && Number.isSafeInteger(marker.revision)
      ? marker.revision : null;
  } catch {
    return null;
  }
}

// A commit that claimed its session but never finished (crash, timeout, lost client) would otherwise block the
// pack forever. If the pack marker already names the revision the publish is complete for readers, so finish the
// registry update (roll forward); otherwise nothing was published, so discard the session (roll back).
async function recoverStaleCommits(env: Env, now: number): Promise<void> {
  const stale = await env.DB.prepare(`
    SELECT * FROM publish_sessions
    WHERE status = 'committing' AND status_updated_at <= ?
    ORDER BY status_updated_at
    LIMIT 10
  `).bind(now - COMMIT_STALE_MS).all<PublisherSession>();
  for (const session of stale.results) {
    try {
      const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(session.pack_id).first<Pack>();
      if (!pack) continue;
      if ((await markerRevision(env, pack)) === session.revision) {
        const manifest = await getManifest(env, buildRevisionKey(pack.storage_token, session.revision, pack.prefix), pack.pack_id, session.revision);
        if (!manifest) continue;
        await finishSession(env, pack, session, (await retainedStorage(env, pack, session.revision, manifest)).retainedBytes);
        continue;
      }
      const claimed = await env.DB.prepare(`
        UPDATE publish_sessions SET status = 'cleaning', status_updated_at = ?
        WHERE session_id = ? AND status = 'committing' AND status_updated_at <= ?
      `).bind(now, session.session_id, now - COMMIT_STALE_MS).run();
      if (changes(claimed) !== 1) continue;
      const parsed: unknown = JSON.parse(session.missing_blobs_json);
      if (!Array.isArray(parsed)) continue;
      const keys = [buildRevisionKey(pack.storage_token, session.revision, pack.prefix)];
      for (const item of parsed) {
        const blob = item as Record<string, unknown>;
        if (typeof blob?.sha512 !== "string" || !isValidSha512(blob.sha512)) throw new Error("invalid blob entry");
        keys.push(buildBlobKey(blob.sha512, pack.storage_token, pack.prefix));
      }
      for (let i = 0; i < keys.length; i += 1000) await env.BUCKET.delete(keys.slice(i, i + 1000));
      await env.DB.batch([
        env.DB.prepare("DELETE FROM publish_sessions WHERE session_id = ? AND status = 'cleaning'").bind(session.session_id),
        env.DB.prepare(`
          DELETE FROM packs
          WHERE pack_id = ? AND current_rev = 0 AND stored_bytes = 0
            AND NOT EXISTS (SELECT 1 FROM publish_sessions WHERE pack_id = ?)
        `).bind(pack.pack_id, pack.pack_id),
      ]);
    } catch {
      continue; // damaged row or storage error: leave it for the next attempt or an operator
    }
  }
}

async function cleanupExpiredSessions(env: Env, now: number): Promise<void> {
  // Rows another cleanup claimed in the last minute are skipped so damaged rows cannot fill all 10 slots.
  const result = await env.DB.prepare(`
    SELECT * FROM publish_sessions
    WHERE expires_at <= ? AND status IN ('active', 'aborted', 'cleaning')
      AND NOT (status = 'cleaning' AND status_updated_at > ?)
    ORDER BY expires_at
    LIMIT 10
  `).bind(now, now - 60_000).all<PublisherSession>();

  for (const session of result.results) {
    const claimed = await env.DB.prepare(`
      UPDATE publish_sessions SET status = 'cleaning', status_updated_at = ?
      WHERE session_id = ? AND expires_at <= ?
        AND (status IN ('active', 'aborted') OR (status = 'cleaning' AND status_updated_at <= ?))
    `).bind(now, session.session_id, now, now - 60_000).run();
    if (changes(claimed) !== 1) continue;

    const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?")
      .bind(session.pack_id).first<Pack>();
    // A damaged row is left for an operator; it must never fail other publishers' begin requests.
    if (!pack) continue;
    let missingBlobs: BlobUploadRequest[];
    try {
      const parsed: unknown = JSON.parse(session.missing_blobs_json);
      if (!Array.isArray(parsed)) throw new Error("invalid blob list");
      missingBlobs = parsed.map((item): BlobUploadRequest => {
        if (typeof item !== "object" || item === null) throw new Error("invalid blob entry");
        const blob = item as Record<string, unknown>;
        if (typeof blob.sha512 !== "string" || !isValidSha512(blob.sha512) ||
            typeof blob.size !== "number" || !Number.isSafeInteger(blob.size) || blob.size < 0) {
          throw new Error("invalid blob entry");
        }
        return { sha512: blob.sha512, size: blob.size };
      });
    } catch {
      continue;
    }
    const keys = [
      buildRevisionKey(pack.storage_token, session.revision, pack.prefix),
      ...missingBlobs.map((blob) => buildBlobKey(blob.sha512, pack.storage_token, pack.prefix)),
    ];
    for (let i = 0; i < keys.length; i += 1000) {
      await env.BUCKET.delete(keys.slice(i, i + 1000));
    }

    await env.DB.batch([
      env.DB.prepare("DELETE FROM publish_sessions WHERE session_id = ? AND status = 'cleaning'")
        .bind(session.session_id),
      env.DB.prepare(`
        DELETE FROM packs
        WHERE pack_id = ? AND current_rev = 0 AND stored_bytes = 0
          AND NOT EXISTS (SELECT 1 FROM publish_sessions WHERE pack_id = ?)
      `).bind(pack.pack_id, pack.pack_id),
    ]);
  }
}

async function redeemPublisher(request: Request, env: Env): Promise<Response> {
  const body = await readJson(request);
  if (!body || typeof body.code !== "string" || body.code.length > 16 * 1024) {
    return errorResponse("Missing or invalid publisher code");
  }

  let rawCode = body.code.trim();
  const parsedCode = parsePublisherCode(rawCode);
  if (parsedCode) rawCode = parsedCode.code;
  if (rawCode.length < 16 || rawCode.length > 512) return errorResponse("Invalid or expired publisher code", 400);

  const codeHash = await sha256Hex(rawCode);
  const now = Date.now();
  const publisherId = generateRandomId("pub");
  const publisherToken = generateRandomSecret(32);
  const tokenHash = await sha256Hex(publisherToken);
  const defaultQuota = positiveInteger(env.DEFAULT_QUOTA_BYTES, DEFAULT_PUBLISHER_QUOTA);

  const result = await env.DB.batch([
    env.DB.prepare(`
      INSERT INTO publishers
        (id, label, token_hash, quota_bytes, max_file_bytes, max_packs, created_at, rate_window_start, request_count)
      SELECT ?, label, ?, ?, ?, ?, ?, 0, 0
      FROM publisher_codes
      WHERE code_hash = ? AND used_at IS NULL AND expires_at >= ?
    `).bind(publisherId, tokenHash, defaultQuota, DEFAULT_MAX_FILE_BYTES, DEFAULT_MAX_PACKS, now, codeHash, now),
    env.DB.prepare(`
      UPDATE publisher_codes SET used_at = ?, publisher_id = ?
      WHERE code_hash = ? AND used_at IS NULL AND expires_at >= ?
        AND EXISTS (SELECT 1 FROM publishers WHERE id = ?)
    `).bind(now, publisherId, codeHash, now, publisherId),
  ]);

  if (changes(result[0]) !== 1 || changes(result[1]) !== 1) {
    return errorResponse("Invalid or expired publisher code", 400);
  }
  const publisher = await env.DB.prepare("SELECT label FROM publishers WHERE id = ?")
    .bind(publisherId).first<{ label: string }>();
  if (!publisher) return errorResponse("Publisher registration could not be completed", 500);

  return jsonResponse({
    publisherToken,
    publisherId,
    label: publisher.label,
    quotaBytes: defaultQuota,
    maxFileBytes: DEFAULT_MAX_FILE_BYTES,
    maxPacks: DEFAULT_MAX_PACKS,
  });
}

function parseBlobRequests(value: unknown, publisher: Publisher): BlobUploadRequest[] | null {
  if (!Array.isArray(value) || value.length > MAX_FILES) return null;
  const unique = new Map<string, number>();
  for (const item of value) {
    if (typeof item !== "object" || item === null || Array.isArray(item)) return null;
    const blob = item as Record<string, unknown>;
    if (
      typeof blob.sha512 !== "string" ||
      !isValidSha512(blob.sha512) ||
      typeof blob.size !== "number" ||
      !Number.isSafeInteger(blob.size) ||
      blob.size < 0 ||
      blob.size > publisher.max_file_bytes ||
      blob.size > DEFAULT_MAX_FILE_BYTES
    ) return null;
    const previous = unique.get(blob.sha512);
    if (previous !== undefined && previous !== blob.size) return null;
    unique.set(blob.sha512, blob.size);
  }
  return [...unique].map(([sha512, size]) => ({ sha512, size }));
}

async function checkBlobs(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const body = await readJson(request);
  if (!body || typeof body.readToken !== "string" || !isValidToken(body.readToken)) return errorResponse("Invalid read token");
  const readToken = body.readToken;
  const prefix = body.prefix === undefined ? "" : body.prefix;
  if (typeof prefix !== "string" || !isValidPrefix(prefix)) return errorResponse("Invalid prefix");
  const blobs = parseBlobRequests(body.blobs, publisherResult);
  if (!blobs || blobs.length > BLOB_PAGE_SIZE) return errorResponse("Invalid blob batch");

  const checks = await Promise.all(blobs.map(async (blob) => {
    const object = await env.BUCKET.head(buildBlobKey(blob.sha512, readToken, prefix));
    return object?.size === blob.size ? blob.sha512 : null;
  }));
  const presentBlobs = checks.filter((sha512): sha512 is string => sha512 !== null);
  return jsonResponse({ presentBlobs });
}

async function beginPublish(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const publisher = publisherResult;
  const body = await readJson(request);
  if (!body) return errorResponse("Invalid JSON payload");

  const packId = body.packId;
  const revision = body.revision;
  const packSecret = body.packSecret;
  const readToken = body.readToken;
  const prefix = body.prefix === undefined ? "" : body.prefix;
  const revisionSize = body.manifestBytes;
  if (!isValidPackId(packId)) return errorResponse("Invalid packId");
  if (!isValidRevision(revision)) return errorResponse("Invalid revision number");
  if (typeof packSecret !== "string" || packSecret.length < 16 || packSecret.length > 256) return errorResponse("Invalid pack secret");
  if (!isValidToken(readToken)) return errorResponse("Invalid read token");
  if (typeof prefix !== "string" || !isValidPrefix(prefix)) return errorResponse("Invalid prefix");
  if (typeof revisionSize !== "number" || !Number.isSafeInteger(revisionSize) || revisionSize < 1 || revisionSize > MAX_MANIFEST_BYTES) {
    return errorResponse("Invalid revision manifest size");
  }
  const blobRequests = parseBlobRequests(body.blobs, publisher);
  if (!blobRequests) return errorResponse("Invalid blob list");

  const packSecretHash = await sha256Hex(packSecret);
  const now = Date.now();
  try {
    await cleanupExpiredSessions(env, now);
    await recoverStaleCommits(env, now);
  } catch {
    // Cleanup of someone else's expired session failed (for example an R2 error); it is retried on the next begin.
  }

  const newPackResult = await env.DB.prepare(`
    INSERT OR IGNORE INTO packs
      (pack_id, publisher_id, pack_secret_hash, read_token, previous_read_token, storage_token, prefix, current_rev, stored_bytes, created_at)
    SELECT ?, ?, ?, ?, NULL, ?, ?, 0, 0, ?
    WHERE (SELECT COUNT(*) FROM packs WHERE publisher_id = ?) < ?
  `).bind(packId, publisher.id, packSecretHash, readToken, readToken, prefix, now, publisher.id, publisher.max_packs).run();
  const createdPack = changes(newPackResult) === 1;

  const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(packId).first<Pack>();
  if (!pack) return errorResponse("Publisher pack limit reached", 403);
  if (pack.publisher_id !== publisher.id) return errorResponse("Pack belongs to another publisher", 403);
  if (pack.pack_secret_hash !== packSecretHash) return errorResponse("Invalid pack secret", 403);
  const path = packPath(pack);
  if (pack.read_token !== readToken || path.prefix !== prefix) return errorResponse("Pack read address cannot be changed", 409);
  if (revision !== pack.current_rev + 1) {
    if (createdPack) await discardEmptyPack(env, packId, publisher.id);
    return jsonResponse({ error: "Revision out of sequence", currentRev: pack.current_rev }, 409);
  }

  const reserveBytes = blobRequests.reduce((total, blob) => total + blob.size, revisionSize);
  const quotaCheck = checkUploadQuota(publisher.quota_bytes, publisher.max_file_bytes, pack.stored_bytes, [
    ...blobRequests,
    { sha512: "revision", size: revisionSize },
  ]);
  if (!quotaCheck.ok) {
    if (createdPack) await discardEmptyPack(env, packId, publisher.id);
    return errorResponse(quotaCheck.error || "Quota exceeded", 403);
  }

  const sessionId = generateRandomSecret(24);
  const expiresAt = now + SESSION_TTL_MS;
  const globalLimit = positiveInteger(env.GLOBAL_STORAGE_LIMIT_BYTES, DEFAULT_GLOBAL_QUOTA);
  const inserted = await env.DB.prepare(`
    INSERT INTO publish_sessions
      (session_id, pack_id, publisher_id, revision, baseline, reserved_bytes, blob_sizes_json, missing_blobs_json, revision_size, expires_at,
       next_blob_offset, status, status_updated_at)
    SELECT ?, p.pack_id, p.publisher_id, ?, p.current_rev, ?, ?, '[]', ?, ?, 0, 'active', 0
    FROM packs p JOIN publishers u ON u.id = p.publisher_id
    WHERE p.pack_id = ? AND p.publisher_id = ? AND p.pack_secret_hash = ?
      AND p.read_token = ? AND p.prefix = ? AND p.current_rev = ?
      AND u.revoked_at IS NULL
      AND NOT EXISTS (SELECT 1 FROM publish_sessions s WHERE s.pack_id = p.pack_id)
      AND (SELECT COALESCE(SUM(stored_bytes), 0) FROM packs WHERE publisher_id = p.publisher_id)
          + (SELECT COALESCE(SUM(reserved_bytes), 0) FROM publish_sessions WHERE publisher_id = p.publisher_id)
          + ? <= u.quota_bytes
      AND (SELECT COALESCE(SUM(stored_bytes), 0) FROM packs)
          + (SELECT COALESCE(SUM(reserved_bytes), 0) FROM publish_sessions)
          + ? <= ?
  `).bind(
    sessionId, revision, reserveBytes, JSON.stringify(blobRequests), revisionSize, expiresAt,
    packId, publisher.id, packSecretHash, readToken, prefix, revision - 1, reserveBytes, reserveBytes, globalLimit,
  ).run();
  if (changes(inserted) !== 1) {
    if (createdPack) await discardEmptyPack(env, packId, publisher.id);
    return errorResponse("Another publish is active, the revision changed, or a storage quota would be exceeded", 409);
  }

  return jsonResponse({ sessionId, blobCount: blobRequests.length, expiresAt });
}

function parseSessionBlobs(session: PublisherSession): BlobUploadRequest[] | null {
  try {
    const value: unknown = JSON.parse(session.blob_sizes_json);
    if (!Array.isArray(value) || value.length > MAX_FILES) return null;
    const blobs: BlobUploadRequest[] = [];
    for (const item of value) {
      if (typeof item !== "object" || item === null || Array.isArray(item)) return null;
      const blob = item as Record<string, unknown>;
      if (typeof blob.sha512 !== "string" || !isValidSha512(blob.sha512) ||
          typeof blob.size !== "number" || !Number.isSafeInteger(blob.size) ||
          blob.size < 0 || blob.size > DEFAULT_MAX_FILE_BYTES) return null;
      blobs.push({ sha512: blob.sha512, size: blob.size });
    }
    return blobs;
  } catch {
    return null;
  }
}

async function prepareBlobPage(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const body = await readJson(request);
  if (!body || typeof body.sessionId !== "string" || !Number.isSafeInteger(body.offset)) {
    return errorResponse("Invalid upload preparation request");
  }
  const session = await env.DB.prepare(
    "SELECT * FROM publish_sessions WHERE session_id = ? AND publisher_id = ?"
  ).bind(body.sessionId, publisherResult.id).first<PublisherSession>();
  if (!session || session.status !== "active" || session.expires_at < Date.now()) {
    return errorResponse("Publish session expired or missing", 409);
  }
  const blobs = parseSessionBlobs(session);
  if (!blobs) return errorResponse("Publish session is damaged", 500);
  const offset = body.offset as number;
  if (offset < 0 || offset > blobs.length) return errorResponse("Upload preparation is out of sequence", 409);
  if (offset !== session.next_blob_offset) {
    // The client may be retrying the page that was just recorded because the response was lost.
    // Answer it from the stored state; any other page is genuinely out of sequence.
    const replayEnd = Math.min(offset + BLOB_PAGE_SIZE, blobs.length);
    if (offset % BLOB_PAGE_SIZE !== 0 || offset >= replayEnd || replayEnd !== session.next_blob_offset) {
      return errorResponse("Upload preparation is out of sequence", 409);
    }
    let recorded: BlobUploadRequest[];
    try {
      const parsed: unknown = JSON.parse(session.missing_blobs_json);
      if (!Array.isArray(parsed)) throw new Error();
      recorded = parsed as BlobUploadRequest[];
    } catch {
      return errorResponse("Publish session is damaged", 500);
    }
    const pageHashes = new Set(blobs.slice(offset, replayEnd).map((blob) => blob.sha512));
    return jsonResponse({
      missingBlobs: recorded.filter((blob) => pageHashes.has(blob.sha512)).map((blob) => blob.sha512),
      nextOffset: replayEnd,
      complete: replayEnd === blobs.length,
    });
  }
  if (offset === blobs.length) return jsonResponse({ missingBlobs: [], nextOffset: offset, complete: true });

  const page = blobs.slice(offset, offset + BLOB_PAGE_SIZE);
  const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(session.pack_id).first<Pack>();
  if (!pack || pack.publisher_id !== publisherResult.id) return errorResponse("Pack not found", 404);
  const path = packPath(pack);
  const checks = await Promise.all(page.map(async (blob) => {
    const key = buildBlobKey(blob.sha512, path.storageToken, path.prefix);
    const object = await env.BUCKET.head(key);
    if (object && object.size !== blob.size) await env.BUCKET.delete(key);
    return object?.size === blob.size;
  }));
  const newlyMissing = page.filter((_, index) => !checks[index]);
  const alreadyStoredBytes = page.reduce((total, blob, index) => total + (checks[index] ? blob.size : 0), 0);
  let previousMissing: BlobUploadRequest[];
  try {
    const parsed: unknown = JSON.parse(session.missing_blobs_json);
    if (!Array.isArray(parsed)) throw new Error();
    previousMissing = parsed as BlobUploadRequest[];
  } catch {
    return errorResponse("Publish session is damaged", 500);
  }
  const nextMissing = [...previousMissing, ...newlyMissing];
  const nextOffset = offset + page.length;
  const statements = [env.DB.prepare(`
    UPDATE publish_sessions
    SET next_blob_offset = ?, reserved_bytes = reserved_bytes - ?, missing_blobs_json = ?
    WHERE session_id = ? AND publisher_id = ? AND status = 'active' AND next_blob_offset = ? AND reserved_bytes >= ?
  `).bind(nextOffset, alreadyStoredBytes, JSON.stringify(nextMissing), session.session_id, publisherResult.id, offset, alreadyStoredBytes)];
  for (const blob of newlyMissing) {
    statements.push(env.DB.prepare(`
      INSERT INTO publish_session_blobs (session_id, sha512, size_bytes, status, status_updated_at)
      SELECT ?, ?, ?, 'pending', 0
      WHERE EXISTS (SELECT 1 FROM publish_sessions WHERE session_id = ? AND status = 'active' AND next_blob_offset = ?)
    `).bind(session.session_id, blob.sha512, blob.size, session.session_id, nextOffset));
  }
  const updated = await env.DB.batch(statements);
  if (changes(updated[0]) !== 1) return errorResponse("Upload preparation could not be recorded", 409);
  return jsonResponse({
    missingBlobs: newlyMissing.map((blob) => blob.sha512),
    nextOffset,
    complete: nextOffset === blobs.length,
  });
}

async function uploadBlob(request: Request, env: Env, sessionId: string, sha512: string): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env, false);
  if (publisherResult instanceof Response) return publisherResult;
  if (!isValidToken(sessionId) || !isValidSha512(sha512) || !request.body) return errorResponse("Invalid blob upload");
  // One narrow query instead of loading and parsing the session's two JSON columns (about 800 KB each at 5,000
  // files) on every upload. publish_session_blobs holds exactly the blobs this session still has to receive, with
  // their declared sizes, so a missing row means the blob is not expected.
  const row = await env.DB.prepare(`
    SELECT s.status, s.expires_at, s.next_blob_offset, json_array_length(s.blob_sizes_json) AS blob_count,
           p.storage_token, p.prefix, b.size_bytes, b.status AS blob_status
    FROM publish_sessions s
    JOIN packs p ON p.pack_id = s.pack_id AND p.publisher_id = s.publisher_id
    LEFT JOIN publish_session_blobs b ON b.session_id = s.session_id AND b.sha512 = ?
    WHERE s.session_id = ? AND s.publisher_id = ?
  `).bind(sha512, sessionId, publisherResult.id).first<{
    status: string; expires_at: number; next_blob_offset: number; blob_count: number;
    storage_token: string; prefix: string; size_bytes: number | null; blob_status: string | null;
  }>();
  if (!row || row.status !== "active" || row.expires_at < Date.now()) {
    return errorResponse("Publish session expired or missing", 409);
  }
  if (row.next_blob_offset !== row.blob_count) return errorResponse("Upload preparation is incomplete", 409);
  if (row.size_bytes === null) return errorResponse("Blob is not expected by this session", 403);
  const blobSize = row.size_bytes;
  const contentLength = request.headers.get("Content-Length");
  if (contentLength === null || !/^\d+$/.test(contentLength) || Number(contentLength) !== blobSize) {
    return errorResponse("Uploaded file size did not match the manifest", 400);
  }
  if (row.blob_status === "uploaded") return jsonResponse({ ok: true, sha512 });
  const now = Date.now();
  const claimed = await env.DB.prepare(`
    UPDATE publish_session_blobs SET status = 'uploading', status_updated_at = ?
    WHERE session_id = ? AND sha512 = ? AND
      (status = 'pending' OR (status = 'uploading' AND status_updated_at <= ?))
  `).bind(now, sessionId, sha512, now - UPLOAD_CLAIM_STALE_MS).run();
  if (changes(claimed) !== 1) return errorResponse("This blob upload is already in progress", 409);
  const blobKey = buildBlobKey(sha512, row.storage_token, row.prefix);

  let received = 0;
  let sizeViolation = false;
  const checkedBody = request.body.pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) {
      received += chunk.byteLength;
      if (received > blobSize || received > publisherResult.max_file_bytes) {
        sizeViolation = true;
        throw new Error("Uploaded file exceeded its declared size");
      }
      controller.enqueue(chunk);
    },
    flush() {
      if (received !== blobSize) {
        sizeViolation = true;
        throw new Error("Uploaded file did not match its declared size");
      }
    },
  }));
  // R2 needs a stream of known length; a plain pipeThrough() result has none. FixedLengthStream is the
  // Workers runtime's documented way to provide it (it is absent under Node, where the test mock reads the stream).
  const FixedLength = (globalThis as { FixedLengthStream?: new (length: number) => TransformStream<Uint8Array, Uint8Array> })
    .FixedLengthStream;
  let putBody: ReadableStream<Uint8Array> = checkedBody;
  let piping: Promise<void> | null = null;
  if (FixedLength) {
    const fixed = new FixedLength(blobSize);
    piping = checkedBody.pipeTo(fixed.writable);
    piping.catch(() => {});
    putBody = fixed.readable;
  }
  try {
    await env.BUCKET.put(blobKey, putBody, { sha512 });
    if (piping) await piping;
  } catch (error) {
    await env.DB.prepare(`
      UPDATE publish_session_blobs SET status = 'pending', status_updated_at = 0
      WHERE session_id = ? AND sha512 = ? AND status = 'uploading'
    `).bind(sessionId, sha512).run();
    const checksumRejected = error instanceof Error && /sha-?\d+|checksum|\(10037\)/i.test(error.message);
    if (sizeViolation || checksumRejected) {
      return errorResponse("Uploaded file did not match its declared size or SHA-512", 400);
    }
    // Anything else (R2 outage, client disconnect) is not the file's fault: report a retryable failure.
    return errorResponse("The relay could not store the uploaded file; try again", 502);
  }
  const recorded = await env.DB.prepare(`
    UPDATE publish_session_blobs SET status = 'uploaded', status_updated_at = ?
    WHERE session_id = ? AND sha512 = ? AND status = 'uploading'
  `).bind(Date.now(), sessionId, sha512).run();
  if (changes(recorded) !== 1) {
    // A retry may have re-claimed a stalled upload and already recorded the same verified bytes.
    const row = await env.DB.prepare(`
      SELECT status, status_updated_at FROM publish_session_blobs WHERE session_id = ? AND sha512 = ?
    `).bind(sessionId, sha512).first<{ status: string; status_updated_at: number }>();
    if (row?.status !== "uploaded") return errorResponse("Uploaded file could not be recorded", 500);
  }
  return jsonResponse({ ok: true, sha512 });
}

async function uploadRevision(request: Request, env: Env, sessionId: string): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  if (!isValidToken(sessionId)) return errorResponse("Invalid publish session");
  const session = await env.DB.prepare(
    "SELECT * FROM publish_sessions WHERE session_id = ? AND publisher_id = ?"
  ).bind(sessionId, publisherResult.id).first<PublisherSession>();
  if (!session || session.status !== "active" || session.expires_at < Date.now()) {
    return errorResponse("Publish session expired or missing", 409);
  }
  const blobs = parseSessionBlobs(session);
  if (!blobs || session.next_blob_offset !== blobs.length) return errorResponse("Upload preparation is incomplete", 409);
  const length = request.headers.get("Content-Length");
  if (length === null || !/^\d+$/.test(length) || Number(length) !== session.revision_size) {
    return errorResponse("Revision size did not match the manifest", 400);
  }
  const reader = request.body?.getReader();
  if (!reader) return errorResponse("Revision body is missing", 400);
  const chunks: Uint8Array[] = [];
  let total = 0;
  try {
    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;
      total += chunk.value.byteLength;
      if (total > session.revision_size || total > MAX_MANIFEST_BYTES) {
        await reader.cancel();
        return errorResponse("Revision size did not match the manifest", 400);
      }
      chunks.push(chunk.value);
    }
  } catch {
    return errorResponse("Revision upload was interrupted", 400);
  }
  if (total !== session.revision_size) return errorResponse("Revision size did not match the manifest", 400);
  const bytes = new Uint8Array(total);
  let byteOffset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, byteOffset);
    byteOffset += chunk.byteLength;
  }
  const parsed = parsePackManifest(bytes, session.pack_id, session.revision);
  if (!parsed || parsed.requiredBlobs.size !== blobs.length ||
      blobs.some((blob) => parsed.requiredBlobs.get(blob.sha512) !== blob.size)) {
    return errorResponse("Revision manifest does not match the prepared files", 400);
  }
  const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(session.pack_id).first<Pack>();
  if (!pack || pack.publisher_id !== publisherResult.id) return errorResponse("Pack not found", 404);
  await env.BUCKET.put(buildRevisionKey(pack.storage_token, session.revision, pack.prefix), bytes, {
    httpMetadata: { contentType: "application/json" },
  });
  return jsonResponse({ ok: true, revision: session.revision });
}

async function finishSession(env: Env, pack: Pack, session: PublisherSession, storedBytes: number): Promise<boolean> {
  const result = await env.DB.batch([
    env.DB.prepare(`
      UPDATE packs SET current_rev = ?, stored_bytes = ?
      WHERE pack_id = ? AND publisher_id = ? AND current_rev = ?
        AND EXISTS (SELECT 1 FROM publish_sessions WHERE session_id = ? AND pack_id = ? AND status = 'committing')
    `).bind(session.revision, storedBytes, pack.pack_id, pack.publisher_id, session.baseline, session.session_id, pack.pack_id),
    env.DB.prepare(`
      DELETE FROM publish_sessions
      WHERE session_id = ? AND pack_id = ? AND status = 'committing'
        AND EXISTS (SELECT 1 FROM packs WHERE pack_id = ? AND current_rev = ?)
    `).bind(session.session_id, pack.pack_id, pack.pack_id, session.revision),
  ]);
  // D1 counts rows removed by ON DELETE CASCADE (the session's blob rows) in meta.changes, so the delete reports
  // 1 + number of blobs. It targets the primary key, so "at least one" means the session row itself was deleted.
  return changes(result[0]) === 1 && changes(result[1]) >= 1;
}

async function commitPublish(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const publisher = publisherResult;
  const body = await readJson(request);
  if (!body) return errorResponse("Invalid JSON payload");

  const { packId, revision, packSecret, sessionId } = body;
  if (!isValidPackId(packId) || !isValidRevision(revision)) return errorResponse("Invalid pack or revision");
  if (typeof packSecret !== "string" || typeof sessionId !== "string") return errorResponse("Missing publish session");

  const packSecretHash = await sha256Hex(packSecret);
  const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(packId).first<Pack>();
  if (!pack) return errorResponse("Pack not found", 404);
  if (pack.publisher_id !== publisher.id) return errorResponse("Forbidden", 403);
  if (pack.pack_secret_hash !== packSecretHash) return errorResponse("Invalid pack secret", 403);

  const latestKey = buildLatestKey(pack.storage_token, pack.prefix);
  if (pack.current_rev === revision) {
    const latest = await env.BUCKET.get(latestKey);
    if (latest) {
      try {
        const marker = JSON.parse(await latest.text()) as Record<string, unknown>;
        if (marker.packId === packId && marker.revision === revision) return jsonResponse({ ok: true, revision });
      } catch {
        return errorResponse("The pack marker is damaged", 500);
      }
    }
  }

  const session = await env.DB.prepare(
    "SELECT * FROM publish_sessions WHERE session_id = ? AND pack_id = ? AND publisher_id = ?"
  ).bind(sessionId, packId, publisher.id).first<PublisherSession>();
  if (!session || (session.status !== "committing" && session.expires_at < Date.now())) {
    return errorResponse("Publish session expired or missing", 409);
  }
  if (session.status === "aborted" || session.status === "cleaning") return errorResponse("Publish session was cancelled", 409);
  if (session.revision !== revision || pack.current_rev !== session.baseline) return errorResponse("Revision changed during publish", 409);
  const sessionBlobs = parseSessionBlobs(session);
  if (!sessionBlobs || session.next_blob_offset !== sessionBlobs.length) {
    return errorResponse("Upload preparation is incomplete", 409);
  }
  const incompleteUploads = await env.DB.prepare(`
    SELECT COUNT(*) AS count FROM publish_session_blobs
    WHERE session_id = ? AND status != 'uploaded'
  `).bind(session.session_id).first<{ count: number }>();
  if ((incompleteUploads?.count ?? 0) !== 0) return errorResponse("One or more pack files are not uploaded", 409);

  const path = packPath(pack);
  const revisionKey = buildRevisionKey(path.storageToken, revision, path.prefix);
  const storedManifest = await getManifest(env, revisionKey, packId, revision);
  if (!storedManifest || storedManifest.size !== session.revision_size) {
    return errorResponse("The uploaded revision is missing, damaged, or has the wrong size", 400);
  }
  if (storedManifest.manifest.files.some((file) => file.size > publisher.max_file_bytes)) {
    return errorResponse("The revision contains a file larger than the publisher limit", 400);
  }

  let declaredBlobs: Map<string, number>;
  try {
    const parsed = JSON.parse(session.blob_sizes_json) as unknown;
    if (!Array.isArray(parsed)) return errorResponse("Publish session is damaged", 500);
    declaredBlobs = new Map<string, number>();
    for (const item of parsed) {
      if (typeof item !== "object" || item === null) return errorResponse("Publish session is damaged", 500);
      const blob = item as Record<string, unknown>;
      if (typeof blob.sha512 !== "string" || !isValidSha512(blob.sha512) ||
          typeof blob.size !== "number" || !Number.isSafeInteger(blob.size)) {
        return errorResponse("Publish session is damaged", 500);
      }
      declaredBlobs.set(blob.sha512, blob.size);
    }
  } catch {
    return errorResponse("Publish session is damaged", 500);
  }
  if (declaredBlobs.size !== storedManifest.requiredBlobs.size) {
    return errorResponse("The uploaded revision does not match the publish request", 400);
  }

  for (const [sha512, size] of storedManifest.requiredBlobs) {
    if (declaredBlobs.get(sha512) !== size) return errorResponse("The uploaded revision does not match the publish request", 400);
  }
  if ([...declaredBlobs.keys()].some((hash) => !storedManifest.requiredBlobs.has(hash))) {
    return errorResponse("The uploaded revision does not match the publish request", 400);
  }
  if (!(await blobsAreStored(env, pack, storedManifest.requiredBlobs))) {
    return errorResponse("A required pack file is missing or has the wrong size", 400);
  }

  const { retainedBlobSizes, retainedBytes } = await retainedStorage(env, pack, revision, storedManifest);

  const latestPayload = JSON.stringify({ formatVersion: 1, packId, revision });
  if (session.status === "active") {
    const claimed = await env.DB.prepare(`
      UPDATE publish_sessions SET status = 'committing', status_updated_at = ?
      WHERE session_id = ? AND pack_id = ? AND status = 'active' AND expires_at >= ?
    `).bind(Date.now(), session.session_id, packId, Date.now()).run();
    if (changes(claimed) !== 1) return errorResponse("Publish session expired or was cancelled", 409);
  }
  await env.BUCKET.put(latestKey, latestPayload, { httpMetadata: { contentType: "application/json" } });

  if (!(await finishSession(env, pack, session, retainedBytes))) {
    const current = await env.DB.prepare("SELECT current_rev FROM packs WHERE pack_id = ?").bind(packId).first<{ current_rev: number }>();
    return errorResponse(current?.current_rev === revision
      ? "The pack marker was published but its registry transaction did not finish"
      : "The pack was published but its registry needs repair", 500);
  }

  // The newest three revision manifests are enough for members and rollback. Keep shared hashes referenced by those revisions.
  // The revision is already published, so this cleanup is best-effort: a storage error must not turn a successful
  // commit into a failure. R2 delete takes up to 1,000 keys per call.
  const expiredRevision = revision - 3;
  if (expiredRevision > 0) {
    try {
      const expiredKey = buildRevisionKey(path.storageToken, expiredRevision, path.prefix);
      const expired = await getManifest(env, expiredKey, packId, expiredRevision);
      const keys = [expiredKey];
      if (expired) {
        for (const hash of expired.requiredBlobs.keys()) {
          if (!retainedBlobSizes.has(hash)) keys.push(buildBlobKey(hash, path.storageToken, path.prefix));
        }
      }
      for (let i = 0; i < keys.length; i += 1000) await env.BUCKET.delete(keys.slice(i, i + 1000));
    } catch {
      // Leftover objects are harmless to readers; they only occupy storage.
    }
  }

  return jsonResponse({ ok: true, revision });
}

async function abortPublish(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const body = await readJson(request);
  if (!body || typeof body.sessionId !== "string") return errorResponse("Missing publish session");
  const result = await env.DB.prepare(`
    UPDATE publish_sessions SET status = 'aborted', status_updated_at = ?, expires_at = MIN(expires_at, ?)
    WHERE session_id = ? AND publisher_id = ? AND expires_at > ? AND status = 'active'
  `).bind(Date.now(), Date.now() + ABORT_GRACE_MS, body.sessionId, publisherResult.id, Date.now()).run();
  if (changes(result) !== 1) return errorResponse("Publish session expired or missing", 409);
  return jsonResponse({ ok: true, reservationHeldUntilExpiry: true });
}

async function rotatePack(request: Request, env: Env): Promise<Response> {
  const publisherResult = await authenticatePublisher(request, env);
  if (publisherResult instanceof Response) return publisherResult;
  const body = await readJson(request);
  if (!body || !isValidPackId(body.packId) || !isValidToken(body.oldReadToken) ||
      typeof body.packSecret !== "string" || body.packSecret.length < 16 || body.packSecret.length > 256) {
    return errorResponse("Invalid link rotation request");
  }
  const packId = body.packId;
  const oldReadToken = body.oldReadToken;
  const packSecretHash = await sha256Hex(body.packSecret);
  const pack = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(packId).first<Pack>();
  if (!pack || pack.publisher_id !== publisherResult.id) return errorResponse("Pack not found", 404);
  if (pack.pack_secret_hash !== packSecretHash) return errorResponse("Invalid pack secret", 403);

  if (pack.previous_read_token === oldReadToken) {
    return jsonResponse({ ok: true, readToken: pack.read_token });
  }
  if (pack.read_token !== oldReadToken) return errorResponse("The invite has already been rotated", 409);

  const newReadToken = generateRandomSecret(24);
  const updated = await env.DB.prepare(`
    UPDATE packs
    SET previous_read_token = read_token, read_token = ?
    WHERE pack_id = ? AND publisher_id = ? AND pack_secret_hash = ? AND read_token = ?
      AND NOT EXISTS (SELECT 1 FROM publish_sessions WHERE pack_id = ?)
  `).bind(newReadToken, packId, publisherResult.id, packSecretHash, oldReadToken, packId).run();
  if (changes(updated) === 1) return jsonResponse({ ok: true, readToken: newReadToken });

  const current = await env.DB.prepare("SELECT * FROM packs WHERE pack_id = ?").bind(packId).first<Pack>();
  if (current?.publisher_id === publisherResult.id && current.pack_secret_hash === packSecretHash &&
      current.previous_read_token === oldReadToken) {
    return jsonResponse({ ok: true, readToken: current.read_token });
  }
  return errorResponse("The pack is busy or its invite changed; check the current owner invite", 409);
}

async function readPack(request: Request, env: Env, url: URL): Promise<Response> {
  if (request.method !== "GET") return new Response(null, { status: 405, headers: { Allow: "GET" } });
  const parts = url.pathname.split("/").filter(Boolean);
  if (parts.length !== 3 && parts.length !== 4) return new Response(null, { status: 404 });
  if (parts[0] !== "read" || !isValidToken(parts[1])) {
    return new Response(null, { status: 404 });
  }
  const readToken = parts[1];
  let relative: string;
  if (parts.length === 3 && parts[2] === "latest.json") {
    relative = "latest.json";
  } else if (parts.length === 4 && parts[2] === "revisions" &&
      /^\d+\.json$/.test(parts[3]) && isValidRevision(Number(parts[3].slice(0, -5)))) {
    relative = `revisions/${parts[3]}`;
  } else if (parts.length === 4 && parts[2] === "files" && isValidSha512(parts[3])) {
    relative = `files/${parts[3]}`;
  } else {
    return new Response(null, { status: 404 });
  }

  const pack = await env.DB.prepare("SELECT * FROM packs WHERE read_token = ?").bind(readToken).first<Pack>();
  if (!pack) return new Response(null, { status: 404 });
  const key = relative === "latest.json"
    ? buildLatestKey(pack.storage_token, pack.prefix)
    : relative.startsWith("revisions/")
      ? buildRevisionKey(pack.storage_token, Number(relative.slice("revisions/".length, -5)), pack.prefix)
      : buildBlobKey(relative.slice("files/".length), pack.storage_token, pack.prefix);
  const object = await env.BUCKET.get(key);
  if (!object) return new Response(null, { status: 404, headers: { "Cache-Control": "no-store" } });
  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("Content-Length", String(object.size));
  headers.set("Cache-Control", "private, no-store");
  if (relative.endsWith(".json")) headers.set("Content-Type", "application/json; charset=utf-8");
  else if (!headers.has("Content-Type")) headers.set("Content-Type", "application/octet-stream");
  return new Response(object.body, { status: 200, headers });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") {
      return new Response(null, {
        status: 204,
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
          "Access-Control-Allow-Headers": "Content-Type, Authorization",
          "Access-Control-Max-Age": "86400",
        },
      });
    }
    try {
      if (url.pathname === "/health" && request.method === "GET") {
        return jsonResponse({ status: "ok", time: Date.now() });
      }
      if (url.pathname === "/publisher/redeem" && request.method === "POST") return await redeemPublisher(request, env);
      if (url.pathname === "/publish/check" && request.method === "POST") return await checkBlobs(request, env);
      if (url.pathname === "/publish/begin" && request.method === "POST") return await beginPublish(request, env);
      if (url.pathname === "/publish/prepare" && request.method === "POST") return await prepareBlobPage(request, env);
      if (url.pathname === "/publish/commit" && request.method === "POST") return await commitPublish(request, env);
      if (url.pathname === "/publish/abort" && request.method === "POST") return await abortPublish(request, env);
      if (url.pathname === "/publish/rotate" && request.method === "POST") return await rotatePack(request, env);
      const uploadMatch = url.pathname.match(/^\/publish\/upload\/([A-Za-z0-9_-]+)\/([0-9a-f]{128})$/);
      if (uploadMatch && request.method === "PUT") return await uploadBlob(request, env, uploadMatch[1], uploadMatch[2]);
      const revisionMatch = url.pathname.match(/^\/publish\/revision\/([A-Za-z0-9_-]+)$/);
      if (revisionMatch && request.method === "PUT") return await uploadRevision(request, env, revisionMatch[1]);
      if (url.pathname.startsWith("/read/")) return await readPack(request, env, url);
      return errorResponse("Not Found", 404);
    } catch (error) {
      // Only the error text is logged (D1/R2 errors carry no request data), so unexpected 500s can be diagnosed.
      console.error("relay error:", error instanceof Error ? error.message : String(error));
      return errorResponse("The relay could not complete the request", 500);
    }
  },
};
