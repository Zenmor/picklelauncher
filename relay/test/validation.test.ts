import test from "node:test";
import assert from "node:assert/strict";
import {
  isValidSha512,
  isValidSha256,
  isValidPackId,
  isValidToken,
  isValidPrefix,
  isValidKey,
  buildBlobKey,
  buildRevisionKey,
  buildLatestKey,
  isSafeManifestPath,
  isAllowedModrinthUrl,
} from "../src/validation.ts";

test("isValidSha512 validates 128 lowercase hex characters", () => {
  const valid = "a".repeat(128);
  assert.equal(isValidSha512(valid), true);
  assert.equal(isValidSha512("A".repeat(128)), false); // uppercase not allowed
  assert.equal(isValidSha512("a".repeat(127)), false);
  assert.equal(isValidSha512("a".repeat(129)), false);
  assert.equal(isValidSha512("g".repeat(128)), false); // non-hex
  assert.equal(isValidSha512(""), false);
});

test("isValidSha256 validates 64 lowercase hex characters", () => {
  const valid = "f".repeat(64);
  assert.equal(isValidSha256(valid), true);
  assert.equal(isValidSha256("F".repeat(64)), false);
  assert.equal(isValidSha256("f".repeat(63)), false);
  assert.equal(isValidSha256("z".repeat(64)), false);
});

test("pack IDs match launcher rules and read tokens are base64url", () => {
  const valid = "AbCdEfGhIjKlMnOpQrStUv"; // 22 chars
  assert.equal(isValidPackId(valid), true);
  assert.equal(isValidToken(valid), true);

  assert.equal(isValidPackId("short"), true);
  assert.equal(isValidPackId("a".repeat(65)), false);
  assert.equal(isValidToken("short"), false);
  assert.equal(isValidToken("a".repeat(129)), false);
  assert.equal(isValidPackId(valid + "!"), false); // special char not allowed
  assert.equal(isValidPackId(valid + " "), false);
});

test("isValidPrefix prevents path traversal", () => {
  assert.equal(isValidPrefix(""), true);
  assert.equal(isValidPrefix("packs"), true);
  assert.equal(isValidPrefix("a/b/c"), true);

  assert.equal(isValidPrefix("/leading"), false);
  assert.equal(isValidPrefix("trailing/"), false);
  assert.equal(isValidPrefix("../escape"), false);
  assert.equal(isValidPrefix("a/../b"), false);
  assert.equal(isValidPrefix("a/./b"), false);
  assert.equal(isValidPrefix("a//b"), false);
});

test("buildBlobKey creates safe keys", () => {
  const sha = "0123456789abcdef".repeat(8);
  const token = "AbCdEfGhIjKlMnOpQrStUv";
  assert.equal(buildBlobKey(sha, token), `${token}/files/${sha}`);
  assert.equal(buildBlobKey(sha, token, "shared"), `shared/${token}/files/${sha}`);

  assert.throws(() => buildBlobKey("bad", token), /Invalid sha512/);
  assert.throws(() => buildBlobKey(sha, token, "../bad"), /Invalid prefix/);
});

test("manifest paths follow the launcher's cross-platform path rules", () => {
  for (const path of ["mods/a.jar", "config/sub/file.toml", "options.txt"]) {
    assert.equal(isSafeManifestPath(path), true);
  }
  for (const path of ["../escape", "mods/CON.jar", "mods/a.jar.", "mods/a?.jar", "saves/world/level.dat", "mods/.index/file", "a.packnew"]) {
    assert.equal(isSafeManifestPath(path), false, path);
  }
});

test("external manifest URLs are restricted to Modrinth CDN paths", () => {
  assert.equal(isAllowedModrinthUrl("https://cdn.modrinth.com/data/abc/versions/1/file.jar"), true);
  for (const url of [
    "http://cdn.modrinth.com/data/abc/file.jar",
    "https://evil.example/data/abc/file.jar",
    "https://cdn.modrinth.com/elsewhere/file.jar",
    "https://user@cdn.modrinth.com/data/abc/file.jar",
    "https://cdn.modrinth.com:444/data/abc/file.jar",
  ]) {
    assert.equal(isAllowedModrinthUrl(url), false, url);
  }
});

test("buildRevisionKey creates safe keys", () => {
  const token = "AbCdEfGhIjKlMnOpQrStUv";
  assert.equal(buildRevisionKey(token, 1), `${token}/revisions/1.json`);
  assert.equal(buildRevisionKey(token, 42, "shared"), `shared/${token}/revisions/42.json`);

  assert.throws(() => buildRevisionKey(token, 0), /Invalid revision/);
  assert.throws(() => buildRevisionKey(token, -1), /Invalid revision/);
});

test("buildLatestKey creates safe keys", () => {
  const token = "AbCdEfGhIjKlMnOpQrStUv";
  assert.equal(buildLatestKey(token), `${token}/latest.json`);
  assert.equal(buildLatestKey(token, "prefix"), `prefix/${token}/latest.json`);
});
