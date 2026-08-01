#!/usr/bin/env python3
"""Hathaway control panel.

One program that (a) ingests telemetry from ALL rigs into the database and
(b) sends SET/DUMP commands to any rig, with a local web UI.

Design
  * One supervised reader thread per serial port. If a port errors or unplugs,
    only that thread marks its rig disconnected and keeps retrying to reopen --
    the other ports keep logging. So one rig crashing never affects the others.
  * Reader threads never touch the DB directly: they push parsed records onto a
    single queue, and one DB-writer thread drains and batches it. That isolates
    the database from flaky ports (and vice versa).
  * Rigs are told apart by the "<RIG_ID>|" prefix the firmware puts on every
    line, so a single process handles the whole fleet and routes commands to the
    right port.
  * The web UI just polls /api/state (simple + robust); Grafana remains the tool
    for viewing telemetry history.

Requires: pyserial, fastapi, uvicorn        (pip install pyserial fastapi uvicorn)

Examples
  python control_panel.py --port COM4 --port COM5
  python control_panel.py --auto --dsn "host=localhost dbname=hathaway user=hathaway password=hathaway"
"""
import argparse
import datetime as dt
import json
import queue
import threading
import time
from collections import defaultdict, deque

import ingest  # reuse parse_hathaway() and the DB backends

# --------------------------------------------------------------------------- #
# Parameter values are validated ONLY by the firmware (single source of truth).
# The panel just sends "SET <name> <value>"; a rejected value comes back as a
# "#ERR ..." line and is shown in that rig's log. The set of tunable parameters
# is learned dynamically from the PARAM: acks each rig reports, so no parameter
# list/ranges are duplicated here.
# --------------------------------------------------------------------------- #
def _fmt_value(v):
    """Send integers without a trailing .0 (firmware sscanf handles either)."""
    f = float(v)
    return str(int(f)) if f == int(f) else str(f)


FRESH_SECONDS = 3.0        # a port is "responding" if a valid line arrived within this
WEIGHT_AVG_N = 10          # moving-average window for the displayed weight


# --------------------------------------------------------------------------- #
# Per-rig state (read by the web layer, written by reader/DB threads)
# --------------------------------------------------------------------------- #
class RigState:
    def __init__(self, rig_id, port):
        self.rig_id = rig_id
        self.port = port
        self.connected = False
        self.err = None
        self.params = {}                      # name -> value
        self.weight_buf = deque(maxlen=WEIGHT_AVG_N)  # raw weights for the moving avg
        self.counts = defaultdict(int)        # e.g. "LICK1", "REWARD2"
        self.dropped = 0
        self.last_seen = None                 # epoch seconds
        self.log = deque(maxlen=60)           # recent PARAM/#ERR/#... lines

    def weight_avg(self):
        if not self.weight_buf:
            return None
        return round(sum(self.weight_buf) / len(self.weight_buf), 2)

    def snapshot(self):
        return {
            "rig_id": self.rig_id,
            "port": self.port,
            "connected": self.connected,
            "err": self.err,
            "params": self.params,
            "weight": self.weight_avg(),      # moving average (display only)
            "counts": dict(self.counts),
            "dropped": self.dropped,
            "last_seen": self.last_seen,
            "log": list(self.log),
        }


