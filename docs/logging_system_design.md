# Parallel Training Rig — Data Logging & Sync System Design

**Project:** Hathaway behavioral training cohort (12× Adafruit Metro ESP32-S3)
**Author/owner:** Aaron
**Status:** Design for review — not yet implemented
**Date:** 2026-07-18

---

## 1. Goals & requirements

| # | Requirement | Source |
|---|-------------|--------|
| R1 | 12 rigs, each an ESP32-S3, controlling all sensors/motors/displays | given |
| R2 | Log behavior (sensor) data **and** stimulus/event data to a central PC | given |
| R3 | Timestamp accuracy across rigs: **1–10 ms** (tightened from the original 30 ms) | given |
| R4 | Handle heterogeneous data rates: **bursty** lick events + **continuous ~10 Hz** HX711 load cell | given |
| R5 | **SD card always-on local backup** on every rig (never lose data) | given |
| R6 | **Wireless** communication and time sync (no data wires to the PC) | given |
| R7 | Data stored **structurally**, usable for (a) real-time monitoring and (b) later neural analysis | given |
| R8 | Design must survive the **worst case: all 12 mice active at once** | given |
| R9 | **Scalable** to add microscope image data later (imaging synced to behavior; microscope on the same PC). Reserve capacity now; do not implement now | given |

### Design principles (these drive every decision below)

1. **Timestamp at the source, at the instant of the event.** The rig stamps each datum with its own high-resolution clock (`esp_timer_get_time()`, microsecond resolution) *before* it is queued, transmitted, or written. Arrival time at the PC is never used as the timestamp — wireless jitter would destroy the 1–10 ms budget.
2. **Sync the clocks, not the packets.** Accuracy = how well the 12 rig clocks agree, not how fast packets arrive. A shared time reference solves R3.
3. **Never block acquisition.** Sensor reading + timestamping runs on one CPU core at high priority; all I/O (SD, WiFi) runs on the other core, fed through a buffer. A slow SD write or a WiFi stall can never delay a lick timestamp.
4. **SD is the source of truth; wireless is best-effort.** The network exists for live monitoring and convenience. If a packet is lost, the SD file still has it, and we reconcile afterward. This makes the whole system robust to WiFi flakiness.
5. **Store structured, export standard.** Live data lands in a time-series database (fast queries, dashboards). For analysis, export to the neuroscience-standard NWB/HDF5 format, which is built to align behavior with imaging.

### 1.6 Component & data-stream inventory (from the `hathaway` repo)

Read from the current firmware. Each rig has:

**Sensors / inputs**

| Component | Pin(s) | Nature | Data character |
|-----------|--------|--------|----------------|
| Lick detector 1 | GPIO 4 (INPUT) | digital, debounced 50 ms | **bursty**, event-driven |
| Lick detector 2 | GPIO 3 (INPUT) | digital, debounced 50 ms | bursty (wired; currently disabled in `loop`) |
| HX711 load cell amp | DOUT 40, SCK 41 | 24-bit ADC, **blocking bit-bang read** | **continuous ~10 Hz** (HX711 does 10 or 80 SPS) |
| Position / lever switch | GPIO 13 (INPUT_PULLUP) | digital, debounced 50 ms | event-driven (`POSITION 0/1`) |

**Actuators / stimuli (these generate task events that must also be logged)**

| Component | Pin(s) | Role |
|-----------|--------|------|
| TFT display (grating) | DC 14, CS 15, BL 16 (+ SPI) | **visual stimulus** — 60 fps drifting grating, ~150 KB SRAM sprite; trial = angle∈{0,45,90,135}°, contrast∈{0.2–1.0}, 3 s |
| Buzzer | GPIO 11 (LEDC PWM) | **auditory stimulus/cue** — tone ∈ {3,6,9,12 kHz} |
| Reward valve 1 (solenoid) | GPIO 8 | reward delivery (50 ms open) |
| Reward valve 2 (solenoid) | GPIO 10 | reward delivery (spout 2) |
| Electromagnet | GPIO 12 | head/body fixation (timed) |
| `Clock` (square-wave/TTL gen) | any GPIO | `micros()`-based configurable pulse train — present in codebase, unused; **candidate for microscope frame-sync TTL** |

