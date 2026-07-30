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


# --------------------------------------------------------------------------- #
# Controller: demux by rig id, keep state, feed the DB, route commands
# --------------------------------------------------------------------------- #
class Controller:
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

        # Comments (#ERR, #...) -> just show in the log.
        if rest.startswith("#"):
            st.log.append(rest)
            return

        # Parameter affirmations -> update state and log to the DB.
        if rest.startswith("PARAM:"):
            name, _, val = rest[len("PARAM:"):].partition(",")
            try:
                v = float(val)
            except ValueError:
                v = val
            st.params[name] = v
            st.log.append(rest)
            if isinstance(v, float):
                self.recq.put((rig_id, [{"kind": "E", "type": "PARAM_" + name,
                                         "channel": 0, "value": v, "t_us": 0}],
                               self._host_ts()))
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
        self.recq.put((rig_id, recs, self._host_ts()))

    @staticmethod
    def _host_ts():
        return dt.datetime.now(dt.timezone.utc).isoformat()

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
        while not self._stop.is_set():
            try:
                rig_id, recs, host_ts = self.recq.get(timeout=0.5)
                sid = self._session(rig_id)
                for r in recs:
                    self.seq[rig_id] += 1
                    t_us = r.get("t_us") or 0
                    row = (sid, rig_id, self.seq[rig_id], t_us, host_ts,
                           r["type"], r["channel"], r["value"])
                    (samples if r["kind"] == "S" else events).append(row)
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
<html><head><meta charset="utf-8"><title>Rig Control Panel</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:16px;background:#0f1115;color:#e6e6e6}
 h1{font-size:18px;margin:0 0 4px}
 .sub{color:#8b93a2;font-size:12px;margin:0 0 12px}
 .bar{margin:0 0 14px}
 .rigs{display:flex;flex-wrap:wrap;gap:14px}
 .card{background:#171a21;border:1px solid #262b36;border-radius:10px;padding:14px;width:380px}
 .hd{display:flex;align-items:center;gap:8px;margin-bottom:10px}
 .hd b{font-size:15px}
 .dot{width:12px;height:12px;border-radius:50%;background:#c0392b;flex:none}
 .dot.on{background:#27ae60}
 .muted{color:#8b93a2;font-size:12px}
 table{width:100%;border-collapse:collapse;font-size:13px}
 th{text-align:left;color:#8b93a2;font-weight:normal;font-size:11px;padding:2px 4px}
 td{padding:3px 4px}
 td.cur{font-family:ui-monospace,monospace;color:#8fd3a0;text-align:right;width:70px}
 input{width:78px;background:#0f1115;color:#e6e6e6;border:1px solid #333;border-radius:5px;padding:3px}
 select{background:#0f1115;color:#e6e6e6;border:1px solid #333;border-radius:5px;padding:3px}
 button{background:#2d6cdf;color:#fff;border:0;border-radius:5px;padding:4px 9px;cursor:pointer}
 button.sec{background:#3a4150}
 button.x{background:transparent;color:#8b93a2;font-size:16px;padding:0 4px}
 .log{margin-top:8px;max-height:110px;overflow:auto;background:#0f1115;border:1px solid #262b36;
      border-radius:6px;padding:6px;font-family:ui-monospace,monospace;font-size:11px;color:#9fb3c8;
      white-space:pre-wrap}
 .row{display:flex;justify-content:space-between;align-items:center;font-size:13px;margin:5px 0}
 .ctl{display:flex;gap:6px;align-items:center}
 #msg{position:fixed;top:10px;right:10px;padding:8px 12px;border-radius:6px;display:none}
 #msg.ok{background:#1e5631}#msg.err{background:#7a2020}
</style></head><body>
<h1>Hathaway control panel <span id="clock" class="muted"></span></h1>
<p class="sub"> Add a tab and pick its COM port. </p>
<div class="bar"><button id="addtab">+ Add rig tab</button></div>
<div id="rigs" class="rigs"></div>
<div id="msg"></div>
<script>
function flash(ok,text){const m=document.getElementById('msg');m.textContent=text;
  m.className=ok?'ok':'err';m.style.display='block';setTimeout(function(){m.style.display='none';},2500);}
async function send(rig,name,inp){
  const v=parseFloat(inp.value);
  if(isNaN(v)){flash(false,'enter a number first');return;}
  const r=await fetch('/api/rig/'+rig+'/set',{method:'POST',
    headers:{'Content-Type':'application/json'},body:JSON.stringify({name:name,value:v})});
  const j=await r.json();flash(j.ok,j.msg);if(j.ok)inp.value='';}
async function openPort(port){
  const r=await fetch('/api/open',{method:'POST',
    headers:{'Content-Type':'application/json'},body:JSON.stringify({port:port})});
  const j=await r.json();flash(j.ok,j.msg);}
async function tareRig(rig){
  const r=await fetch('/api/rig/'+rig+'/tare',{method:'POST'});
  const j=await r.json();flash(j.ok,j.msg);}

let TABS=[];            // [{id, port}]
let nextId=1, inited=false;
const els={};           // tabId -> {el, rows, sel}
function addTab(port){TABS.push({id:nextId, port:port||""});return nextId++;}
function removeTab(id){TABS=TABS.filter(function(t){return t.id!==id;});
  if(els[id]){els[id].el.remove();delete els[id];}}

function buildTab(tab){
  const el=document.createElement('div');el.className='card';
  el.innerHTML=
    '<div class="hd"><span class="dot" data-dot></span><b data-label>Rig &mdash;</b>'
    +'<span style="flex:1"></span><button class="x" data-close title="remove tab">&times;</button></div>'
    +'<div class="row"><span class="muted">COM port</span>'
    +'<span class="ctl"><select data-sel></select>'
    +'<button data-refresh>Fetch values from rig</button></span></div>'
    +'<div class="row"><span class="muted">weight</span>'
    +'<span class="ctl"><span data-weight>&mdash;</span>'
    +'<button data-tare>Tare scale</button></span></div>'
    +'<table><tr><th>parameter</th><th style="text-align:right">current</th>'
    +'<th>new</th><th></th></tr><tbody data-params></tbody></table>'
    +'<div class="log" data-log></div>';
  const rec={el:el,rows:{},sel:el.querySelector('[data-sel]'),rigId:null};
  rec.sel.onchange=function(){tab.port=rec.sel.value;if(tab.port)openPort(tab.port);};
  el.querySelector('[data-refresh]').onclick=function(){if(tab.port)openPort(tab.port);
    else flash(false,'pick a COM port first');};
  el.querySelector('[data-tare]').onclick=function(){
    if(rec.rigId!=null)tareRig(rec.rigId);else flash(false,'no rig on this port yet');};
  el.querySelector('[data-close]').onclick=function(){removeTab(tab.id);};
  document.getElementById('rigs').appendChild(el);
  els[tab.id]=rec;
  return rec;
}
function setOptions(sel,ports,selected){
  const key=ports.join(',');
  if(sel.dataset.opts!==key){
    sel.innerHTML='<option value="">&mdash; select &mdash;</option>'
      +ports.map(function(p){return '<option>'+p+'</option>';}).join('');
    sel.dataset.opts=key;
  }
  if(document.activeElement!==sel && sel.value!==(selected||''))sel.value=selected||'';
}
function ensureRow(rec,rig,name){
  if(rec.rows[name])return;
  const tr=document.createElement('tr');
  tr.innerHTML='<td>'+name+'</td><td class="cur" data-cur>&mdash;</td>'
    +'<td><input placeholder="value"></td><td><button>Set</button></td>';
  const inp=tr.querySelector('input');
  tr.querySelector('button').onclick=function(){send(rig,name,inp);};
  rec.el.querySelector('[data-params]').appendChild(tr);
  rec.rows[name]=tr;
}
function update(state){
  document.getElementById('clock').textContent=new Date().toLocaleTimeString();
  if(!inited){inited=true;                        // first load: one tab per open port
    const open=Object.keys(state.ports||{});
    if(open.length)open.forEach(function(p){addTab(p);});else addTab("");}
  const avail=state.available_ports||[];
  TABS.forEach(function(tab){
    const rec=els[tab.id]||buildTab(tab);
    setOptions(rec.sel,avail,tab.port);
    const pinfo=(tab.port&&state.ports[tab.port])?state.ports[tab.port]:null;
    const responding=pinfo?pinfo.responding:false;
    const rigId=pinfo?pinfo.rig_id:null;
    rec.rigId=rigId;                     // so the Tare button knows the target rig
    const dot=rec.el.querySelector('[data-dot]');
    dot.className='dot'+(responding?' on':'');
    dot.title=responding?'responding':(pinfo?(pinfo.err||'no response'):'no port selected');
    rec.el.querySelector('[data-label]').innerHTML=
      (rigId!=null)?('Rig '+rigId):(tab.port?'Rig ? (waiting)':'Rig &mdash;');
    const r=(rigId!=null&&state.rigs[rigId])?state.rigs[rigId]:null;
    rec.el.querySelector('[data-weight]').textContent=
      (r&&r.weight!=null)?(r.weight+' g'):'\\u2014';
    const params=r?(r.params||{}):{};
    Object.keys(params).forEach(function(name){
      ensureRow(rec,rigId,name);
      rec.rows[name].querySelector('[data-cur]').textContent=params[name];
    });
    rec.el.querySelector('[data-log]').textContent=
      r?((r.log||[]).slice(-12).reverse().join('\\n')||'(no messages)'):'';
  });
}
document.getElementById('addtab').onclick=function(){addTab("");tick();};
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