# --------------------------------------------------------------------------- #
# Serial port: supervised reader thread + thread-safe writer
# --------------------------------------------------------------------------- #
class SerialLink:
    def __init__(self, port, baud, on_line, on_status, on_connect_cmd="DUMP"):
        self.port = port
        self.baud = baud
        self._on_line = on_line          # (port, line) -> None
        self._on_status = on_status      # (port, connected, err) -> None
        self._on_connect_cmd = on_connect_cmd  # sent after each successful open
        self._ser = None
        self._wlock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"reader-{port}",
                                        daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()

    def send(self, text):
        """Write a command line. Returns True if it went out."""
        with self._wlock:
            if self._ser is not None and self._ser.is_open:
                try:
                    self._ser.write((text.rstrip("\n") + "\n").encode("ascii"))
                    return True
                except Exception:
                    return False
        return False

    def _run(self):
        import serial  # pyserial; imported here so import errors surface per-port
        buf = b""
        while not self._stop.is_set():
            try:
                if self._ser is None:
                    self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
                    self._on_status(self.port, True, None)
                    if self._on_connect_cmd:       # refresh values on (re)connect
                        self.send(self._on_connect_cmd)
                chunk = self._ser.read(4096)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("ascii", "replace").strip()
                        if line:
                            self._on_line(self.port, line)
            except Exception as e:                     # port unplugged / IO error
                self._on_status(self.port, False, str(e))
                try:
                    if self._ser is not None:
                        self._ser.close()
                except Exception:
                    pass
                self._ser = None
                buf = b""
                self._stop.wait(2.0)                   # backoff, then reconnect
        # stopped: release the port so it is neither read nor retried
        try:
            if self._ser is not None:
                self._ser.close()
        except Exception:
            pass


