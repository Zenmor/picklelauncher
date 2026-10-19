-- D1 Database Schema for PickleLauncher Shared Packs Relay

CREATE TABLE IF NOT EXISTS publishers (
    id TEXT PRIMARY KEY,
    label TEXT NOT NULL,
    token_hash TEXT UNIQUE NOT NULL,
    quota_bytes INTEGER NOT NULL DEFAULT 2147483648, -- 2 GB default
    max_file_bytes INTEGER NOT NULL DEFAULT 524288000, -- 500 MB default
    max_packs INTEGER NOT NULL DEFAULT 5,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER
);

CREATE TABLE IF NOT EXISTS publisher_codes (
    code_hash TEXT PRIMARY KEY,
    label TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER,
    publisher_id TEXT
);

CREATE TABLE IF NOT EXISTS packs (
    pack_id TEXT PRIMARY KEY,
    publisher_id TEXT NOT NULL REFERENCES publishers(id),
    pack_secret_hash TEXT NOT NULL,
    current_rev INTEGER NOT NULL DEFAULT 0,
    stored_bytes INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_publishers_token_hash ON publishers(token_hash);
CREATE INDEX IF NOT EXISTS idx_publisher_codes_code_hash ON publisher_codes(code_hash);
CREATE INDEX IF NOT EXISTS idx_packs_publisher ON packs(publisher_id);