**Streams to log** — behavior: lick1, lick2, load-cell samples, position transitions. Task/stimulus events: trial start + params (angle, contrast, period, speed), grating on/off, buzzer note (freq), reward (spout, count), magnet on/off. *Today only `LICK1`, `REWARD`, `LICK2`, `POSITION` are emitted over Serial — the stimulus onsets are not yet logged and must be.*

### 1.7 Key implications (these change the firmware design)

1. **The 50 ms debounce directly conflicts with the 1–10 ms target.** Licks and the switch use a 50 ms `millis()`-based debounce, so the *reported* event lags the true first contact by up to 50 ms, and two licks closer than 50 ms are merged — which also defeats capturing bursts. **Fix:** stamp the **raw first-edge time in a hardware ISR** (µs-precise) as the logged event; use debounce only as a *logic lockout* for reward gating, never as the measurement. The debounce becomes a refractory window, not a delay on the timestamp.
2. **The TFT grating is the dominant CPU load and the main jitter source — not the sensors.** It pushes a 60 fps sprite over SPI every frame, and `drawGrating()` rebuilds the whole sprite at each trial start (several ms). Everything today runs cooperatively in one `loop()`, so during a frame push or redraw the licks/switch simply aren't being read — timing jitter is bounded by loop latency. This is the core justification for the dual-core + ISR design: keep the display/task logic on one core, and make the µs-critical captures **interrupt-driven** so they're immune to display timing.
3. **Timestamps are `millis()` (1 ms) and single-threaded today** → move to `esp_timer_get_time()` (µs) captured at the ISR/sample instant.
4. **SPI bus sharing.** The TFT owns an SPI bus; the new SD card also wants SPI. Either share the bus with a separate CS (never write SD mid-frame from the display core) or put SD on the S3's *second* SPI peripheral. Decide in Phase 1.
5. **`Clock` class = ready-made frame-sync generator.** When the microscope arrives, this existing square-wave generator can emit (or the rig can capture) the frame-clock TTL on the reserved GPIO — the sync story is already half-built.

---

## 2. System architecture (overview)

```
   RIG 1 .. RIG 12  (ESP32-S3)                 CENTRAL PC
 ┌───────────────────────┐
 │ Core 1 (real-time)     │   ESP-NOW beacon    ┌──────────────────────────┐
 │  • lick ISR (burst)    │◄─────────────────── │  Time master (ESP32)     │
 │  • HX711 @10Hz         │   (every 1 s)       │   broadcasts master time │
 │  • encoder / position  │                     └──────────────────────────┘
 │  • stimulus events     │
 │  • esp_timer stamp (µs)│      WiFi / MQTT     ┌──────────────────────────┐
 │        │ ring buffer   │ ───────────────────►│  MQTT broker (Mosquitto) │
 │        ▼               │   publish per rig   └────────────┬─────────────┘
 │ Core 0 (I/O)           │                                  │ subscribe
 │  • SD writer (truth)   │──► microSD (local backup)        ▼
 │  • MQTT publisher      │                     ┌──────────────────────────┐
 └───────────────────────┘                     │ Python ingest service    │
                                                │  → TimescaleDB (Postgres)│
                                                └────────────┬─────────────┘
                                                             │
                              real-time monitoring ◄─────────┤ Grafana dashboards
                              neural analysis      ◄─────────┤ export → NWB/HDF5
                                                             │
                       FUTURE: microscope ──► image files (NAS/disk) + frame
                                              timestamps recorded in DB (metadata only)
```

---

## 3. Per-rig firmware architecture (ESP32-S3, dual-core)

The ESP32-S3 has two cores. In the Arduino framework, `loop()` and your sketch run on **Core 1 (APP)**, and the WiFi/Bluetooth radio stack runs on **Core 0 (PRO)**. We use this split deliberately.

