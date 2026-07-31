#!/usr/bin/env python3
"""
Hathaway serial-logging ingest service.

Reads the line protocol emitted by the ESP32-S3 firmware from a serial port
(or stdin, for board-free testing), parses it, and writes it into a database
in batches. Continuous samples go to `samples`, discrete events to `events`.

Backends (choose with --db):
  postgres : PostgreSQL / TimescaleDB      (production; needs psycopg2)
  print    : just echo parsed records      (smoke test, no DB)

Sources (choose with --source):
  serial   : a COM port via pyserial       (needs pyserial; --port required)
  stdin    : read lines from standard input (for piping fake data in tests)

Examples
  # Real board -> TimescaleDB
  python ingest.py --source serial --port COM3 --db postgres \
      --dsn "host=localhost dbname=hathaway user=hathaway password=hathaway"

  # No board at all: pipe the simulator in
  python fake_serial.py | python ingest.py --source stdin --db print
"""
import argparse
import sys
import time
import datetime as dt

BATCH_SIZE = 200        # rows per DB write
FLUSH_SECONDS = 0.5     # or flush at least this often


# --------------------------------------------------------------------------- #
# Protocol parsing
# --------------------------------------------------------------------------- #
def parse_line(line):
    """Return a parsed record dict, or None for comments/blank/malformed lines.

    Data line:  seq,t_us,kind,type,channel,value
    """
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.split(",")
    if len(parts) != 6:
        return None
    try:
        return {
            "seq": int(parts[0]),
            "t_us": int(parts[1]),
            "kind": parts[2],            # 'S' or 'E'
            "type": parts[3],
            "channel": int(parts[4]),
            "value": float(parts[5]),
        }
    except ValueError:
        return None


def read_rig_id(line, default=1):
    """Pull rig id out of a '#RIG <n>' header line, else return default."""
    if line.startswith("#RIG"):
        try:
            return int(line.split()[1])
        except (IndexError, ValueError):
            pass
    return default


def _dev_us(parts, idx):
    """Device time (ms from firmware millis()) -> microseconds, or None."""
    if len(parts) > idx and parts[idx].strip():
        return int(parts[idx].strip()) * 1000
    return None


# --------------------------------------------------------------------------- #
# Hathaway wire format
#
# Every data line has one shape:
#
#     <RIG_ID>|NAME:<channel>,<value>,<device_ms>
#
# so this parser needs no per-message knowledge. The only thing it cannot read
# off the line is whether a message is a state SAMPLE or an EVENT, and the rig
# announces that at startup with one "#DEF <NAME>,<S|E>" line per message type.
# Adding a new message to the firmware therefore requires no change here.
#
# KNOWN_KINDS is only a fallback for a rig that has already been streaming when
# ingest starts, so its #DEF burst was missed.
# --------------------------------------------------------------------------- #
KNOWN_KINDS = {
    "WEIGHT": "S", "POSITION": "S", "MAGNET": "S",
    "LICK": "E", "REWARD": "E",
}

# Learned from "#DEF" lines at runtime; takes precedence over KNOWN_KINDS.
_schema = {}

# Types that also produce a cumulative-count sample alongside the event.
COUNTED_EVENTS = {"REWARD": "REWARD_COUNT"}


def strip_rig_prefix(line):
    """Split '<rig>|<body>' into (rig_id, body). No prefix -> (None, line)."""
    head, sep, rest = line.partition("|")
    if sep and head.isdigit():
        return int(head), rest
    return None, line


def parse_schema_line(line):
    """Learn a message kind from '#DEF <NAME>,<S|E>'. True if it was one."""
    s = line.strip()
    if not s.startswith("#DEF "):
        return False
    name, _, kind = s[len("#DEF "):].partition(",")
    kind = kind.strip().upper()
    if name.strip() and kind in ("S", "E"):
        _schema[name.strip()] = kind
        return True
    return False


