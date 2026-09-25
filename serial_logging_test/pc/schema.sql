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
    t_us        BIGINT       NOT NULL,   -- EPOCH microseconds, from the device
                                         -- clock (see DEVICE TIME below)
    host_ts     TIMESTAMPTZ  NOT NULL,   -- PC wall-clock when the line was read
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

-- Device time is the plotted axis, so index it.
CREATE INDEX IF NOT EXISTS idx_samples_t_us ON samples (t_us DESC);
CREATE INDEX IF NOT EXISTS idx_events_t_us  ON events  (t_us DESC);


-- ============================================================================
-- DEVICE TIME  --  the column the dashboards plot
-- ============================================================================
--
-- `host_ts` is the PC's wall clock at the moment a line was read, and it is
-- badly lumpy: both readers call ser.read(4096) with a 0.2 s timeout, so a whole
-- 200 ms batch of lines is handed over at once and stamped microseconds apart.
-- Two events 100 ms apart on the rig land either on the same host_ts or 200 ms
-- apart, at random. Fine for "roughly when did this session happen", useless
-- for anything inside a trial. It is kept because it is the only independent
-- record of arrival, which is what lets a bad clock anchor be spotted later.
--
-- `t_us` is the real timing: the firmware's own millis(), stamped at the instant
-- of capture, already converted to EPOCH microseconds by the DeviceClass in
-- ingest.py. Reboots, the 49.7-day millis() wrap and the choice of anchor are
-- all handled there, once, at write time -- see the class docstring.
--
-- So these views are a plain alias: no joins, no anchor lookup, nothing to go
-- wrong. They exist only to give the converted column a name the dashboards can
-- read, and to keep `SELECT *` working.
-- ----------------------------------------------------------------------------

CREATE OR REPLACE VIEW samples_dev AS
SELECT *, to_timestamp(t_us / 1000000.0) AS dev_ts FROM samples;

CREATE OR REPLACE VIEW events_dev AS
SELECT *, to_timestamp(t_us / 1000000.0) AS dev_ts FROM events;


-- Old rows written before the conversion hold device UPTIME microseconds, not
-- epoch, so they map to 1970 and simply fall outside any dashboard time range.
-- The two are far apart (uptime < 1e12, epoch > 1.7e15) if you ever need to
-- tell them apart or migrate them.


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
