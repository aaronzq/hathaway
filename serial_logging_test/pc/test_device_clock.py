#!/usr/bin/env python3
"""Tests for DeviceClock and the device-time path end to end.

    python test_device_clock.py            # logic only, no database
    python test_device_clock.py --pg       # also run against a real PostgreSQL

The logic tests need nothing but the standard library. The --pg tests spin up a
throwaway PostgreSQL (pip install pgserver psycopg2-binary), apply schema.sql,
push records through the real pipeline and check what the dashboards would read.

Scenarios covered, all of them things that actually go wrong on a rig:
  * a congested boot burst, which is the worst possible anchor
  * the 49.7-day millis() wrap
  * a reboot mid-session, with and without the #DEF hint
  * a logger that was disconnected for a while (must NOT look like a reboot)
"""
import argparse
import datetime as dt
import math
import sys
import time

import ingest
from ingest import DeviceClock

READ_MS = 200          # ser.read(4096) timeout=0.2 -> a batch every ~200 ms
US = 1_000_000

_fails = []


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        _fails.append(what)


def approx(a, b, tol):
    return abs(a - b) <= tol


# --------------------------------------------------------------------------- #
# A fake rig + a fake serial link with the real batching behaviour
# --------------------------------------------------------------------------- #
class FakeLink:
    """Device events in, (dev_ms, host_us) pairs out, batched like pyserial.

    Every line in a 200 ms read window is handed over at the same instant, which
    is the whole reason host timestamps cannot be trusted.
    """

    def __init__(self, t0_host_us=1_800_000_000 * US):
        self.t0 = t0_host_us
        self.true_offset = None

    def deliver(self, dev_ms, boot_ms=0, extra_delay_ms=0):
        """host_us the PC would record for an event at device time dev_ms."""
        # true wall clock of the event, then rounded up to the read boundary
        true_us = self.t0 + (dev_ms - boot_ms) * 1000
        batch = math.ceil((true_us + extra_delay_ms * 1000) / (READ_MS * 1000))
        return batch * READ_MS * 1000


def drive(clock, pairs, hint_before=()):
    """Feed (dev_ms, host_us) pairs; return {label: t_us} for everything written."""
    out = {}
    for i, (label, dev_ms, host_us) in enumerate(pairs):
        if i in hint_before:
            for item, t in clock.announce_boot():
                out[item] = t
        for item, t in clock.stamp(label, dev_ms, host_us):
            out[item] = t
    for item, t in clock.close():
        out[item] = t
    return out


# --------------------------------------------------------------------------- #
# 1. the anchor
# --------------------------------------------------------------------------- #
def test_warmup_beats_first_line_anchoring():
    print("anchor: the optional warm-up beats first-line anchoring (off by default)")
    link = FakeLink()
    # The boot burst: 21 schema/param lines shoved out at once, so the first
    # timestamped record arrives maximally late. Then normal traffic.
    pairs = [("burst", 5000, link.deliver(5000, 5000, extra_delay_ms=180))]
    pairs += [(f"w{k}", 5000 + k * 100, link.deliver(5000 + k * 100, 5000))
              for k in range(1, 200)]

    clock = DeviceClock(warmup_s=10.0)
    got = drive(clock, pairs)

    # Truth: device 5000 ms == link.t0. Recover the origin the clock chose.
    origin = got["w1"] - 100 * 1000
    err_ms = (origin - link.t0) / 1000.0
    check(abs(err_ms) <= READ_MS, f"origin within one read cycle (off by {err_ms:.0f} ms)")

    naive = pairs[0][2] - 0                       # what "anchor on line 1" gives
    naive_err = (naive - link.t0) / 1000.0
    check(abs(err_ms) < abs(naive_err),
          f"better than first-line anchoring ({abs(err_ms):.0f} ms vs {abs(naive_err):.0f} ms)")
    check(len(got) == len(pairs), "every record eventually written, none dropped")


