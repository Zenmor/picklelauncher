import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { DatabaseSync } from "node:sqlite";

// These run the real SQL through SQLite. D1 is SQLite-based, but D1 transaction
// handling of a multi-statement migration file is NOT exercised here.
const read = (name: string) => readFileSync(new URL(`../${name}`, import.meta.url), "utf8");
const baseline = () => {
    const db = new DatabaseSync(":memory:");
    db.exec(read("migrations/0001_baseline.sql"));
    return db;
};

function describeSchema(db: DatabaseSync): string[] {
    const out: string[] = [];
    const tables = db.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name").all() as { name: string }[];
    for (const { name } of tables) {
        const cols = (db.prepare(`PRAGMA table_info(${name})`).all() as any[])
            .map((c) => `${c.name} ${c.type} nn=${c.notnull} d=${c.dflt_value} pk=${c.pk}`).sort();
        const fks = (db.prepare(`PRAGMA foreign_key_list(${name})`).all() as any[])
            .map((f) => `${f.from}>${f.table}.${f.to} del=${f.on_delete}`).sort();
        const uniq = (db.prepare(`PRAGMA index_list(${name})`).all() as any[])
            .map((i) => `${i.unique ? "U" : "I"}:` + (db.prepare(`PRAGMA index_info(${i.name})`).all() as any[]).map((x) => x.name).join(",")).sort();
        out.push(`${name}\n  ${cols.join("\n  ")}\n  fk: ${fks.join(" ")}\n  idx: ${uniq.join(" ")}`);
    }
    return out;
}

test("0001 + 0002 produce the same schema as schema.sql", () => {
    const fresh = new DatabaseSync(":memory:");
    fresh.exec(read("schema.sql"));
    const migrated = baseline();
    migrated.exec(read("migrations/0002_publish_sessions_and_read_tokens.sql"));
    // Known, accepted difference: SQLite cannot ALTER a column DEFAULT, so migrated
    // publishers keep the old quota/max_file defaults. The Worker always inserts
    // both columns explicitly (see the redeem INSERT), so the defaults are unused.
    const known = (s: string) => s
        .replace("max_file_bytes INTEGER nn=1 d=524288000", "max_file_bytes INTEGER nn=1 d=100000000")
        .replace("quota_bytes INTEGER nn=1 d=2147483648", "quota_bytes INTEGER nn=1 d=3221225472");
    assert.deepEqual(describeSchema(migrated).map(known), describeSchema(fresh));
});

test("0002 keeps publishers and clamps max_file_bytes to the Worker limit", () => {
    const db = baseline();
    db.exec("INSERT INTO publishers (id,label,token_hash,created_at) VALUES ('p','l','h',1)");
    db.exec("INSERT INTO publishers (id,label,token_hash,max_file_bytes,created_at) VALUES ('q','l','h2',50,1)");
    db.exec(read("migrations/0002_publish_sessions_and_read_tokens.sql"));
    const rows = db.prepare("SELECT id,max_file_bytes,request_count,rate_window_start FROM publishers ORDER BY id").all() as any[];
    assert.deepEqual(rows.map((r) => [r.id, r.max_file_bytes, r.request_count, r.rate_window_start]), [["p", 100000000, 0, 0], ["q", 50, 0, 0]]);
});

test("0002 refuses a database that still has packs and changes nothing; re-run after clearing works", () => {
    const db = baseline();
    db.exec("INSERT INTO publishers (id,label,token_hash,created_at) VALUES ('p','l','h',1)");
    db.exec("INSERT INTO packs (pack_id,publisher_id,pack_secret_hash,created_at) VALUES ('k','p','s',1)");
    const sql = read("migrations/0002_publish_sessions_and_read_tokens.sql");
    assert.throws(() => db.exec(sql), /packs_must_be_empty/);
    assert.equal((db.prepare("PRAGMA table_info(packs)").all() as any[]).length, 6);
    assert.equal((db.prepare("PRAGMA table_info(publishers)").all() as any[]).length, 8);
    db.exec("DELETE FROM packs");
    db.exec(sql); // guard table left by the refusal must not block the re-run
    assert.equal((db.prepare("PRAGMA table_info(packs)").all() as any[]).length, 10);
    assert.equal(db.prepare("SELECT count(*) n FROM sqlite_master WHERE name='_relay_migration_guard'").get()!.n, 0);
});

test("0001 is idempotent so `d1 migrations apply` is safe on a database made from the baseline schema.sql", () => {
    const db = baseline();
    db.exec(read("migrations/0001_baseline.sql"));
});