def parse_hathaway(line):
    """Parse one line of hathaway serial output.

    Returns a list of record dicts (0, 1, or 2); unknown lines -> [].
    Each dict: {kind 'S'|'E', type, channel, value, t_us (or None)}.
    The '<rig>|' prefix is optional here, so callers that already stripped it
    (control_panel.py) and callers that did not both work.
    """
    _, s = strip_rig_prefix(line.strip())
    s = s.strip()
    if not s:
        return []
    if s.startswith("#"):            # #DEF / #ERR / #TARE ok ...
        parse_schema_line(s)
        return []

    name, sep, rest = s.partition(":")
    if not sep:
        return []
    name = name.strip()
    parts = rest.split(",")
    if len(parts) != 3:
        return []
    try:
        channel = int(parts[0])
        value = float(parts[1])
        t_us = _dev_us(parts, 2)
    except (ValueError, IndexError):
        return []

    kind = _schema.get(name) or KNOWN_KINDS.get(name)
    if kind is None:
        # Unannounced message type: record it as an event rather than drop it.
        kind = "E"

    recs = []
    counted = COUNTED_EVENTS.get(name)
    if counted:                      # cumulative line, for step plots
        recs.append({"kind": "S", "type": counted, "channel": channel,
                     "value": value, "t_us": t_us})
    recs.append({"kind": kind, "type": name, "channel": channel,
                 "value": value, "t_us": t_us})
    return recs


# --------------------------------------------------------------------------- #
# Database backends
# --------------------------------------------------------------------------- #
class BaseDB:
    def start_session(self, rig_id, note):
        raise NotImplementedError

    def write(self, samples, events):
        raise NotImplementedError

    def close(self):
        pass


class PrintDB(BaseDB):
    def start_session(self, rig_id, note):
        print(f"[session] rig={rig_id} note={note!r}", file=sys.stderr)
        return 1

    def write(self, samples, events):
        for r in samples + events:
            print(r)

    def close(self):
        sys.stdout.flush()


class PostgresDB(BaseDB):
    def __init__(self, dsn):
        import psycopg2
        import psycopg2.extras
        self._extras = psycopg2.extras
        self.conn = psycopg2.connect(dsn)
        self.conn.autocommit = False
        # Ensure schema exists (idempotent).
        import os
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "schema.sql")) as fh:
            with self.conn.cursor() as cur:
                cur.execute(fh.read())
        self.conn.commit()
        self.session_id = None

    def start_session(self, rig_id, note):
        with self.conn.cursor() as cur:
            cur.execute(
                "INSERT INTO sessions(rig_id, note) VALUES(%s,%s) "
                "RETURNING session_id", (rig_id, note))
            self.session_id = cur.fetchone()[0]
        self.conn.commit()
        return self.session_id

    def write(self, samples, events):
        with self.conn.cursor() as cur:
            if samples:
                self._extras.execute_values(
                    cur,
                    "INSERT INTO samples(session_id,rig_id,seq,t_us,host_ts,"
                    "type,channel,value) VALUES %s", samples)
            if events:
                self._extras.execute_values(
                    cur,
                    "INSERT INTO events(session_id,rig_id,seq,t_us,host_ts,"
                    "type,channel,value) VALUES %s", events)
        self.conn.commit()

    def close(self):
        if self.session_id is not None:
            with self.conn.cursor() as cur:
                cur.execute("UPDATE sessions SET ended_at=now() "
                            "WHERE session_id=%s", (self.session_id,))
            self.conn.commit()
        self.conn.close()


def make_db(args):
    if args.db == "print":
        return PrintDB()
    if args.db == "postgres":
        return PostgresDB(args.dsn)
    raise ValueError(args.db)


# --------------------------------------------------------------------------- #
# Source iterators
# --------------------------------------------------------------------------- #
def serial_lines(port, baud):
    import serial  # pyserial
    ser = serial.Serial(port, baud, timeout=0.2)
    buf = b""
    try:
        while True:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    yield raw.decode("ascii", "replace")
    finally:
        ser.close()


def stdin_lines():
    for raw in sys.stdin:
        yield raw


def simulate_lines(hz, rig):
    """Emit fake data in the SAME human-readable format as hathaway.ino, in
    real time. Exercises the real parser; no second process / pipe needed."""
    import random
    t0 = time.monotonic()
    period = 1.0 / hz
    reward_num = 0
    position = 0
    magnet = 0
    # Announce the message set exactly as the firmware does at startup.
    for name, kind in (("WEIGHT", "S"), ("POSITION", "S"), ("MAGNET", "S"),
                       ("LICK", "E"), ("REWARD", "E")):
        yield f"1|#DEF {name},{kind}"
    while True:
        now = time.monotonic()
        t_ms = int((now - t0) * 1000)
        # continuous weight
        yield f"1|WEIGHT:1,{20.0 + 2.0 * random.random():.2f},{t_ms}"
        # occasional position flip
        if random.random() < 0.02:
            position ^= 1
            yield f"1|POSITION:1,{position},{t_ms}"
        # occasional magnet on/off flip
        if random.random() < 0.02:
            magnet ^= 1
            yield f"1|MAGNET:1,{magnet},{t_ms}"
        # sparse licks
        if random.random() < 0.15:
            yield f"1|LICK:{random.choice((1, 2))},1,{t_ms}"
        # rare rewards
        if random.random() < 0.03:
            reward_num += 1
            yield f"1|REWARD:1,{reward_num},{t_ms}"
        time.sleep(period)


