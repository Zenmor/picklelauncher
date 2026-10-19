import test from "node:test";
import assert from "node:assert/strict";
import { parsePackManifest } from "../src/manifest.ts";

const packId = "AbCdEfGhIjKlMnOpQrStUv";
const hashA = "a".repeat(128);
const hashB = "b".repeat(128);

function manifest(overrides: Record<string, unknown> = {}): Uint8Array {
  return new TextEncoder().encode(
    JSON.stringify({
      formatVersion: 1,
      packId,
      name: "Test Pack",
      revision: 3,
      components: [{ uid: "net.minecraft", version: "1.21.1" }],
      files: [{ path: "mods/a.jar", size: 10, sha512: hashA }],
      ...overrides,
    }),
  );
}

test("parsePackManifest validates and extracts the owned blob set", () => {
  const parsed = parsePackManifest(manifest(), packId, 3);
  assert.ok(parsed);
  assert.equal(parsed.manifest.name, "Test Pack");
  assert.deepEqual([...parsed.requiredBlobs], [[hashA, 10]]);
});

test("parsePackManifest excludes Modrinth-served files from R2 blobs", () => {
  const parsed = parsePackManifest(
    manifest({
      files: [
        { path: "mods/a.jar", size: 10, sha512: hashA },
        { path: "mods/b.jar", size: 20, sha512: hashB, urls: ["https://cdn.modrinth.com/data/abc/versions/1/b.jar"] },
      ],
    }),
    packId,
    3,
  );
  assert.ok(parsed);
  assert.deepEqual([...parsed.requiredBlobs], [[hashA, 10]]);
});

test("parsePackManifest rejects mismatched identity and unsafe files", () => {
  assert.equal(parsePackManifest(manifest(), packId, 2), null);
  assert.equal(parsePackManifest(manifest({ packId: "OtherPackId123456789012" }), packId, 3), null);
  assert.equal(parsePackManifest(manifest({ files: [{ path: "../escape", size: 1, sha512: hashA }] }), packId, 3), null);
  assert.equal(parsePackManifest(manifest({ files: [{ path: "mods/a.jar", size: 1, sha512: hashA, urls: ["https://evil.example/file"] }] }), packId, 3), null);
});

test("parsePackManifest rejects a file/directory path collision", () => {
  assert.equal(
    parsePackManifest(
      manifest({
        files: [
          { path: "config", size: 1, sha512: hashA },
          { path: "config/a.toml", size: 1, sha512: hashB },
        ],
      }),
      packId,
      3,
    ),
    null,
  );
});
