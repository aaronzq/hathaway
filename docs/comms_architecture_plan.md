# Communication Architecture — Plan & Roadmap

## Decision summary

- **Now (proof-of-concept, 1–3 rigs):** stay on USB Serial — one COM port per rig,
  existing `ingest.py` + TimescaleDB + Grafana. Wire format unchanged.
- **Immediate work:** move all serial I/O off the control loop onto the
  ESP32-S3's *second core* so sensing and control never block on `Serial`.
- **Future (fleet, ~12+ rigs):** migrate the transport to WiFi + MQTT. Recorded
  in the last section — decided, but **not built yet**.

The dual-core refactor is the enabler for both: once control and comms are
decoupled through queues, switching Serial → WiFi later touches only the comms
task, not the control loop.

---

## Immediate work: move serial comm to the second core

### Why
Today every `Serial.print` runs **inline inside `loop()`** (telemetry in the
handlers, acks, etc.). `Serial.println` blocks once the TX FIFO fills — a ~10-byte
line at 115200 can stall the loop ~1 ms. With HX711 reads, grating scrolling, and
lick/switch timing in that same loop, this is a live source of jitter. Pushing all
serial I/O to the other core removes that stall entirely.

### Target structure
- **Core 1 (APP_CPU) — control task.** The current `loop()` work: `grating.update()`,
  `buzzer`, licks, `rewarder.update()`, switch, `magnet.update()`, `handleScale()`.
  Must never block. **No direct `Serial` calls anymore.**
- **Core 0 (PRO_CPU) — comms task.** Owns `Serial` completely — both TX and RX.
- **Two FreeRTOS queues** as the only cross-core channel:
  - `txQueue`: control → comms (telemetry records).
  - `cmdQueue`: comms → control (validated parameter updates).

### Outbound / telemetry path
- Define a fixed-size record, e.g.
  `struct Telem { uint8_t kind; uint8_t type; uint8_t channel; float value; uint32_t dev_ms; };`
- Handlers stop printing. Instead they build a record, **stamp `millis()` at capture
  time**, and `xQueueSend(txQueue, &rec, 0)` — non-blocking; if the queue is full,
  bump a dropped-count (control must never block).
- The comms task drains `txQueue`, formats the **existing** wire lines
  (`MAGNET:1,<ms>`, `POSITION:0,<ms>`, `HX711 reading: …`, etc.), and `Serial.print`s
  them there, where blocking is harmless.
- Wire format is unchanged, so `ingest.py`, the simulator, and Grafana need no edits.
- Timestamp fidelity actually *improves*: the event time is stamped at capture on the
  control core; TX latency no longer skews it.

### Inbound / command path (ties into the tunable-params plan)
- The comms task reads incoming bytes non-blocking, accumulates a line, parses
  `SET <NAME> <VALUE>`, and range-checks it.
- Valid → `xQueueSend(cmdQueue, &update, 0)`. Invalid → emit `#ERR …`.
- The control task, at the top of each loop, drains `cmdQueue` and **applies** the
  change: assign the mutable global **and** call the setter
  (`rewarder1.setRewardDuration(...)`, `magnet.setFixDuration(...)`).
- Enqueue a `PARAM:<NAME>,<value>,<ms>` ack onto `txQueue` so every change is logged.

### Concurrency rules (keep these strict)
- **Only the comms task touches `Serial`** — read and write. The control task never
  calls `Serial` directly.
- **Only the control task mutates** the `rewarder`/`magnet` objects and the tunable
  globals (by draining `cmdQueue`). Single writer → no cross-core races.
- Queues are the **sole** shared state. Non-blocking enqueue on the control side;
  blocking I/O only on the comms core.

### RTOS details
- Create the comms task with
  `xTaskCreatePinnedToCore(commsTask, "comms", ~4096, NULL, prio, &handle, 0)` (core 0).
- Leave the Arduino `loopTask` (control) on core 1 (its default); optionally raise its
  priority.
- Queue depths: `txQueue` ~64–128 records, `cmdQueue` ~8. Tune to burst size.
- Drop policy for telemetry: drop-oldest or count drops — never block control.
- Mind the task watchdog on both cores; feed appropriately.

### Migration order
1. **Telemetry only.** Introduce the record struct + `txQueue` + a comms task that just
   drains and prints. Move existing prints to enqueues. Verify byte-identical output via
   `--source simulate` and a real rig. (No behavior change yet — this is the safe first step.)
2. **Inbound commands.** Add RX + `cmdQueue` + the `SET` dispatcher (the tunable-params
   work), applied on the control core.
3. **Setters.** Add `setRewardDuration()` / `setFixDuration()`, wire `REWARD_DURATION`
   and `MAG_FIX_DURATION`, then the direct-read globals (`REWARD_INTERVAL`,
   `SCALE_HIGH_THRESH`, `SCALE_LOW_THRESH`).

### Testing
- `fake_serial.py` / `--source simulate` unaffected (wire format identical).
- Optional: log loop-period min/max before vs after to confirm the inline-`Serial` stall
  is gone.
- Confirm a `SET` round-trips and the `PARAM` ack lands in the `events` table.

---

## Future: WiFi + MQTT (decided, not built)

When rig count grows or OTA becomes worth it:

- **Transport:** WiFi + MQTT broker; rig id in the topic (`rig/<N>/telemetry`,
  `rig/<N>/cmd`). Bidirectional command channel comes for free.
- **Reuse the same dual-core split** — only the comms task changes: swap `Serial` for an
  MQTT client on core 0 (where the WiFi stack already runs). Control loop untouched.
- **Add:** local ring buffer + auto-reconnect, `seq` gap detection, SNTP clock-mapping
  records (device `millis()` ↔ wall-clock), OTA update path, heartbeat / last-seen.
- **Timing stays valid** because events are stamped with device `millis()` on capture —
  WiFi jitter affects arrival time, not recorded event time.
- `ingest.py` becomes an MQTT subscriber demuxing rigs into TimescaleDB (schema already
  has `rig_id` / `session_id`).
- Keep Serial as the bench-side debug path.

**Why WiFi later, not now:** at 1–3 co-located rigs, Serial is lower-latency, fully
deterministic, and far simpler to debug. WiFi's wins — no cabling, OTA to the whole
fleet, physical spread, central logging — only pay off at scale, and cost added
infrastructure (broker, reconnect/OTA logic) we don't need yet.