# --------------------------------------------------------------------------- #
# Controller: demux by rig id, keep state, feed the DB, route commands
# --------------------------------------------------------------------------- #
class Controller:
    # Queue marker: "this rig announced its schema, so it may have restarted".
    BOOT_HINT = "#boot-hint"

    def __init__(self, db, note="", baud=115200):
        self.db = db
        self.note = note
        self.baud = baud
        self.rigs = {}                 # rig_id -> RigState
        self.link_by_port = {}         # port -> SerialLink
        self.link_by_rig = {}          # rig_id -> SerialLink
        self.port_rig = {}             # port -> last rig_id seen
        self.port_status = {}          # port -> {"connected":bool,"err":str}
        self.port_rx = {}              # port -> epoch of last VALID line received
        self.recq = queue.Queue()      # (rig_id, [records], host_ts)
        self.sessions = {}             # rig_id -> session_id
        self.seq = defaultdict(int)    # rig_id -> running seq
        # One device clock per rig: each board has its own millis() origin, its
        # own reboots and its own wrap. See DeviceClock in ingest.py.
        self.clocks = defaultdict(ingest.DeviceClock)
        self.lock = threading.Lock()
        self._stop = threading.Event()
        self._db_thread = threading.Thread(target=self._db_worker,
                                           name="db-writer", daemon=True)

    # -- lifecycle -------------------------------------------------------- #
    def start(self):
        self._db_thread.start()

    def open_port(self, port):
        """Open a port if needed and (re)request a parameter dump. Idempotent:
        selecting an already-open port just refreshes its values."""
        link = self.link_by_port.get(port)
        if link is None:
            link = SerialLink(port, self.baud, self.on_line, self.on_status,
                              on_connect_cmd="DUMP")
            self.link_by_port[port] = link
            link.start()
            return True, f"opening {port} (will refresh on connect)"
        link.send("DUMP")   # already open: refresh now
        return True, f"refreshing {port}"

    def close_port(self, port):
        """Stop and release a port entirely: its reader thread exits, so it is
        no longer read, logged, or retried in the background."""
        link = self.link_by_port.pop(port, None)
        if link is None:
            return False, f"{port} is not open"
        link.stop()
        rig_id = self.port_rig.pop(port, None)
        self.port_status.pop(port, None)
        self.port_rx.pop(port, None)
        if rig_id is not None and self.link_by_rig.get(rig_id) is link:
            del self.link_by_rig[rig_id]
        return True, f"closed {port}"

    def list_available_ports(self):
        names = set(self.link_by_port.keys())
        try:
            from serial.tools import list_ports
            for p in list_ports.comports():
                names.add(p.device)
        except Exception:
            pass
        return sorted(names)

    def shutdown(self):
        self._stop.set()
        for link in self.link_by_port.values():
            link.stop()
        # The DB thread has stopped, so drain the clocks here: each one may still
        # be holding a warm-up buffer, and those records are real data.
        self._db_thread.join(timeout=2.0)
        samples, events = [], []
        for rig_id, clock in self.clocks.items():
            for (kind, row), epoch_us in clock.close():
                row = row[:3] + (epoch_us,) + row[4:]
                (samples if kind == "S" else events).append(row)
        if samples or events:
            try:
                self.db.write(samples, events)
            except Exception as e:
                print("[db] final write error:", e)
        try:
            self.db.close()
        except Exception:
            pass

    # -- helpers ---------------------------------------------------------- #
    def _rig(self, rig_id, port):
        with self.lock:
            st = self.rigs.get(rig_id)
            if st is None:
                st = RigState(rig_id, port)
                self.rigs[rig_id] = st
            st.port = port
            return st

    # -- serial callbacks (reader threads) -------------------------------- #
    def on_status(self, port, connected, err):
        self.port_status[port] = {"connected": connected, "err": err}
        rig_id = self.port_rig.get(port)
        if rig_id is not None and rig_id in self.rigs:
            st = self.rigs[rig_id]
            st.connected = connected
            st.err = err

    def on_line(self, port, line):
        rig_part, sep, rest = line.partition("|")
        valid = False
        if sep:
            try:
                rig_id = int(rig_part)
                valid = True                 # a real "<int>|..." line from a rig
            except ValueError:
                rig_id = 0
        else:                       # no rig prefix (legacy / stray line)
            rig_id, rest = 0, line

        if valid:
            self.port_rx[port] = time.time()   # port is responding (DUMP reply / stream)
        self.port_rig[port] = rig_id
        self.link_by_rig[rig_id] = self.link_by_port.get(port)
        st = self._rig(rig_id, port)
        st.connected = True
        st.err = None
        st.last_seen = time.time()

        # Comments (#DEF, #ERR, #...) -> learn the schema, then show in the log.
        if rest.startswith("#"):
            ingest.parse_schema_line(rest)   # "#DEF <NAME>,<S|E>"
            if rest.startswith("#DEF"):
                # setup() dumps the schema, so this may be a restart -- but our
                # own DUMP looks identical, so only arm the detector. See
                # DeviceClock.arm_boot(). Sent through the queue rather than
                # touched directly: the clock belongs to the DB thread, and this
                # keeps it seeing everything in arrival order.
                self.recq.put((rig_id, self.BOOT_HINT, *self._stamps()))
            st.log.append(rest)
            return

        # Parameter affirmations -> update state and log to the DB.
        if rest.startswith("PARAM:"):
            # "PARAM:<name>,<value>,<device_ms>". The timestamp is what lets a
            # query say which settings were in force for a given trial. Firmware
            # older than that change sends only two fields, so it stays optional.
            name, _, tail = rest[len("PARAM:"):].partition(",")
            val, _, dev_ms = tail.partition(",")
            try:
                v = float(val)
            except ValueError:
                v = val
            st.params[name] = v
            st.log.append(rest)
            if isinstance(v, float):
                try:
                    t_us = int(dev_ms) * 1000
                except ValueError:
                    t_us = 0            # old firmware: no timestamp on the line
                self.recq.put((rig_id, [{"kind": "E", "type": "PARAM_" + name,
                                         "channel": 0, "value": v, "t_us": t_us}],
                               *self._stamps()))
            return

        # Normal telemetry -> parse with the existing ingest parser.
        recs = ingest.parse_hathaway(rest)
        if not recs:
            return
        for r in recs:
            if r["type"] == "WEIGHT":
                st.weight_buf.append(r["value"])   # feed the moving average
            if r["kind"] == "E":
                ch = r["channel"] or ""
                st.counts[f'{r["type"]}{ch}'] += 1
        self.recq.put((rig_id, recs, *self._stamps()))

    @staticmethod
    def _host_ts():
        return dt.datetime.now(dt.timezone.utc).isoformat()

    @staticmethod
    def _stamps():
        """(iso string, epoch microseconds) for the same instant of arrival."""
        now = dt.datetime.now(dt.timezone.utc)
        return now.isoformat(), ingest.DeviceClock.host_us(now)

    # -- DB writer thread ------------------------------------------------- #
    def _session(self, rig_id):
        sid = self.sessions.get(rig_id)
        if sid is None:
            sid = self.db.start_session(rig_id, self.note)
            self.sessions[rig_id] = sid
        return sid

    def _db_worker(self):
        samples, events = [], []
        last = time.monotonic()

        def collect(stamped):
            """Route stamped rows by the kind that travels with each one."""
            for (kind, row), epoch_us in stamped:
                row = row[:3] + (epoch_us,) + row[4:]
                (samples if kind == "S" else events).append(row)

        while not self._stop.is_set():
            try:
                rig_id, recs, host_ts, host_us = self.recq.get(timeout=0.5)
                clock = self.clocks[rig_id]

                if recs is self.BOOT_HINT:
                    clock.arm_boot()
                    continue

                sid = self._session(rig_id)
                if any(r["type"] == "BOOT" for r in recs):
                    collect(clock.announce_boot())   # explicit, no corroboration

                for r in recs:
                    self.seq[rig_id] += 1
                    item = (r["kind"],
                            (sid, rig_id, self.seq[rig_id], None, host_ts,
                             r["type"], r["channel"], r["value"]))
                    t_us = r.get("t_us")
                    if not t_us:
                        collect([(item, 0)])   # no device time on this line
                    else:
                        collect(clock.stamp(item, t_us // 1000, host_us))
            except queue.Empty:
                pass
            if (len(samples) + len(events) >= 200
                    or (time.monotonic() - last) >= 0.5):
                if samples or events:
                    try:
                        self.db.write(samples, events)
                    except Exception as e:
                        print("[db] write error:", e)
                    samples, events = [], []
                last = time.monotonic()

    # -- commands (called from the web thread) ---------------------------- #
    def send_set(self, rig_id, name, value):
        # No client-side validation: the firmware is the single safeguard and
        # replies with "#ERR ..." (shown in the rig log) if it rejects a value.
        link = self.link_by_rig.get(rig_id)
        if link is None:
            return False, f"rig {rig_id} not seen yet (no port mapped)"
        if not link.send(f"SET {name} {_fmt_value(value)}"):
            return False, f"rig {rig_id} port not open"
        return True, f"sent SET {name} {_fmt_value(value)} (rig will confirm or reject)"

    def send_dump(self, rig_id):
        link = self.link_by_rig.get(rig_id)
        if link is None:
            return False, f"rig {rig_id} not seen yet"
        if not link.send("DUMP"):
            return False, f"rig {rig_id} port not open"
        return True, f"sent DUMP to rig {rig_id}"

    def send_tare(self, rig_id):
        link = self.link_by_rig.get(rig_id)
        if link is None:
            return False, f"rig {rig_id} not seen yet"
        if not link.send("TARE"):
            return False, f"rig {rig_id} port not open"
        return True, f"sent TARE to rig {rig_id} (scale zeroing)"

    def snapshot(self):
        with self.lock:
            rigs = {str(rid): st.snapshot() for rid, st in self.rigs.items()}
        now = time.time()
        ports = {}
        for p in self.link_by_port:
            conn = self.port_status.get(p, {}).get("connected", False)
            fresh = (now - self.port_rx.get(p, 0)) < FRESH_SECONDS
            ports[p] = {"rig_id": self.port_rig.get(p),
                        "connected": conn,
                        "responding": conn and fresh,   # green only if a rig is replying
                        "err": self.port_status.get(p, {}).get("err")}
        return {"rigs": rigs, "ports": ports,
                "available_ports": self.list_available_ports(),
                "server_time": self._host_ts()}


# --------------------------------------------------------------------------- #
# Web layer (FastAPI). The page polls /api/state; commands POST to /api/rig/...
# --------------------------------------------------------------------------- #
CTRL = None   # set in main() before the server starts


def make_app():
    from fastapi import FastAPI
    from fastapi.responses import HTMLResponse, JSONResponse
    from pydantic import BaseModel

    app = FastAPI(title="Hathaway control panel")

    class SetBody(BaseModel):
        name: str
        value: float

    class OpenBody(BaseModel):
        port: str

    @app.get("/", response_class=HTMLResponse)
    def index():
        return HTML_PAGE

    @app.get("/api/state")
    def state():
        return JSONResponse(CTRL.snapshot())

    @app.post("/api/open")
    def open_port(body: OpenBody):
        ok, msg = CTRL.open_port(body.port)
        return JSONResponse({"ok": ok, "msg": msg}, status_code=200 if ok else 400)

    @app.post("/api/close")
    def close_port(body: OpenBody):
        ok, msg = CTRL.close_port(body.port)
        return JSONResponse({"ok": ok, "msg": msg}, status_code=200 if ok else 400)

    @app.post("/api/rig/{rig_id}/set")
    def set_param(rig_id: int, body: SetBody):
        ok, msg = CTRL.send_set(rig_id, body.name, body.value)
        return JSONResponse({"ok": ok, "msg": msg}, status_code=200 if ok else 400)

    @app.post("/api/rig/{rig_id}/dump")
    def dump(rig_id: int):
        ok, msg = CTRL.send_dump(rig_id)
        return JSONResponse({"ok": ok, "msg": msg}, status_code=200 if ok else 400)

    @app.post("/api/rig/{rig_id}/tare")
    def tare(rig_id: int):
        ok, msg = CTRL.send_tare(rig_id)
        return JSONResponse({"ok": ok, "msg": msg}, status_code=200 if ok else 400)

    @app.on_event("shutdown")
    def _shutdown():
        CTRL.shutdown()

    return app


HTML_PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>Hathaway control panel</title>
<style>
 *{box-sizing:border-box;scrollbar-width:none;-ms-overflow-style:none}
 *::-webkit-scrollbar{width:0;height:0;display:none}
 html,body{height:100%}
 body{margin:0;font-family:system-ui,Arial,sans-serif;color:#c7d0dc;font-size:13px;
      display:flex;flex-direction:column;background:#0d1320}
 .mono{font-family:ui-monospace,Menlo,monospace}
 header{position:relative;text-align:center;padding:12px;flex:none;
        border-bottom:1px solid rgba(109,207,142,.16)}
 header h1{margin:0;font-size:clamp(15px,2vw,21px);letter-spacing:5px;text-transform:uppercase;
        font-weight:600;color:#eaf3ee}
 header .sub{font-size:10px;letter-spacing:4px;color:#5f7480;margin-top:3px;
        font-family:ui-monospace,monospace;text-transform:uppercase}
 #clock{font-size:12px;color:#6f8590;margin-top:5px;font-family:ui-monospace,monospace}
 main{flex:1;display:grid;min-height:0;gap:12px;padding:12px;
      grid-template-columns:minmax(180px,215px) minmax(0,1fr);
      grid-template-rows:minmax(0,1fr)}
 /* connections keeps the styled panel */
 .panel{position:relative;background:#151c29;border:1px solid rgba(140,160,180,.13);
        border-radius:8px;padding:12px;overflow:auto}
 .panel::before,.panel::after{content:"";position:absolute;width:11px;height:11px;
        border:1px solid #6dcf8e;opacity:.55;pointer-events:none}
 .panel::before{left:-1px;top:-1px;border-right:0;border-bottom:0}
 .panel::after{right:-1px;bottom:-1px;border-left:0;border-top:0}
 .ttl{font-size:10px;letter-spacing:3px;text-transform:uppercase;color:#6f8b84;
      margin:0 0 10px;display:flex;align-items:center;gap:8px}
 .ttl::after{content:"";flex:1;height:1px;background:linear-gradient(90deg,rgba(109,207,142,.35),transparent)}
 .muted{color:#7d8794;font-size:11px}
 .conns{grid-column:1}
 .addrow{display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap}
 .prow{display:flex;align-items:center;gap:8px;padding:8px 4px;border-top:1px solid rgba(140,160,180,.08)}
 .prow:first-of-type{border-top:0}
 .dot{width:9px;height:9px;border-radius:50%;background:#e5675f;flex:none;display:inline-block;
      box-shadow:0 0 8px rgba(229,103,95,.8)}
 .dot.on{background:#6dcf8e;box-shadow:0 0 8px rgba(109,207,142,.8)}
 /* plain, simple control area */
 .control{grid-column:2;display:flex;flex-direction:column;padding:12px;overflow:auto}
 .chead{display:flex;align-items:center;gap:8px;margin-bottom:14px;flex-wrap:wrap;font-size:14px}
 .midrow{display:flex;gap:16px;align-items:stretch;flex-wrap:wrap}
 .params-col{flex:1 1 260px;min-width:0}
 .hero{flex:0 0 180px;display:flex;flex-direction:column;justify-content:center;
       text-align:center;border-left:1px solid rgba(140,160,180,.12);padding-left:16px}
 .hero .rig{font-size:14px;color:#8b93a2;letter-spacing:1px}
 .hero .w{font-size:clamp(24px,3vw,40px);font-weight:600;color:#e8eef4;line-height:1.15;margin-top:4px}
 .hero .wl{font-size:12px;color:#8b93a2;margin-top:2px}
 #ctllog{flex:1;min-height:60px;overflow:auto;font-family:ui-monospace,monospace;font-size:11px;
      color:#8fa0ac;white-space:pre-wrap;background:#0b111b;border:1px solid rgba(140,160,180,.12);
      border-radius:4px;padding:8px;margin-top:12px}
 table{width:100%;border-collapse:collapse;font-size:13px}
 th{text-align:left;color:#8b93a2;font-weight:400;font-size:12px;padding:4px}
 td{padding:4px}
 td.cur{font-family:ui-monospace,monospace;color:#6dcf8e;text-align:right;width:76px}
 input,select{background:#0b111b;color:#c7d0dc;border:1px solid rgba(140,160,180,.25);
      border-radius:4px;padding:4px}
 input{width:82px}
 button{background:rgba(109,207,142,.12);color:#6dcf8e;border:1px solid rgba(109,207,142,.45);
      border-radius:4px;padding:5px 11px;cursor:pointer;font-size:13px}
 button:hover{background:rgba(109,207,142,.22);color:#8fe0aa}
 button.x{background:transparent;border:0;color:#7d8794;font-size:15px;padding:0 4px}
 button.x:hover{color:#e5675f}
 .row{display:flex;justify-content:space-between;align-items:center;margin:8px 0}
 .ctl{display:flex;gap:8px;align-items:center}
 #msg{position:fixed;top:12px;right:12px;padding:8px 12px;border-radius:4px;display:none;
      font-size:12px;border:1px solid}
 #msg.ok{background:rgba(24,74,50,.94);border-color:#6dcf8e;color:#d6ffe6}
 #msg.err{background:rgba(80,26,24,.94);border-color:#e5675f;color:#ffdcd9}
 @media(max-width:820px){
   main{grid-template-columns:1fr;grid-template-rows:auto auto}
   .conns,.control{grid-column:1}.control{min-height:320px}
   .hero{border-left:0;padding-left:0}
 }
</style></head><body>
<header>
  <h1>Behavior Rigs Control Center</h1>
  <div id="clock"></div>
</header>
<main>
  <div class="panel conns">
    <div class="ttl">Connections</div>
    <div class="addrow"><select id="addsel"></select><button id="addbtn">Add</button></div>
    <div class="muted" style="margin-bottom:6px">
      <span class="dot on"></span> logging &nbsp; <span class="dot"></span> attempting</div>
    <div id="portlist"></div>
  </div>

  <div class="control">
    <div class="chead"><span>Rig <select id="rigsel"></select></span>
      <span class="muted">COM:</span> <b id="ctlport" class="mono">&mdash;</b></div>
    <div class="midrow">
      <div class="params-col">
        <div id="ctl" style="display:none">
          <div style="margin-bottom:8px"><button id="fetchbtn">Fetch</button></div>
          <table><tr><th>parameter</th><th style="text-align:right">current</th>
            <th>new</th><th></th></tr><tbody id="ctlparams"></tbody></table>
        </div>
        <div id="ctlnone" class="muted">Select a running rig to control it.</div>
      </div>
      <div class="hero">
        <div class="rig" id="heroRig">&mdash;</div>
        <div class="w" id="ctlweight">&mdash;</div>
        <div class="wl">weight</div>
        <div style="margin-top:10px"><button id="tarebtn">Tare</button></div>
      </div>
    </div>
    <div id="ctllog"></div>
  </div>
</main>

<div id="msg"></div>
<script>
function flash(ok,text){const m=document.getElementById('msg');m.textContent=text;
  m.className=ok?'ok':'err';m.style.display='block';setTimeout(function(){m.style.display='none';},2500);}
async function post(url,body){const o={method:'POST'};
  if(body){o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(body);}
  const r=await fetch(url,o);const j=await r.json();flash(j.ok,j.msg);return j;}
function addPort(){const p=document.getElementById('addsel').value;
  if(!p){flash(false,'pick a port to add');return;}post('/api/open',{port:p});}
function closePort(p){post('/api/close',{port:p});}
function fetchRig(rig){post('/api/rig/'+rig+'/dump');}
function tareRig(rig){post('/api/rig/'+rig+'/tare');}
async function send(rig,name,inp){const v=parseFloat(inp.value);
  if(isNaN(v)){flash(false,'enter a number first');return;}
  const j=await post('/api/rig/'+rig+'/set',{name:name,value:v});if(j.ok)inp.value='';}

function setOptions(sel,opts,selected){
  const key=opts.join(',');
  if(sel.dataset.opts!==key){
    sel.innerHTML='<option value="">&mdash; select &mdash;</option>'
      +opts.map(function(o){return '<option>'+o+'</option>';}).join('');
    sel.dataset.opts=key;
  }
  if(document.activeElement!==sel && sel.value!==(selected||''))sel.value=selected||'';
}

// ---- Connections sector: add / remove / status of every port ----
function renderConnections(state){
  const ports=state.ports||{};
  const open=Object.keys(ports).sort();
  const avail=(state.available_ports||[]).filter(function(p){return open.indexOf(p)<0;});
  setOptions(document.getElementById('addsel'),avail,'');
  const box=document.getElementById('portlist');
  box.innerHTML=open.length?open.map(function(p){
    const info=ports[p], on=info.responding;
    const rig=(info.rig_id!=null)?('Rig '+info.rig_id):'Rig ?';
    const title=on?'logging to database':(info.err||'attempting to connect');
    return '<div class="prow"><span class="dot'+(on?' on':'')+'" title="'+title+'"></span>'
      +'<b style="width:64px">'+rig+'</b><span class="muted" style="width:96px">'+p+'</span>'
      +'<span style="flex:1"></span><button class="x" data-close="'+p+'" title="remove">&times;</button></div>';
  }).join(''):'<div class="muted">No ports open. Add one above.</div>';
  box.querySelectorAll('[data-close]').forEach(function(b){
    b.onclick=function(){closePort(b.getAttribute('data-close'));};});
}

// ---- Control sector: one rig at a time, selected by Rig ID ----
let selectedRig="", ctlRows={}, ctlRigForRows=null, lastState=null;
function ensureCtlRow(rig,name){
  if(ctlRows[name])return;
  const tr=document.createElement('tr');
  tr.innerHTML='<td>'+name+'</td><td class="cur" data-cur>&mdash;</td>'
    +'<td><input placeholder="value"></td><td><button>Set</button></td>';
  const inp=tr.querySelector('input');
  tr.querySelector('button').onclick=function(){send(rig,name,inp);};
  document.getElementById('ctlparams').appendChild(tr);
  ctlRows[name]=tr;
}
function renderControl(state){
  const ports=state.ports||{};
  const rigPort={};                         // rig_id -> COM port (running ports only)
  Object.keys(ports).forEach(function(p){const r=ports[p].rig_id;if(r!=null)rigPort[r]=p;});
  const rigIds=Object.keys(rigPort).map(Number).sort(function(a,b){return a-b;});
  setOptions(document.getElementById('rigsel'),rigIds.map(String),selectedRig);
  selectedRig=document.getElementById('rigsel').value;
  const has=selectedRig!=="" && rigIds.indexOf(Number(selectedRig))>=0;
  document.getElementById('ctl').style.display=has?'block':'none';
  document.getElementById('ctlnone').style.display=has?'none':'block';
  const hero=document.getElementById('heroRig'), w=document.getElementById('ctlweight'),
        cp=document.getElementById('ctlport'), lg=document.getElementById('ctllog');
  if(!has){cp.innerHTML='&mdash;';hero.innerHTML='&mdash;';w.innerHTML='&mdash;';lg.textContent='';return;}
  const rig=Number(selectedRig);
  cp.textContent=rigPort[rig];
  hero.textContent='RIG '+rig;
  if(ctlRigForRows!==rig){        // switched rigs: rebuild the parameter rows
    document.getElementById('ctlparams').innerHTML='';ctlRows={};ctlRigForRows=rig;}
  const r=state.rigs[rig]||{};
  w.textContent=(r.weight!=null)?(r.weight+' g'):'\\u2014';
  const params=r.params||{};
  Object.keys(params).forEach(function(name){
    ensureCtlRow(rig,name);
    ctlRows[name].querySelector('[data-cur]').textContent=params[name];});
  lg.textContent=(r.log||[]).slice(-20).reverse().join('\\n')||'(no messages)';
}

function update(state){
  lastState=state;
  document.getElementById('clock').textContent=new Date().toLocaleTimeString();
  renderConnections(state);
  renderControl(state);
}
document.getElementById('addbtn').onclick=addPort;
document.getElementById('rigsel').onchange=function(){selectedRig=this.value;
  if(lastState)renderControl(lastState);};
document.getElementById('tarebtn').onclick=function(){if(selectedRig)tareRig(Number(selectedRig));};
document.getElementById('fetchbtn').onclick=function(){if(selectedRig)fetchRig(Number(selectedRig));};
async function tick(){try{const r=await fetch('/api/state');update(await r.json());}catch(e){}}
setInterval(tick,1000);tick();
</script></body></html>"""


# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #
def _make_db(args):
    if args.db == "print":
        return ingest.PrintDB()
    if args.db == "postgres":
        return ingest.PostgresDB(args.dsn)
    raise ValueError(args.db)


def _discover_ports():
    from serial.tools import list_ports
    return [p.device for p in list_ports.comports()]


def main():
    global CTRL
    p = argparse.ArgumentParser(description="Hathaway ingest + control panel")
    p.add_argument("--port", action="append", default=[],
                   help="serial port; repeat for several rigs (e.g. --port COM4 --port COM5)")
    p.add_argument("--auto", action="store_true",
                   help="auto-detect and use all serial ports")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--db", choices=["postgres", "print"], default="postgres")
    p.add_argument("--dsn", default="host=localhost dbname=hathaway "
                                    "user=hathaway password=hathaway")
    p.add_argument("--note", default="")
    p.add_argument("--host", default="127.0.0.1", help="web UI bind address")
    p.add_argument("--http-port", type=int, default=8000, help="web UI port")
    args = p.parse_args()

    ports = list(args.port)
    if args.auto:
        ports = sorted(set(ports) | set(_discover_ports()))
    # Ports are optional now: you can also pick them per-tab in the web UI.

    db = _make_db(args)
    CTRL = Controller(db, note=args.note, baud=args.baud)
    CTRL.start()
    for port in ports:
        CTRL.open_port(port)
        print(f"[panel] opened {port}")

    import uvicorn
    print(f"[panel] web UI at http://{args.host}:{args.http_port}")
    uvicorn.run(make_app(), host=args.host, port=args.http_port, log_level="warning")


if __name__ == "__main__":
    main()
