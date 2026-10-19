#!/usr/bin/env node
// PickleLauncher Shared Packs Relay - Admin CLI Tool

import { createHash, randomBytes } from "node:crypto";
import { execSync } from "node:child_process";

function sha256Hex(str) {
  return createHash("sha256").update(str).digest("hex");
}

function bytesToBase64Url(buf) {
  return buf.toString("base64").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function encodePublisherCode(data) {
  const json = JSON.stringify(data);
  return `picklepub:1:${bytesToBase64Url(Buffer.from(json, "utf-8"))}`;
}

const args = process.argv.slice(2);
const command = args[0];

function usage() {
  console.log(`
PickleLauncher Shared Packs Relay - Admin Tool

Usage:
  node relay/tools/admin.mjs issue-code <label> [--days 7] [--worker <url>] [--db <name>]
  node relay/tools/admin.mjs list [--db <name>]
  node relay/tools/admin.mjs set-quota <publisherId> <bytes> [--db <name>]
  node relay/tools/admin.mjs revoke <publisherId> [--db <name>]
  node relay/tools/admin.mjs delete-pack <packId> [--db <name>]
  node relay/tools/admin.mjs transfer-pack <packId> <newPublisherId> [--db <name>]
`);
}

function getArg(flag, defaultValue) {
  const idx = args.indexOf(flag);
  if (idx !== -1 && idx + 1 < args.length) {
    return args[idx + 1];
  }
  return defaultValue;
}

const dbName = getArg("--db", "voidrelay-db");

function executeSql(sql) {
  try {
    const cmd = `npx wrangler d1 execute ${dbName} --remote --command="${sql.replace(/"/g, '\\"')}"`;
    return execSync(cmd, { encoding: "utf-8", stdio: ["ignore", "pipe", "pipe"] });
  } catch (err) {
    console.error("Wrangler execution failed:", err.message);
    if (err.stderr) console.error(err.stderr.toString());
    process.exit(1);
  }
}

if (!command || command === "--help" || command === "-h") {
  usage();
  process.exit(0);
}

if (command === "issue-code") {
  const label = args[1];
  if (!label) {
    console.error("Error: <label> is required");
    process.exit(1);
  }

  const days = parseInt(getArg("--days", "7"), 10);
  const workerUrl = getArg("--worker", "https://voidrelay.workers.dev");
  const expiresAt = Date.now() + days * 24 * 60 * 60 * 1000;

  const rawCode = bytesToBase64Url(randomBytes(24));
  const codeHash = sha256Hex(rawCode);

  const inviteStr = encodePublisherCode({
    worker: workerUrl,
    code: rawCode,
    label,
    exp: expiresAt,
  });

  const sql = `INSERT INTO publisher_codes (code_hash, label, expires_at) VALUES ('${codeHash}', '${label.replace(/'/g, "''")}', ${expiresAt});`;

  console.log(`\nIssued single-use publisher code for: ${label}`);
  console.log(`Expires: ${new Date(expiresAt).toISOString()}`);
  console.log(`\n--- PUBLISHER CODE (Give this to your friend) ---`);
  console.log(inviteStr);
  console.log(`-------------------------------------------------\n`);
  console.log(`SQL to run in D1:\n${sql}\n`);
} else if (command === "list") {
  console.log("Listing publishers and packs...");
  const sql = `SELECT id, label, quota_bytes, max_file_bytes, max_packs, datetime(created_at/1000, 'unixepoch') as created, revoked_at FROM publishers; SELECT * FROM packs;`;
  console.log(executeSql(sql));
} else if (command === "set-quota") {
  const pubId = args[1];
  const bytes = parseInt(args[2], 10);
  if (!pubId || isNaN(bytes)) {
    console.error("Error: set-quota requires <publisherId> and <bytes>");
    process.exit(1);
  }
  const sql = `UPDATE publishers SET quota_bytes = ${bytes} WHERE id = '${pubId.replace(/'/g, "''")}';`;
  console.log(executeSql(sql));
  console.log(`Updated quota for ${pubId} to ${bytes} bytes.`);
} else if (command === "revoke") {
  const pubId = args[1];
  if (!pubId) {
    console.error("Error: revoke requires <publisherId>");
    process.exit(1);
  }
  const now = Date.now();
  const sql = `UPDATE publishers SET revoked_at = ${now} WHERE id = '${pubId.replace(/'/g, "''")}';`;
  console.log(executeSql(sql));
  console.log(`Revoked publisher ${pubId}.`);
} else if (command === "delete-pack") {
  const packId = args[1];
  if (!packId) {
    console.error("Error: delete-pack requires <packId>");
    process.exit(1);
  }
  const sql = `DELETE FROM packs WHERE pack_id = '${packId.replace(/'/g, "''")}';`;
  console.log(executeSql(sql));
  console.log(`Deleted pack ${packId} from database.`);
} else if (command === "transfer-pack") {
  const packId = args[1];
  const newPubId = args[2];
  if (!packId || !newPubId) {
    console.error("Error: transfer-pack requires <packId> and <newPublisherId>");
    process.exit(1);
  }
  const sql = `UPDATE packs SET publisher_id = '${newPubId.replace(/'/g, "''")}' WHERE pack_id = '${packId.replace(/'/g, "''")}';`;
  console.log(executeSql(sql));
  console.log(`Transferred pack ${packId} to publisher ${newPubId}.`);
} else {
  console.error(`Unknown command: ${command}`);
  usage();
  process.exit(1);
}
