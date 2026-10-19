-- Relay 0002: Worker-streamed uploads, publish sessions, read-token rotation.
--
-- Upgrades a database created from the baseline schema (0001, commit 44f36b6f)
-- to the schema in ../schema.sql. A database created from the CURRENT
-- schema.sql already has all of this: do not run this file on it (the ALTER
-- statements fail on purpose when a column already exists).
--
-- Why `packs` is rebuilt instead of altered: SQLite cannot add a UNIQUE column
-- with ALTER TABLE, and the new read_token / storage_token columns are
-- NOT NULL UNIQUE. Rebuilding is only safe because the guard below refuses to
-- run while any pack exists. Baseline packs cannot be adopted: their objects
-- were stored under a different R2 key layout (keyed by packId, with a prefix
-- that was never saved in D1), and they have no read token. If the rows are
-- test data, clear them first with `DELETE FROM packs;` and apply again.
--
-- Do not add BEGIN/COMMIT or PRAGMA statements here; D1 manages transactions.
--
-- Known, harmless difference from schema.sql: SQLite cannot ALTER a column
-- DEFAULT, so migrated publishers keep the old quota_bytes (2 GiB) and
-- max_file_bytes (500 MB) DEFAULTs. The Worker always inserts both explicitly
-- (publisher redeem), so the defaults are never used. Existing publishers keep
-- their current quota_bytes; raise it with the admin CLI if wanted.
-- Verified on real D1 (query API and `wrangler d1 migrations apply --remote`):
-- when the guard fires, D1 rolls back the whole file. Nothing is left behind and
-- only 0001 stays recorded in d1_migrations. The file is safe to re-run.

-- Guard: fails with "NOT NULL constraint failed: ...packs_must_be_empty" when
-- the baseline database still contains packs. It is the first statement, so a
-- refusal changes nothing; the DROP IF EXISTS makes a re-run after a refusal
-- (when D1 does not roll the file back) work.
DROP TABLE IF EXISTS _relay_migration_guard;
CREATE TABLE _relay_migration_guard (packs_must_be_empty INTEGER NOT NULL);
INSERT INTO _relay_migration_guard (packs_must_be_empty) SELECT NULL WHERE EXISTS (SELECT 1 FROM packs);
DROP TABLE _relay_migration_guard;

-- Publishers keep their rows; new rate-limit columns start at zero.
ALTER TABLE publishers ADD COLUMN rate_window_start INTEGER NOT NULL DEFAULT 0;
ALTER TABLE publishers ADD COLUMN request_count INTEGER NOT NULL DEFAULT 0;
-- The Worker never accepts a file above 100 MB; keep stored limits honest.
UPDATE publishers SET max_file_bytes = 100000000 WHERE max_file_bytes > 100000000;

DROP TABLE packs;
CREATE TABLE packs (
    pack_id TEXT PRIMARY KEY,
    publisher_id TEXT NOT NULL REFERENCES publishers(id),
    pack_secret_hash TEXT NOT NULL,
    read_token TEXT NOT NULL UNIQUE,
    previous_read_token TEXT,
    storage_token TEXT NOT NULL UNIQUE,
    prefix TEXT NOT NULL DEFAULT '',
    current_rev INTEGER NOT NULL DEFAULT 0,
    stored_bytes INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);
CREATE INDEX idx_packs_publisher ON packs(publisher_id);

CREATE TABLE publish_sessions (
    session_id TEXT PRIMARY KEY,
    pack_id TEXT NOT NULL UNIQUE REFERENCES packs(pack_id),
    publisher_id TEXT NOT NULL REFERENCES publishers(id),
    revision INTEGER NOT NULL,
    baseline INTEGER NOT NULL,
    reserved_bytes INTEGER NOT NULL,
    blob_sizes_json TEXT NOT NULL,
    missing_blobs_json TEXT NOT NULL,
    revision_size INTEGER NOT NULL,
    next_blob_offset INTEGER NOT NULL DEFAULT 0,
    expires_at INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'active',
    status_updated_at INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE publish_session_blobs (
    session_id TEXT NOT NULL REFERENCES publish_sessions(session_id) ON DELETE CASCADE,
    sha512 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    status_updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, sha512)
);

CREATE INDEX idx_publish_sessions_publisher ON publish_sessions(publisher_id);
CREATE INDEX idx_publish_sessions_expiry ON publish_sessions(expires_at);
