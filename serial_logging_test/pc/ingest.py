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
    # Keep this in step with TELEM_TABLE in hathaway.ino. It is only a fallback,
    # but a WRONG fallback is worse than no fallback: a sample misfiled as an
    # event lands in the wrong table and quietly disappears from every dashboard
    # panel that reads `samples`.
    "TONE": "S",      # frequency Hz while sounding, 0 when silent
    "TASK": "S",      # id of the running task
    "STATE": "E",     # state entered; channel = state index
}

# Learned from "#DEF" lines at runtime; takes precedence over KNOWN_KINDS.
_schema = {}

# Message types we have already complained about, so the warning is printed once
# rather than on every line.
_warned_unknown = set()


# --------------------------------------------------------------------------- #
# Device clock
# --------------------------------------------------------------------------- #
class DeviceClock:
    """Turn the firmware's millis() into absolute epoch microseconds.

    Why this exists
    ---------------
    The rig stamps every record with millis() at the instant of capture, which is
    the only trustworthy timing in the system. The PC's own arrival time is
    quantised to the serial read cycle (ser.read with a 0.2 s timeout hands over
    a whole batch at once), so two events 100 ms apart on the rig can arrive with
    identical host timestamps. Anything measured inside a trial has to come from
    the device clock.

    What millis() lacks is an origin: it counts from power-on. This class supplies
    one, and deals with the three ways that goes wrong.

    The origin
        Taken from the first record of a run: offset = host_time - device_time,
        then frozen. That first reading is late by however long the line sat in
        the serial buffer -- up to ~200 ms -- so the absolute wall-clock label on
        every row inherits that error.

        It does not matter here, because the same constant is added to every row,
        so it cancels out of every interval. Gaps between events -- which is what
        the rig measures -- stay exact.

        If absolute accuracy is ever wanted, pass warmup_s: the offset is then
        the SMALLEST difference seen over that many seconds, which is a better
        estimate because arrival delay only ever adds, never subtracts. Records
        are buffered while it watches. Off by default.

    The 49.7-day wrap
        millis() is uint32 and wraps at 2**32 ms. Deltas are taken modulo 2**32
        and accumulated into a 64-bit counter, so the wrap needs no special case:
        the wrapped delta across the boundary is just small and positive.

    Reboots
        A reset restarts millis() at 0, which looks the same as a wrap. Three
        signals, preferred in this order:
          1. an explicit BOOT record from the firmware (unambiguous)
          2. the schema burst the firmware emits in setup() -- announce_boot()
          3. a jump too large to be a wrap (fallback, threshold below)
        On a reboot the clock re-anchors: a new warm-up, a new offset.
    """

    WRAP = 1 << 32                  # millis() is uint32
    WARMUP_S = 0.0                  # 0 = take the offset from the first record
                                    # and write immediately; >0 spends that many
                                    # seconds looking for a better one first
    WARMUP_MIN_SAMPLES = 1
    # A forward jump larger than this cannot be explained by the wrap, so treat
    # it as a reset. Deliberately generous: the cost of a false positive is a
    # re-anchor (a small step at the seam), and a logger that was disconnected
    # for a while should NOT be mistaken for a reboot.
    REBOOT_JUMP_MS = 6 * 3600 * 1000

    def __init__(self, warmup_s=None):
        self._warmup_s = self.WARMUP_S if warmup_s is None else warmup_s
        self._reset_state()
        self.reboots = 0
        self.wraps = 0

    # -- internal ----------------------------------------------------------
    def _reset_state(self):
        self._prev_raw = None       # last raw millis() seen
        self._extended = None       # raw millis() widened past the wrap
        self._offset_us = None      # host_us - device_us, the anchor
        self._warm_until = None     # host time the warm-up ends
        self._samples = 0
        self._pending = []          # records held during warm-up
        self._boot_armed = False    # a #DEF burst is awaiting corroboration

    def arm_boot(self):
        """A schema burst arrived: treat the next backward step as a restart.

        On its own a #DEF burst is ambiguous -- the firmware sends one from
        setup(), but so does any host that asks for DUMP. So it does not
        re-anchor by itself; it only makes the next backward step in device time
        count as a reset without having to clear the size threshold.
        """
        self._boot_armed = True

    def _extend(self, raw_ms):
        """Widen raw millis() into a monotonic 64-bit value. Returns None on reboot."""
        if self._prev_raw is None:
            self._extended = raw_ms
        else:
            went_back = raw_ms < self._prev_raw
            if went_back and self._boot_armed:
                return None                     # corroborated restart
            delta = (raw_ms - self._prev_raw) % self.WRAP
            if delta > self.REBOOT_JUMP_MS:
                return None                     # too big to be the wrap -> reset
            if went_back:
                self.wraps += 1                 # crossed 2**32
            self._extended += delta
        self._prev_raw = raw_ms
        self._boot_armed = False
        return self._extended

    def _flush(self):
        """Release parked records using the offset estimated so far."""
        if not self._pending:
            return []
        off = self._offset_us or 0
        out = [(item, ms * 1000 + off) for item, ms in self._pending]
        self._pending = []
        return out

    # -- public ------------------------------------------------------------
    # One call per record. Hand it the thing you want stamped and it hands back
    # whatever is now ready to write -- usually just that record, sometimes a
    # backlog released because the warm-up finished, sometimes nothing at all
    # because the warm-up is still running. The caller never has to know which.
    def stamp(self, item, dev_ms, host_us):
        """-> [(item, t_us_epoch), ...], possibly empty, possibly several."""
        released = []

        ext = self._extend(dev_ms)
        if ext is None:                     # jump too big to be the wrap: reset
            self.reboots += 1
            released = self._flush()        # backlog belongs to the OLD run
            self._reset_state()
            ext = self._extend(dev_ms)

        if self._warm_until is None:
            self._warm_until = host_us + self._warmup_s * 1_000_000
        self._samples += 1

        warm = (host_us < self._warm_until
                or self._samples < self.WARMUP_MIN_SAMPLES)

        # Set the offset from the first record, then freeze it. With warmup_s > 0,
        # keep refining it while warming up, keeping the smallest seen: the
        # least-delayed line is the closest to the truth, because arrival delay
        # only ever adds.
        #
        # Freezing matters more than it looks. If the offset kept changing after
        # rows had been written, rows either side of a change would sit on
        # different origins and the gap between them would come out wrong -- which
        # is the one thing this class exists to protect.
        if warm or self._offset_us is None:
            offset = host_us - ext * 1000
            if self._offset_us is None or offset < self._offset_us:
                self._offset_us = offset

        self._pending.append((item, ext))
        return released if warm else released + self._flush()

    def announce_boot(self):
        """The firmware just restarted (BOOT record, or the setup() schema burst).

        Returns any records still parked from the previous run.
        """
        if self._prev_raw is not None:
            self.reboots += 1
        released = self._flush()
        self._reset_state()
        return released

    def close(self):
        """End of stream: release anything still parked."""
        return self._flush()

    @property
    def settled(self):
        return self._offset_us is not None and not self._pending

    @staticmethod
    def host_us(now=None):
        now = now or dt.datetime.now(dt.timezone.utc)
        return int(now.timestamp() * 1_000_000)

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
        # Say so, once: guessing silently is how a new SAMPLE ends up in the
        # events table and then missing from the dashboards with no clue why.
        if name not in _warned_unknown:
            _warned_unknown.add(name)
            print(f"[WARN] '{name}' has no #DEF and is not in KNOWN_KINDS; "
                  f"filing it as an EVENT. If it is a state sample it will not "
                  f"reach the `samples` table -- send DUMP or add it to "
                  f"KNOWN_KINDS.", file=sys.stderr)
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
    # Ask the rig to re-announce its schema. Opening the port usually resets the
    # board, so the startup "#DEF" burst normally arrives by itself -- but if we
    # attached to a rig that was already running, that burst is long gone, and
    # every message type added to the firmware since KNOWN_KINDS was last
    # updated would then be misfiled. control_panel.py already does this on
    # every (re)connect; without it, ingest.py's behaviour depends on the order
    # the board and this process happened to start in.
    dump_due = time.monotonic() + 2.0
    try:
        while True:
            if dump_due is not None and time.monotonic() >= dump_due:
                dump_due = None
                try:
                    ser.write(b"DUMP\n")
                except Exception:
                    pass          # a rig that cannot be written to still reads
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
    clock = DeviceClock()
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
                _, body = strip_rig_prefix(s)
                if body.startswith("#DEF"):
                    # The firmware dumps its whole schema from setup(), so a #DEF
                    # burst means "this rig just started". A host DUMP produces
                    # the same burst though, and re-anchoring for that would put
                    # a pointless seam in the mapping -- so this only ARMS the
                    # detector. The clock re-anchors on the next record whose
                    # device time is actually behind, i.e. once corroborated.
                    clock.arm_boot()
                recs = parse_hathaway(line)
            if not recs:
                continue

            # An explicit BOOT record needs no corroboration.
            if any(r["type"] == "BOOT" for r in recs):
                for (kind, row), epoch_us in clock.announce_boot():
                    row = row[:3] + (epoch_us,) + row[4:]
                    (samples if kind == "S" else events).append(row)

            if session_id is None:
                session_id = db.start_session(rig_id, args.note)
                print(f"[ingest] session {session_id} started (rig {rig_id})",
                      file=sys.stderr, flush=True)

            now = dt.datetime.now(dt.timezone.utc)
            host_ts = now.isoformat()
            host_us = DeviceClock.host_us(now)
            for rec in recs:
                seq += 1
                # The kind travels WITH the row: stamp() can hand back a backlog
                # released from earlier lines, and those must be routed by their
                # own kind, not by whatever record we happen to be holding now.
                item = (rec["kind"],
                        (session_id, rig_id, rec.get("seq", seq), None, host_ts,
                         rec["type"], rec["channel"], rec["value"]))
                t_us = rec.get("t_us")
                if t_us is None:
                    # No device timestamp on this line (PARAM acks and the like).
                    # Nothing to convert; t_us = 0 keeps it off any device-time
                    # axis, which is what we want.
                    stamped = [(item, 0)]
                else:
                    stamped = clock.stamp(item, t_us // 1000, host_us)
                for (kind, row), epoch_us in stamped:
                    row = row[:3] + (epoch_us,) + row[4:]
                    (samples if kind == "S" else events).append(row)

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
        for (kind, row), epoch_us in clock.close():   # release the warm-up buffer
            row = row[:3] + (epoch_us,) + row[4:]
            (samples if kind == "S" else events).append(row)
        flush()
        db.close()
        print(f"[ingest] done. rows written: {n_written}, "
              f"reboots: {clock.reboots}, millis() wraps: {clock.wraps}",
              file=sys.stderr)


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
