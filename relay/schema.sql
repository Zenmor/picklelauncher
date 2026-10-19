-- D1 Database Schema for PickleLauncher Shared Packs Relay

CREATE TABLE IF NOT EXISTS publishers (
    id TEXT PRIMARY KEY,
    label TEXT NOT NULL,
    token_hash TEXT UNIQUE NOT NULL,
    quota_bytes INTEGER NOT NULL DEFAULT 3221225472, -- 3 GiB
    max_file_bytes INTEGER NOT NULL DEFAULT 100000000, -- 100 MB Worker request-body limit
    max_packs INTEGER NOT NULL DEFAULT 5,
    rate_window_start INTEGER NOT NULL DEFAULT 0,
    request_count INTEGER NOT NULL DEFAULT 0,
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
    read_token TEXT NOT NULL UNIQUE,
    previous_read_token TEXT,
    storage_token TEXT NOT NULL UNIQUE,
    prefix TEXT NOT NULL DEFAULT '',
    current_rev INTEGER NOT NULL DEFAULT 0,
    stored_bytes INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS publish_sessions (
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

CREATE TABLE IF NOT EXISTS publish_session_blobs (
    session_id TEXT NOT NULL REFERENCES publish_sessions(session_id) ON DELETE CASCADE,
    sha512 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending',
    status_updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (session_id, sha512)
);

CREATE INDEX IF NOT EXISTS idx_publishers_token_hash ON publishers(token_hash);
CREATE INDEX IF NOT EXISTS idx_publisher_codes_code_hash ON publisher_codes(code_hash);
CREATE INDEX IF NOT EXISTS idx_packs_publisher ON packs(publisher_id);
CREATE INDEX IF NOT EXISTS idx_publish_sessions_publisher ON publish_sessions(publisher_id);
CREATE INDEX IF NOT EXISTS idx_publish_sessions_expiry ON publish_sessions(expires_at);