# --------------------------------------------------------------------------- #
# Main loop
# --------------------------------------------------------------------------- #
def run(args):
    db = make_db(args)
    rig_id = args.rig
    session_id = None

    samples, events = [], []
    n_written = 0
    last_flush = time.monotonic()

    def flush():
        nonlocal samples, events, n_written, last_flush
        if samples or events:
            db.write(samples, events)
            n_written += len(samples) + len(events)
            samples, events = [], []
        last_flush = time.monotonic()

    if args.source == "serial":
        source = serial_lines(args.port, args.baud)
    elif args.source == "simulate":
        source = simulate_lines(args.hz, args.rig)
    else:
        source = stdin_lines()

    # The real firmware emits no sequence number, so we assign one here.
    seq = 0
    print(f"[ingest] db={args.db} source={args.source} format={args.format}"
          f" -> waiting for data...", file=sys.stderr, flush=True)
    last_beat = time.monotonic()
    try:
        for line in source:
            s = line.strip()
            if args.format == "csv":
                if s.startswith("#RIG"):
                    rig_id = read_rig_id(s, rig_id)
                    continue
                rec = parse_line(line)
                recs = [rec] if rec else []
            else:  # hathaway (real firmware / simulate)
                # Every firmware line is prefixed "<rig>|" so several rigs can
                # share one stream; take the rig id from the line when present.
                line_rig, _ = strip_rig_prefix(s)
                if line_rig is not None:
                    rig_id = line_rig
                recs = parse_hathaway(line)
            if not recs:
                continue

            if session_id is None:
                session_id = db.start_session(rig_id, args.note)
                print(f"[ingest] session {session_id} started (rig {rig_id})",
                      file=sys.stderr, flush=True)

            host_ts = dt.datetime.now(dt.timezone.utc).isoformat()
            for rec in recs:
                seq += 1
                t_us = rec.get("t_us")
                if t_us is None:
                    t_us = 0        # this line carried no device timestamp
                elif "seq" in rec:  # csv format supplies its own seq
                    pass
                row = (session_id, rig_id, rec.get("seq", seq), t_us, host_ts,
                       rec["type"], rec["channel"], rec["value"])
                (samples if rec["kind"] == "S" else events).append(row)

            if len(samples) + len(events) >= BATCH_SIZE:
                flush()
            elif time.monotonic() - last_flush >= FLUSH_SECONDS:
                flush()

            # heartbeat so you can see it's alive
            if time.monotonic() - last_beat >= 2.0:
                print(f"[ingest] running... {n_written} rows written",
                      file=sys.stderr, flush=True)
                last_beat = time.monotonic()
    except KeyboardInterrupt:
        print("\n[ingest] stopping...", file=sys.stderr)
    finally:
        flush()
        db.close()
        print(f"[ingest] done. rows written: {n_written}", file=sys.stderr)


def main():
    p = argparse.ArgumentParser(description="Hathaway serial ingest service")
    p.add_argument("--source", choices=["serial", "stdin", "simulate"],
                   default="serial")
    p.add_argument("--port", help="serial port, e.g. COM3 (Windows) or /dev/ttyACM0")
    p.add_argument("--baud", type=int, default=115200,
                   help="115200 for the real hathaway.ino")
    p.add_argument("--format", choices=["hathaway", "csv"], default="hathaway",
                   help="hathaway = real firmware's human-readable prints; "
                        "csv = the seq,t_us,kind,type,channel,value protocol")
    p.add_argument("--hz", type=float, default=30.0,
                   help="fake data rate when --source simulate")
    p.add_argument("--db", choices=["postgres", "print"], default="postgres")
    p.add_argument("--dsn",
                   default="host=localhost dbname=hathaway "
                           "user=hathaway password=hathaway",
                   help="PostgreSQL connection string")
    p.add_argument("--rig", type=int, default=1, help="fallback rig id")
    p.add_argument("--note", default="", help="session note")
    args = p.parse_args()
    if args.source == "serial" and not args.port:
        p.error("--port is required when --source serial")
    run(args)


if __name__ == "__main__":
    main()