def test_intervals_are_exact():
    print("intervals: device gaps survive the host's batching")
    link = FakeLink()
    pairs = []
    for i in range(30):                            # a cue pair per trial
        t = 5000 + i * 3000 + (i * 37 % 190)       # vary phase in the read cycle
        pairs.append((f"on{i}", t, link.deliver(t, 5000)))
        pairs.append((f"off{i}", t + 100, link.deliver(t + 100, 5000)))
    got = drive(DeviceClock(), pairs)          # shipped default: no warm-up

    gaps = sorted({(got[f"off{i}"] - got[f"on{i}"]) // 1000 for i in range(30)})
    check(gaps == [100], f"every ON->OFF gap is exactly 100 ms (saw {gaps})")

    host_gaps = sorted({(link.deliver(5000 + i*3000 + (i*37 % 190) + 100, 5000)
                         - link.deliver(5000 + i*3000 + (i*37 % 190), 5000)) // 1000
                        for i in range(30)})
    check(len(host_gaps) > 1,
          f"...while the host clock gives {host_gaps} ms for the same events")


# --------------------------------------------------------------------------- #
# 2. the 49.7-day wrap
# --------------------------------------------------------------------------- #
def test_millis_wrap():
    print("wrap: millis() rolls over at 2**32 ms without a seam")
    WRAP = 1 << 32
    link = FakeLink()
    boot = WRAP - 1500                             # 1.5 s short of the rollover
    pairs, raw = [], boot
    labels = []
    for k in range(30):                            # step 100 ms straight through
        raw = (boot + k * 100) % WRAP
        host = link.deliver(boot + k * 100, boot)
        pairs.append((f"p{k}", raw, host))
        labels.append(f"p{k}")
    clock = DeviceClock(warmup_s=0.5)
    got = drive(clock, pairs)

    check(clock.wraps == 1, f"one wrap detected (got {clock.wraps})")
    check(clock.reboots == 0, f"and NOT mistaken for a reboot (got {clock.reboots})")
    steps = sorted({(got[labels[k+1]] - got[labels[k]]) // 1000
                    for k in range(len(labels) - 1)})
    check(steps == [100], f"time steps 100 ms across the boundary (saw {steps})")
    check(all(got[labels[k+1]] > got[labels[k]] for k in range(len(labels)-1)),
          "device time stays monotonic across the wrap")


# --------------------------------------------------------------------------- #
# 3. reboots
# --------------------------------------------------------------------------- #
def test_reboot_detected_by_jump():
    print("reboot: millis() restarting from 0 is caught without any hint")
    link = FakeLink()
    pairs = [(f"a{k}", 300000 + k * 100, link.deliver(300000 + k * 100, 300000))
             for k in range(20)]                   # 5 minutes of uptime
    base = link.deliver(300000 + 20 * 100, 300000) + 3 * US   # 3 s of downtime
    pairs += [(f"b{k}", 250 + k * 100, base + k * 100 * 1000) for k in range(20)]

    clock = DeviceClock(warmup_s=0.5)
    got = drive(clock, pairs)
    check(clock.reboots == 1, f"exactly one reboot (got {clock.reboots})")
    check(len(got) == 40, f"all 40 records written (got {len(got)})")
    check(got["b0"] > got["a19"],
          "post-reboot rows land AFTER pre-reboot rows, not 5 minutes earlier")
    seam_s = (got["b0"] - got["a19"]) / US
    check(0 < seam_s < 10, f"seam is a few seconds, not minutes ({seam_s:.1f} s)")


def test_reboot_with_def_hint():
    print("reboot: the #DEF burst catches the one reset the size test cannot")
    # A reset from a short uptime is easy: millis() goes from (say) 650 back to
    # 120, and the wrapped delta is then ~2**32, wildly over the threshold.
    #
    # The hard case is a reset that happens just *before* the 49.7-day wrap. Then
    # the wrapped delta is tiny -- 1.3 s here -- and looks exactly like ordinary
    # forward progress, so the size test cannot see it. Meanwhile the board was
    # really away for 30 s, so every later row would be placed ~29 s too early.
    WRAP = 1 << 32
    US_ = 1_000_000
    pre_uptime = WRAP - 1000          # 1 s short of rolling over
    downtime_s = 30

    host0 = 1_800_000_000 * US_
    pairs = [(f"a{k}", pre_uptime + k * 100, host0 + k * 100 * 1000) for k in range(10)]
    resume_host = host0 + 900 * 1000 + downtime_s * US_
    pairs += [(f"b{k}", 300 + k * 100, resume_host + k * 100 * 1000) for k in range(10)]

    results = {}
    for hinted in (False, True):
        clock = DeviceClock(warmup_s=0.3)
        out = {}
        for i, (label, dev, host) in enumerate(pairs):
            if hinted and i == 10:
                for item, t in clock.announce_boot():
                    out[item] = t
                clock.arm_boot()
            for item, t in clock.stamp(label, dev, host):
                out[item] = t
        for item, t in clock.close():
            out[item] = t
        results[hinted] = (clock.reboots, (out["b0"] - out["a9"]) / US_)

    unhinted_reboots, unhinted_seam = results[False]
    hinted_reboots, hinted_seam = results[True]

    check(unhinted_reboots == 0,
          f"without the hint the reset is invisible (reboots={unhinted_reboots})")
    check(unhinted_seam < 2,
          f"...and the {downtime_s} s outage collapses to {unhinted_seam:.1f} s of "
          f"device time, putting every later row too early")
    check(hinted_reboots == 1, f"with the hint it is caught (reboots={hinted_reboots})")
    check(approx(hinted_seam, downtime_s, 2),
          f"...and the real {downtime_s} s gap is restored ({hinted_seam:.1f} s)")


def test_long_gap_is_not_a_reboot():
    print("gap: a logger away for 20 minutes must not look like a reboot")
    link = FakeLink()
    pairs = [(f"a{k}", 60000 + k * 100, link.deliver(60000 + k * 100, 60000))
             for k in range(10)]
    # 20 minutes later the device is far ahead, but never restarted.
    t = 60000 + 20 * 60 * 1000
    pairs += [(f"b{k}", t + k * 100,
               link.deliver(t + k * 100, 60000)) for k in range(10)]
    clock = DeviceClock(warmup_s=0.5)
    got = drive(clock, pairs)
    check(clock.reboots == 0, f"no reboot inferred (got {clock.reboots})")
    span_min = (got["b9"] - got["a0"]) / US / 60
    check(approx(span_min, 20, 1), f"the 20-minute gap is preserved ({span_min:.1f} min)")


def test_nothing_is_lost_on_reboot_during_warmup():
    print("reboot: a reset *during* warm-up still writes the buffered rows")
    link = FakeLink()
    pairs = [(f"a{k}", 1000 + k * 50, link.deliver(1000 + k * 50, 1000)) for k in range(6)]
    base = link.deliver(1000 + 6 * 50, 1000) + US
    pairs += [(f"b{k}", 200 + k * 50, base + k * 50 * 1000) for k in range(30)]
    clock = DeviceClock(warmup_s=1.0)
    got = drive(clock, pairs)
    check(len(got) == len(pairs), f"all {len(pairs)} records written (got {len(got)})")


# --------------------------------------------------------------------------- #
# 4. end to end against a real database
# --------------------------------------------------------------------------- #
def test_postgres():
    print("postgres: full pipeline, then the queries the dashboards run")
    import pathlib, re, json, tempfile
    import pgserver, psycopg2
    from psycopg2.extras import execute_values

    here = pathlib.Path(__file__).parent
    srv = pgserver.get_server(pathlib.Path(tempfile.gettempdir()) / "hathaway_test_pg")
    conn = psycopg2.connect(srv.get_uri()); conn.autocommit = True
    cur = conn.cursor()
    cur.execute((here / "schema.sql").read_text())
    cur.execute("TRUNCATE samples, events; DELETE FROM sessions;")
    cur.execute("INSERT INTO sessions(rig_id,note) VALUES (1,'clock test') RETURNING session_id")
    sid = cur.fetchone()[0]
    print("   schema.sql applied, views:", end=" ")
    cur.execute("SELECT table_name FROM information_schema.views "
                "WHERE table_schema='public' ORDER BY 1")
    print([r[0] for r in cur.fetchall()])

    # --- push a session through the real DeviceClock ----------------------
    now_us = int(dt.datetime.now(dt.timezone.utc).timestamp() * US)
    link = FakeLink(t0_host_us=now_us - 120 * US)
    clock = DeviceClock()
    BOOT = 5000
    rows, seq, n_trials = [], 0, 30

    def push(kind, typ, ch, val, dev_ms, delay=0):
        nonlocal seq
        seq += 1
        host_us = link.deliver(dev_ms, BOOT, extra_delay_ms=delay)
        host_iso = dt.datetime.fromtimestamp(host_us / US, dt.timezone.utc)
        item = (kind, (sid, 1, seq, None, host_iso, typ, ch, val))
        for (k, row), t_us in clock.stamp(item, dev_ms, host_us):
            rows.append((k, row[:3] + (t_us,) + row[4:]))

    # a congested boot burst first, exactly as setup() produces
    for i in range(21):
        push("S", "TASK", 2, 2, BOOT, delay=180)
    for k in range(1000):
        push("S", "WEIGHT", 1, 20 + 0.3 * math.sin(k / 9), BOOT + k * 100)
    for i in range(n_trials):
        t = BOOT + 800 + i * 3000 + (i * 37 % 190)
        push("E", "LICK", 2, 1, t)
        push("S", "TONE", 1, 6000, t)
        push("S", "TONE", 1, 0, t + 100)
        push("E", "REWARD", 2, i + 1, t + 100)
    for (k, row), t_us in clock.close():
        rows.append((k, row[:3] + (t_us,) + row[4:]))

    S = [r for k, r in rows if k == "S"]
    E = [r for k, r in rows if k == "E"]
    execute_values(cur, "INSERT INTO samples(session_id,rig_id,seq,t_us,host_ts,type,channel,value) VALUES %s", S)
    execute_values(cur, "INSERT INTO events (session_id,rig_id,seq,t_us,host_ts,type,channel,value) VALUES %s", E)
    check(len(S) + len(E) == seq, f"all {seq} records reached the DB (got {len(S)+len(E)})")

    # --- the measurement that matters -------------------------------------
    for col, tbl in (("host_ts", "samples"), ("dev_ts", "samples_dev")):
        cur.execute(f"""
          SELECT round(extract(epoch from (f.{col}-o.{col}))*1000)::int g, count(*)
          FROM {tbl} o JOIN LATERAL (
            SELECT {col} FROM {tbl} f WHERE f.type='TONE' AND f.value=0
             AND f.session_id=o.session_id AND f.t_us>o.t_us
             ORDER BY f.t_us LIMIT 1) f ON true
          WHERE o.type='TONE' AND o.value>0 GROUP BY 1 ORDER BY 1""")
        got = cur.fetchall()
        print(f"   cue gap on {col:8s}: " + ", ".join(f"{g}ms x{n}" for g, n in got))
        if col == "dev_ts":
            check(got == [(100, n_trials)], "dev_ts gives exactly 100 ms, every trial")
        else:
            check(len(got) > 1, "host_ts is inconsistent, as expected")

    cur.execute("SELECT min(dev_ts), max(dev_ts) FROM samples_dev")
    lo, hi = cur.fetchone()
    check(lo.year == dt.datetime.now().year, f"dev_ts sits on the wall clock ({lo})")

    # --- every dashboard query still parses and returns rows --------------
    dash = json.loads((here / "grafana/provisioning/dashboards/hathaway.json").read_text())
    frm = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(minutes=10)).isoformat()
    to = (dt.datetime.now(dt.timezone.utc) + dt.timedelta(minutes=10)).isoformat()

    def expand(sql):
        sql = re.sub(r"\$__timeFilter\(\s*([A-Za-z_.]+)\s*\)",
                     lambda m: f"{m.group(1)} >= '{frm}'::timestamptz "
                               f"AND {m.group(1)} <= '{to}'::timestamptz", sql)
        return sql.replace("$__timeFrom()", f"'{frm}'").replace("$__timeTo()", f"'{to}'")

    bad = 0
    for p in dash["panels"]:
        for t in p["targets"]:
            try:
                cur.execute(expand(t["rawSql"]))
                cur.fetchall()
            except Exception as ex:
                bad += 1
                print(f"   panel {p['id']} [{t['refId']}] FAILED: "
                      f"{str(ex).strip().splitlines()[0]}")
    check(bad == 0, f"all dashboard queries run ({bad} failures)")


# --------------------------------------------------------------------------- #
# 5. control_panel, through its real threads
# --------------------------------------------------------------------------- #
def test_control_panel():
    print("control_panel: reader thread -> queue -> DB thread")
    import control_panel

    class CaptureDB(ingest.BaseDB):
        def __init__(self): self.rows = []
        def start_session(self, rig_id, note): return 1
        def write(self, samples, events): self.rows += list(samples) + list(events)
        def close(self): pass

    db = CaptureDB()
    c = control_panel.Controller(db, note="clock test")
    c.start()

    for line in ("#DEF WEIGHT,S", "#DEF TONE,S", "#DEF LICK,E", "#DEF REWARD,E"):
        c.on_line("COM_TEST", "1|" + line)
    for _ in range(21):                            # the congested boot burst
        c.on_line("COM_TEST", "1|TASK:2,2,5000")

    lines, n_trials = [], 12
    for k in range(60):
        lines.append((5000 + k * 100, f"1|WEIGHT:1,{20 + (k % 7) * 0.1:.2f},{5000 + k*100}"))
    for i in range(n_trials):
        t = 6000 + i * 400
        lines += [(t, f"1|LICK:2,1,{t}"), (t, f"1|TONE:1,6000,{t}"),
                  (t + 100, f"1|TONE:1,0,{t + 100}"),
                  (t + 100, f"1|REWARD:2,{i + 1},{t + 100}")]
    for _, line in sorted(lines, key=lambda x: x[0]):
        c.on_line("COM_TEST", line)
        time.sleep(0.004)

    time.sleep(1.5)
    c.shutdown()            # must drain each clock's warm-up buffer
    time.sleep(0.3)

    tone = [r for r in db.rows if r[5] == "TONE"]
    on = sorted(r[3] for r in tone if r[7] > 0)
    off = sorted(r[3] for r in tone if r[7] == 0)
    gaps = sorted({(b - a) // 1000 for a, b in zip(on, off)})
    clock = c.clocks[1]

    # A REWARD line yields two records: the event and a cumulative
    # REWARD_COUNT sample (see COUNTED_EVENTS in ingest.py).
    expected = 21 + len(lines) + n_trials
    check(len(db.rows) == expected,
          f"every record written and none duplicated ({len(db.rows)} of {expected})")
    check(len(on) == len(off) == n_trials, f"{len(on)} cue onsets, {len(off)} offsets")
    check(gaps == [100], f"cue gaps exactly 100 ms (saw {gaps})")
    check(clock.reboots == 0, f"no spurious reboot (got {clock.reboots})")
    check(dt.datetime.fromtimestamp(on[0] / US, dt.timezone.utc).year >= 2024,
          "t_us holds epoch microseconds, not device uptime")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pg", action="store_true", help="also test against real PostgreSQL")
    args = ap.parse_args()

    test_warmup_beats_first_line_anchoring()
    test_intervals_are_exact()
    test_millis_wrap()
    test_reboot_detected_by_jump()
    test_reboot_with_def_hint()
    test_long_gap_is_not_a_reboot()
    test_nothing_is_lost_on_reboot_during_warmup()
    test_control_panel()
    if args.pg:
        test_postgres()

    print()
    if _fails:
        print(f"FAIL ({len(_fails)})")
        for f in _fails:
            print("  -", f)
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