Important nuance from the repo: the **TFT grating + task state machine are coupled** (trial control drives the display) and are the heaviest CPU consumer, but they are *not* microsecond-critical (60 fps = 16.7 ms frames). So they stay together on the real-time core, while the truly time-critical captures are handled by **interrupts**, which preempt the display work regardless of which core they run on.

### Core 1 — Task control + display (real-time, but ms-scale)
- Runs the behavior state machine, grating rendering/scroll, buzzer, reward/magnet timing — the existing `loop()` logic, cleaned up.
- Emits **stimulus/task events** (trial start + angle/contrast, grating on/off, buzzer note, reward, magnet) into the ring buffer, each stamped with `esp_timer_get_time()` at the moment the output is triggered.

### Microsecond-critical captures — interrupt-driven (immune to display load)
- **Lick 1 & Lick 2** — GPIO **ISRs**. Each raw first-edge is stamped with `esp_timer_get_time()` *inside the ISR* and pushed to the ring buffer, then a refractory lockout (replacing the old 50 ms debounce) suppresses re-triggers without delaying the logged time. This is why bursts are safe and why a busy display can't add jitter.
- **Position switch** — same ISR pattern.
- **HX711 load cell** — sampled by a high-priority periodic task (10 Hz); the blocking bit-bang read is kept off the display core.
- **Time-sync beacon receive** — an ISR/callback records the local time when the master beacon arrives (see §4).
- Every producer drops a small fixed-size **record** into a lock-free **ring buffer** (FreeRTOS queue / `ringbuf`).

### Core 0 — Logging & transport (lower priority, allowed to block)
- A consumer task drains the ring buffer and fans each record out to **two sinks**:
  1. **SD card** (source of truth) — batched writes (flush every N records or every ~50 ms) using the `SdFat` library on a pre-allocated contiguous file. Batching keeps SD latency spikes off the critical path.
  2. **MQTT publish** (best-effort live stream) — see §5.
- If WiFi drops, Core 0 keeps writing SD and buffers/reconnects MQTT; acquisition is unaffected.

### Record format (on-device)
Each record is compact and self-describing:

```
{ rig_id, seq, t_us (local µs, sync-corrected), type, channel, value }
```
- `seq` = a per-rig monotonically increasing **sequence number** — the key to gap detection and to reconciling SD vs. database later.
- `type` = enum (LICK, LOADCELL, POSITION, REWARD, STIMULUS, …).
- On the wire we use a compact binary encoding (CBOR/MessagePack) or a fixed CSV line; SD stores the same.

### Ring buffer sizing
Size for `(worst-case burst rate) × (longest tolerable I/O stall)` with margin. With worst-case ~250 records/s per rig (§8) and a generous 2 s stall budget, a few thousand records (tens of KB) is plenty — trivial for the S3's PSRAM.

---

## 4. Time synchronization (recommended: broadcast beacon)

**The problem:** over wireless, you cannot run a shared clock wire, and ordinary network messages arrive with variable delay (jitter of tens of ms). We need all 12 rigs to agree on "now" to within 1–10 ms.

**Recommended approach — ESP-NOW broadcast beacon:**
- One ESP32 is the **time master** (a 13th board, or an ESP32 attached to the PC). It does nothing but broadcast a tiny packet every ~1 s containing its own `esp_timer` value.
- **ESP-NOW** is Espressif's connectionless, low-latency protocol — **fully supported on the ESP32-S3**. It is not a separate radio: it's a mode of the same 2.4 GHz WiFi radio that sends short action frames without associating to an access point, so it runs alongside normal WiFi (same-channel constraint noted below). A *broadcast* reaches all 12 rigs at essentially the same instant, and its air latency is low and consistent (~1–3 ms).
- Each rig records the local time it received the beacon, computes `offset = master_time − local_time`, and maintains a running **linear fit (offset + drift slope)** so it can convert its local microseconds onto a single shared "experiment clock." Because every rig disciplines to the *same* beacon, their timestamps are mutually consistent to ~1–2 ms even if the absolute value drifts slightly.
- The master's timeline is mapped to PC wall-clock once per session (the PC notes wall-clock when it sees the first beacon), which is all neural analysis needs — relative alignment across streams.

