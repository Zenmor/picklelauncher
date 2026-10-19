import test from "node:test";
import assert from "node:assert/strict";
import { checkUploadQuota, findOrphanedBlobs } from "../src/quota.ts";

test("checkUploadQuota accepts uploads within quota limits", () => {
  const quotaBytes = 1000;
  const maxFileBytes = 500;
  const currentStored = 200;

  const blobs = [
    { sha512: "a".repeat(128), size: 100 },
    { sha512: "b".repeat(128), size: 300 },
  ];

  const result = checkUploadQuota(quotaBytes, maxFileBytes, currentStored, blobs);
  assert.equal(result.ok, true);
});

test("checkUploadQuota rejects files exceeding max_file_bytes", () => {
  const quotaBytes = 1000;
  const maxFileBytes = 250;
  const currentStored = 0;

  const blobs = [{ sha512: "a".repeat(128), size: 300 }];

  const result = checkUploadQuota(quotaBytes, maxFileBytes, currentStored, blobs);
  assert.equal(result.ok, false);
  assert.match(result.error || "", /exceeds maximum file size/);
});

test("checkUploadQuota rejects uploads that exceed total quota", () => {
  const quotaBytes = 1000;
  const maxFileBytes = 500;
  const currentStored = 800;

  const blobs = [{ sha512: "a".repeat(128), size: 300 }]; // 800 + 300 = 1100 > 1000

  const result = checkUploadQuota(quotaBytes, maxFileBytes, currentStored, blobs);
  assert.equal(result.ok, false);
  assert.match(result.error || "", /Publisher quota exceeded/);
});

test("findOrphanedBlobs correctly identifies blobs to prune", () => {
  const retainedBlobs = new Set(["blob1", "blob2", "blob3"]);
  const retiredRevisionBlobs = ["blob2", "blob4", "blob5"]; // blob2 is still in use, blob4 & blob5 are orphans

  const orphans = findOrphanedBlobs(retainedBlobs, retiredRevisionBlobs);
  assert.deepEqual(orphans.sort(), ["blob4", "blob5"]);
});
