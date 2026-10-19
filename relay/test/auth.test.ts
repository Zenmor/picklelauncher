import test from "node:test";
import assert from "node:assert/strict";
import {
  sha256Hex,
  generateRandomId,
  generateRandomSecret,
  encodePublisherCode,
  parsePublisherCode,
} from "../src/auth.ts";

test("sha256Hex hashes strings accurately", async () => {
  // SHA-256 of empty string is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  assert.equal(
    await sha256Hex(""),
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  );
  // SHA-256 of "picklelauncher"
  const expected = "6fe2f3f80ebb13b4b25f69d5f3d9a88619f0d21cac89d09e9dba6a78436f8e44";
  assert.equal(await sha256Hex("picklelauncher"), expected);
});

test("generateRandomId and generateRandomSecret generate unique url-safe strings", () => {
  const id1 = generateRandomId("pub");
  const id2 = generateRandomId("pub");
  assert.notEqual(id1, id2);
  assert.equal(id1.startsWith("pub_"), true);

  const secret1 = generateRandomSecret(32);
  const secret2 = generateRandomSecret(32);
  assert.notEqual(secret1, secret2);
  assert.match(secret1, /^[A-Za-z0-9_-]+$/);
});

test("encodePublisherCode and parsePublisherCode round-trip cleanly", () => {
  const data = {
    worker: "https://relay.example.workers.dev",
    code: "abcdef1234567890_-ABCDEF",
    label: "FriendAlice",
    exp: 1770000000000,
  };

  const codeStr = encodePublisherCode(data);
  assert.equal(codeStr.startsWith("picklepub:1:"), true);

  const parsed = parsePublisherCode(codeStr);
  assert.deepEqual(parsed, data);
});

test("parsePublisherCode rejects corrupt or non-code strings", () => {
  assert.equal(parsePublisherCode("invalid"), null);
  assert.equal(parsePublisherCode("picklepub:2:abc"), null);
  assert.equal(parsePublisherCode("picklepub:1:!!!invalidbase64"), null);
  assert.equal(parsePublisherCode("picklepub:1:e30="), null); // empty JSON object
});
