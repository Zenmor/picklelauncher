import test from "node:test";
import assert from "node:assert/strict";
import { createPresignedPutUrl } from "../src/presign.ts";

test("createPresignedPutUrl generates valid SigV4 presigned URL", async () => {
  const fixedDate = new Date("2026-10-17T12:00:00Z");
  const urlString = await createPresignedPutUrl({
    accountId: "0123456789abcdef0123456789abcdef",
    bucket: "test-bucket",
    key: "files/abc123hash",
    accessKeyId: "TESTACCESSKEYID",
    secretAccessKey: "TESTSECRETACCESSKEYVALUE0123456789ABCDEF",
    contentLength: 1048576, // 1 MB
    expiresInSeconds: 900,
    now: fixedDate,
  });

  const parsed = new URL(urlString);
  assert.equal(parsed.protocol, "https:");
  assert.equal(parsed.host, "0123456789abcdef0123456789abcdef.r2.cloudflarestorage.com");
  assert.equal(parsed.pathname, "/test-bucket/files/abc123hash");

  const q = parsed.searchParams;
  assert.equal(q.get("X-Amz-Algorithm"), "AWS4-HMAC-SHA256");
  assert.equal(q.get("X-Amz-Date"), "20261017T120000Z");
  assert.equal(q.get("X-Amz-Expires"), "900");
  assert.equal(q.get("X-Amz-SignedHeaders"), "content-length;host");
  assert.equal(q.get("X-Amz-Credential"), "TESTACCESSKEYID/20261017/auto/s3/aws4_request");
  assert.match(q.get("X-Amz-Signature") || "", /^[0-9a-f]{64}$/);
});

test("createPresignedPutUrl is deterministic for identical inputs", async () => {
  const fixedDate = new Date("2026-10-17T12:00:00Z");
  const opts = {
    accountId: "0123456789abcdef0123456789abcdef",
    bucket: "test-bucket",
    key: "revisions/1.json",
    accessKeyId: "KEY",
    secretAccessKey: "SECRET",
    contentLength: 4096,
    now: fixedDate,
  };

  const url1 = await createPresignedPutUrl(opts);
  const url2 = await createPresignedPutUrl(opts);
  assert.equal(url1, url2);
});

test("createPresignedPutUrl changes signature when Content-Length changes", async () => {
  const fixedDate = new Date("2026-10-17T12:00:00Z");
  const baseOpts = {
    accountId: "0123456789abcdef0123456789abcdef",
    bucket: "test-bucket",
    key: "files/blob",
    accessKeyId: "KEY",
    secretAccessKey: "SECRET",
    now: fixedDate,
  };

  const url1 = await createPresignedPutUrl({ ...baseOpts, contentLength: 100 });
  const url2 = await createPresignedPutUrl({ ...baseOpts, contentLength: 200 });

  const sig1 = new URL(url1).searchParams.get("X-Amz-Signature");
  const sig2 = new URL(url2).searchParams.get("X-Amz-Signature");
  assert.notEqual(sig1, sig2);
});
