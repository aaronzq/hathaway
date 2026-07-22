-- ============================================================================
-- Hathaway serial-logging test  -  database schema
-- Target: PostgreSQL 14+ with the TimescaleDB extension.
-- Also runs on plain PostgreSQL (the TimescaleDB block is optional and
-- guarded). A near-identical schema for SQLite lives in ingest.py.
-- ============================================================================

-- A "session" = one recording run from one rig.
CREATE TABLE IF NOT EXISTS sessions (
    session_id  BIGSERIAL PRIMARY KEY,
    rig_id      INTEGER      NOT NULL,
    started_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),
    ended_at    TIMESTAMPTZ,
    note        TEXT
);

-- Continuous sensor data (e.g. the load cell). High volume, regular rate.
CREATE TABLE IF NOT EXISTS samples (
    session_id  BIGINT       NOT NULL REFERENCES sessions(session_id),
    rig_id      INTEGER      NOT NULL,
    seq         BIGINT       NOT NULL,   -- device per-rig counter
    t_us        BIGINT       NOT NULL,   -- device clock, microseconds (precise)
    host_ts     TIMESTAMPTZ  NOT NULL,   -- PC wall-clock at ingest
    type        TEXT         NOT NULL,   -- e.g. LOADCELL
    channel     INTEGER      NOT NULL,
    value       DOUBLE PRECISION NOT NULL
);

-- Discrete events (licks, rewards, stimulus onsets...). Bursty, sparse.
CREATE TABLE IF NOT EXISTS events (
    session_id  BIGINT       NOT NULL REFERENCES sessions(session_id),
    rig_id      INTEGER      NOT NULL,
    seq         BIGINT       NOT NULL,
    t_us        BIGINT       NOT NULL,
    host_ts     TIMESTAMPTZ  NOT NULL,
    type        TEXT         NOT NULL,   -- LICK | REWARD | ...
    channel     INTEGER      NOT NULL,
    value       DOUBLE PRECISION NOT NULL
);

-- Fast time-range and per-rig lookups (used by the dashboards).
CREATE INDEX IF NOT EXISTS idx_samples_host_ts ON samples (host_ts DESC);
CREATE INDEX IF NOT EXISTS idx_samples_rig_seq ON samples (rig_id, seq);
CREATE INDEX IF NOT EXISTS idx_events_host_ts  ON events  (host_ts DESC);
CREATE INDEX IF NOT EXISTS idx_events_rig_seq  ON events  (rig_id, seq);

-- ----------------------------------------------------------------------------
-- FUTURE: microscope image data. Pixels live on disk/NAS; only metadata here,
-- joined to behavior on time. Left as a comment now so the design is on record.
-- CREATE TABLE frames (
--     session_id BIGINT REFERENCES sessions(session_id),
--     t_us       BIGINT,          -- frame time on the shared clock
--     frame_idx  BIGINT,
--     file_path  TEXT,            -- path to the TIFF/stack/video
--     meta       JSONB
-- );
-- ----------------------------------------------------------------------------

-- ----------------------------------------------------------------------------
-- TimescaleDB (optional). Converts the big tables into "hypertables" that are
-- transparently partitioned by time -> fast inserts + fast time-range queries
-- + built-in compression/retention. Safe to skip on plain PostgreSQL.
-- ----------------------------------------------------------------------------
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_available_extensions WHERE name = 'timescaledb') THEN
        CREATE EXTENSION IF NOT EXISTS timescaledb;
        PERFORM create_hypertable('samples', 'host_ts',
                                  if_not_exists => TRUE, migrate_data => TRUE);
        PERFORM create_hypertable('events', 'host_ts',
                                  if_not_exists => TRUE, migrate_data => TRUE);
    END IF;
END $$;