**Why not just NTP/SNTP?** NTP over WiFi can reach a few ms on a quiet, dedicated LAN, and it's built in — but it runs *through* the congested WiFi/broker path, so its jitter grows exactly when the network is busy (i.e., when all mice are active). The beacon sidesteps that path entirely. We can run SNTP additionally at boot for a coarse absolute time, then let the beacon do the precise relative work.

**Integration risk to validate early:** ESP-NOW and WiFi share one radio and must be on the **same channel**. When a rig is associated to the AP for MQTT, its channel is fixed; the beacon master must broadcast on that channel. This coexistence is supported but must be tested in week 1 — it's the highest-risk item in the plan.

**Forward-compatibility with the microscope:** when the 2-photon/miniscope arrives, the cleanest cross-system sync is its **frame-clock TTL**. Reserve one spare GPIO on each rig now as a future TTL input, and plan for the microscope's frame times to be recorded on the same experiment clock. (Not built now — just don't design it out.)

---

## 5. Wireless transport — MQTT (explained)

Since you're new to this: **MQTT** is a lightweight **publish/subscribe** messaging protocol built for exactly this shape of system — many small devices sending data to a central collector.

- A central program called a **broker** (we'll use **Mosquitto**, free and battle-tested) runs on the PC.
- Each rig **publishes** its records to a **topic**, e.g. `rig/07/lick`, `rig/07/loadcell`. Topics are just hierarchical names.
- On the PC, one program **subscribes** to `rig/#` (all rigs) and writes to the database; a dashboard can independently subscribe to the same topics for live display. Publishers and subscribers don't know about each other — the broker decouples them.
- **QoS 1 ("at least once")** guarantees delivery with possible duplicates; our `seq` numbers let the ingest service drop duplicates. The broker also **buffers** briefly if the PC is momentarily busy.

Why MQTT over the alternatives (so you know the tradeoffs):

| Option | What it is | Pro | Con | Verdict |
|--------|-----------|-----|-----|---------|
| **MQTT** | pub/sub via a broker | decoupled, easy monitoring, QoS, broker buffers, huge headroom | one extra service (broker) to run | **Chosen** |
| UDP stream | fire-and-forget packets | lowest latency | lossy; you build ordering/dedup/monitoring yourself | rejected (SD already gives us the truth; not worth the DIY) |
| TCP socket per rig | 12 direct connections | reliable, ordered | you write the server + 12× reconnect logic; no built-in fan-out to a dashboard | rejected (MQTT gives this for free) |

Note MQTT is our **live** path only. Because SD is the source of truth (§9), even total WiFi loss costs us only real-time visibility, not data.

---

## 6. Central PC — storage & monitoring

### 6.1 Database — TimescaleDB (PostgreSQL), explained

You said you don't know databases, so here's the reasoning in plain terms.

A **database** is structured storage you can query fast. Your data is **time-series** (every row has a timestamp) but also **relational** (rows belong to a rig, a mouse, a session, a trial). You also need it to **scale** and to make room for **image data** later. The best fit is:

**TimescaleDB** = **PostgreSQL** (the industry-standard relational database) **+ a time-series extension** that makes time-range queries and huge tables fast (automatic partitioning by time, compression, retention policies).

Why this over the alternatives:
- **vs. InfluxDB** (a pure time-series DB): Influx has great dashboards but is weak at relational joins and at cleanly linking future image metadata to behavior. Timescale gives you both worlds and is "just Postgres," so any tool speaks to it.
- **vs. flat files only (CSV/Parquet/HDF5)**: great for offline analysis, but real-time monitoring and live queries need a database in front. We still *export* to files for analysis (§6.3).
- **vs. SQLite**: fine for a single rig, but it handles concurrent writers from 12 rigs poorly. Postgres is built for concurrency.

**The image-scalability pattern (important):** you do **not** store large image files inside the database. Microscope data is huge — a single 2-photon session can be many GB (e.g. 512×512×16-bit at 30 fps ≈ ~15 MB/s ≈ ~900 MB/min). The standard, scalable pattern is:
- Image files live on a **file store** (a NAS, a big disk, or object storage).
- The database holds only a small **`images` / `frames` table**: frame timestamp (on the shared experiment clock), rig/session/mouse it belongs to, and the **file path**.
- Analysis joins behavior to imaging **on time**. The DB stays small and fast; storage scales by adding disks, not by bloating the database.

This is why we choose a relational core now: adding that `frames` table later is a one-line schema change, not a redesign.

### 6.2 Ingest & monitoring stack
- **Python ingest service**: `paho-mqtt` subscribes to `rig/#`, deduplicates by `(rig_id, seq)`, and batch-inserts into TimescaleDB (`psycopg`/`asyncpg`). Batching = tens of thousands of inserts/s comfortably.
- **Grafana for real-time monitoring**: Grafana connects directly to TimescaleDB/Postgres and gives you live dashboards (lick rasters, load-cell traces, reward counts per rig) **with almost no code** — you build panels by pointing-and-clicking SQL. This is the fastest path to "watch all 12 rigs on one screen."
- Language choice: **Python** throughout (ubiquitous in neuro labs, fast to build). A compiled ingest service is unnecessary at these data rates (§8), but the design leaves room for one if ever needed.

### 6.3 Export for neural analysis — NWB
For offline analysis, export each session to **NWB (Neurodata Without Borders)** / HDF5 — the neuroscience-community standard designed to hold behavior, stimulus, and imaging on a common timeline in one file. This is the natural bridge to the future microscope data and to standard analysis toolchains. The DB is the live/queryable store; NWB is the archival/analysis artifact.

---

## 7. Data schema (structural storage)

Relational tables, all timestamps on the shared experiment clock (microseconds):

- **`rigs`** — `rig_id`, hardware info, MAC.
- **`mice`** — `mouse_id`, metadata.
- **`sessions`** — `session_id`, `rig_id`, `mouse_id`, start/stop wall-clock, experiment-clock mapping.
- **`trials`** — `trial_id`, `session_id`, params (angle, contrast, …), start/stop.
- **`events`** — `session_id`, `rig_id`, `seq`, `t_us`, `type` (LICK, REWARD, STIMULUS, POSITION…), `value`. *(Bursty, sparse; a Timescale hypertable.)*
- **`samples`** — `session_id`, `rig_id`, `seq`, `t_us`, `channel` (e.g. loadcell), `value`. *(Continuous 10 Hz; a hypertable.)*
- **`frames`** *(future, microscope)* — `session_id`, `t_us`, `file_path`, frame index, imaging params. *(Metadata only; pixels on the file store.)*

Splitting bursty `events` from continuous `samples` keeps each table's access pattern clean and fast.

---

## 8. Communication & storage load analysis (worst case)

Worst case = all 12 mice simultaneously active, licking in bursts. Conservative **design budget** per rig (well above realistic biology — mouse lick bouts are ~7 Hz):

| Stream | Realistic | Design budget (per rig) |
|--------|-----------|--------------------------|
| Lick events | ~7–14 /s | 100 /s |
| HX711 load cell | 10 /s | 80 /s |
| Position/encoder | sparse | 50 /s |
| Reward/stimulus | <1 /s | 5 /s |
| **Per rig total** | — | **~250 msg/s** |

**Cohort (×12): ~3,000 msg/s.** At ~100 bytes/record:

- **WiFi bandwidth:** 3,000 × 100 B ≈ **300 KB/s ≈ 2.4 Mbit/s** across the whole cohort. A single 2.4 GHz 802.11n AP delivers tens of Mbit/s of real throughput → **we use well under 10%.**
- **MQTT broker (Mosquitto):** handles tens of thousands of msg/s on a normal PC → **~10× headroom or more.**
- **Database inserts:** 3,000 rows/s batched → trivial for TimescaleDB (100k+/s capable).
- **SD per rig:** ~25 KB/s → negligible (SD sustains MB/s).

**Conclusion:** the cohort is nowhere near any bandwidth, broker, or storage limit — even at pathological worst case. The design margin is large. The genuine risks are, as stated, **clock-sync jitter** and **acquisition-core blocking**, both addressed structurally above — *not* throughput.

*(Note: the future microscope is a different regime — GBs of image data — which is exactly why it goes to a file store, not through MQTT or the DB. Behavior traffic and image traffic never compete.)*

---

## 9. Reliability & reconciliation

- **SD = source of truth.** Every record is written locally regardless of network state.
- **`seq` numbers** per rig make gaps detectable. The live MQTT stream may drop/duplicate under stress; the SD file is complete.
- **Post-session reconciliation:** a script compares each rig's SD file against the database by `(rig_id, seq)` and back-fills anything the live path missed. The database ends every session provably complete.
- **Failure modes covered:** WiFi outage (SD keeps all data), PC/broker restart (rigs keep logging to SD + buffer MQTT), beacon master loss (rigs coast on last drift fit; sync degrades slowly, flagged by a watchdog), SD full (pre-flight free-space check + rotation).

---

## 10. Open items / risks to validate

1. **ESP-NOW + WiFi channel coexistence** (§4) — highest risk; prototype on 2 boards first.
2. **Debounce → ISR refractory refactor** — the current 50 ms `millis()` debounce on licks/switch must become an ISR first-edge timestamp + refractory lockout, or the 1–10 ms target and burst capture are unachievable (§1.7-1). Confirm the lick sensor's raw maximum edge rate and set the refractory window from biology, not from noise.
3. **SD ↔ TFT SPI bus** — decide whether SD shares the display SPI bus (separate CS, no SD writes mid-frame) or uses the S3's second SPI peripheral (§1.7-4).
4. **HX711 mode** — confirm 10 Hz vs 80 Hz; either is fine for load, but sets sample-table volume.
5. **Add stimulus-event logging** — trial params, grating on/off, buzzer, magnet are not logged today and must be (§1.6).
6. **Dedicated AP/channel** for the cohort to avoid contention with building WiFi.
7. **PC/NAS storage sizing** once microscope specs are known.

---

## 11. Phased implementation plan

**Phase 0 — De-risk (2 boards):** prove ESP-NOW beacon sync + WiFi/MQTT coexistence; measure achieved cross-rig timestamp agreement (target ≤10 ms, expect ~1–2 ms).

**Phase 1 — Single-rig firmware:** dual-core skeleton (acquisition Core 1 → ring buffer → Core 0), lick ISR, HX711 @10 Hz, `esp_timer` stamping + beacon correction, SD writer with batching + `seq`. Validate no acquisition blocking under simulated bursts.

**Phase 2 — Wireless + PC:** Mosquitto broker, Python ingest → TimescaleDB, Grafana live dashboard for one rig.

**Phase 3 — Scale to 12:** replicate, load-test at worst case, verify reconciliation fills injected gaps.

**Phase 4 — Analysis export:** per-session NWB/HDF5 export aligning events + samples on the experiment clock.

**Phase 5 (future) — Microscope:** add `frames` table + file store, wire frame-clock TTL to the reserved GPIO, extend NWB export to include imaging.

---

## 12. Additional bill of materials (beyond current rigs)

- microSD module/slot + card per rig (if not already fitted) — local backup.
- 1× ESP32 as the time-master beacon (or a PC-attached ESP32).
- 1× dedicated WiFi AP for the cohort.
- Central PC (or small server) running Mosquitto + Python + TimescaleDB + Grafana.
- (Future) NAS/large disk for image storage.
```
