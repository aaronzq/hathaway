# Hathaway Communication Framework

How telemetry (rig → host) and commands (host → rig) work, what every function
does, and the exact steps to add or remove a message, a parameter, or a command.

---

## 1. The idea in one paragraph

`hathaway.ino` is for the animal task. It never calls `Serial`. When something
happens it calls `Comms::emit(...)`; when the host changes a setting the change
appears in a global variable by itself. Everything between those two points —
formatting, the second CPU core, queues, parsing, range checks, acks, errors —
is handled by a generic engine that is driven by **two tables** at the top of the
sketch. Adding a message or a command means adding a row to a table, not writing
any communication code.

---

## 2. File map

| File | What it is | How often you edit it |
|---|---|---|
| `hathaway.ino` | The task, plus `TELEM_TABLE` and `CMD_TABLE` | Every time you add a message or command |
| `protocol.h` | Wire mechanism: record types, table types, formatter, parser | Almost never |
| `comms.h` | The facility's public API (6 functions) | Never |
| `comms.cpp` | The engine: queues, the core-0 task, dispatch | Almost never |
| `behavior_task.h` | Task constants and the tunable globals | When adding a parameter |
| `behavior_board.h` | Pin assignments | When rewiring |
| `serial_logging_test/pc/ingest.py` | Host parser + database writer | Rarely — the parser is generic |
| `serial_logging_test/pc/control_panel.py` | Multi-rig web UI, sends commands | Rarely |
| `tools/run_golden.sh` | Protocol test, runs on your PC | Run it after changes |

**Dependency rule:** `comms.cpp` includes only `protocol.h` and Arduino headers.
It knows nothing about spouts, gratings or scales. That is what keeps it generic,
and it also avoids a linker error — `behavior_task.h` *defines* mutable globals,
so only one file may include it.

---

## 3. Two cores, two queues

```
        CORE 1  (loop, the task)                CORE 0  (comms task)
        ────────────────────────                ────────────────────
  handleLick1()
      └─ Comms::emit(...)  ──► [ telemQueue, 128 ] ──►  protoFormat()
                                                            └─ Serial.write()

        Comms::service()   ◄── [  cmdQueue,   8  ] ◄──  protoParseCommand()
            └─ writes the global,                            ▲
               calls apply(),                                └─ Serial.read()
               emits the ack
```

Three rules make this safe:

1. **Only core 0 touches `Serial`** — both reading and writing. The task never
   calls `Serial` directly. (One exception: `Comms::begin()` calls
   `Serial.begin()` from `setup()`, before the comms task exists.)
2. **Only core 1 writes the tunable globals and the device objects.** Commands
   are validated on core 0 but *applied* on core 1, so there is a single writer.
3. **`emit()` never blocks.** If the queue is full the record is dropped and a
   counter increments. Sensing and timing never stall waiting on the UART.

Timestamps are taken with `millis()` at the moment of the event on core 1, so
transmission delay never skews recorded event time.

---

## 4. Wire format

### Data plane — one shape for everything

```
<RIG_ID>|NAME:<channel>,<value>,<device_ms>
```

```
1|WEIGHT:1,20.14,12345
1|POSITION:1,0,12349
1|LICK:2,1,12351
1|REWARD:1,7,12352
```

- Channels are **1-based**. A single-channel device reports channel 1.
- `value` is printed with `%g` — integers stay integers, floats stay readable.
- `device_ms` is the rig's `millis()` at the moment of capture.
- The `<RIG_ID>|` prefix is added by the engine so several rigs can share a
  database. `RIG_ID` is set in `behavior_task.h`.

### Control plane — a small fixed vocabulary

| Line | Meaning | When |
|---|---|---|
| `1\|#DEF LICK,E` | LICK is an **E**vent (`S` = state **S**ample) | Startup and on `DUMP` |
| `1\|PARAM:REWARD_DURATION1,50` | Current value of a parameter | Startup, on `DUMP`, after each `SET` |
| `1\|#TARE ok` | An action finished | After the action runs |
| `1\|#ERR parse: xyz` | Line not understood | Immediately, from core 0 |
| `1\|#ERR unknown: FOO` | No such parameter | Immediately |
| `1\|#ERR range: X=999` | Outside the row's `lo`/`hi` | Immediately |
| `1\|#ERR busy: X` | Command queue full | Immediately |

### Inbound — three forms

```
SET REWARD_DURATION1 75     change a parameter
TARE                        run an action
DUMP        (or GET)        re-send the schema and all parameter values
```

---

## 5. Walkthrough: a lick, end to end

1. `handleLick1()` on core 1 sees `lick1.update()` return true and calls
   `Comms::emit(TELEM_LICK, 1, 1.0f, now)`.
2. `emit()` packs a 12-byte `TelemRec {type, channel, value, dev_ms}` and pushes
   it onto `telemQueue` with a zero timeout. If the queue is full it increments
   `g_dropped` and returns. **It never waits.**
3. `commsTask` on core 0 wakes, pops the record, and calls `protoFormat()`.
4. `protoFormat()` writes `1|`, finds the row with `id == TELEM_LICK`, sees no
   custom formatter, and calls `FMT_STD`, producing `LICK:1,1,12350\n`.
5. `Serial.write()` sends it. Blocking here is harmless — it is not the task core.
6. On the PC, `parse_hathaway()` splits the line, looks up `LICK` in the schema
   it learned from `#DEF`, and produces an event record for the database.

## 6. Walkthrough: `SET REWARD_DURATION1 75`

1. `commsTask` accumulates bytes until `\n`, then calls `handleCommandLine()`.
2. `protoParseCommand()` reads the first token, `SET`, so it falls through to
   `sscanf(s, "SET %31s %lf", ...)`, finds `REWARD_DURATION1` in `CMD_TABLE`,
   and checks `75` against that row's `lo`/`hi` (0…1000).
3. Valid → it fills a `CmdMsg {slot, value}` and pushes it onto `cmdQueue`.
   Invalid → core 0 writes `#ERR ...` directly and nothing is queued.
4. Next `loop()`, `Comms::service()` on core 1 pops it and:
   - `protoStore()` writes `75` into `REWARD_DURATION1` via the row's pointer,
   - calls the row's `apply` function, `applyRewardDuration1(75)`, which calls
     `rewarder1.setRewardDuration(75)`,
   - emits `PARAM:REWARD_DURATION1,75` as the ack.
5. The ack travels back out through the normal telemetry path.

The ack reports the value **actually stored**, so if a `%g`/integer conversion
changed it, you see the real value.

---

## 7. Function reference

### `protocol.h` — pure, no Serial, no RTOS

Everything here is a pure function, which is why the whole formatting and
parsing path can be compiled and tested on your PC.

| Item | Purpose |
|---|---|
| `struct TelemRec` | One event in flight: `type`, `channel`, `value`, `dev_ms`. Fixed size so it can be queued by value. |
| `enum TelemKind` | `TELEM_SAMPLE` (`'S'`) or `TELEM_EVENT` (`'E'`). A sample holds its value between updates; an event is an instant. |
| `struct TelemSpec` | One `TELEM_TABLE` row: `{id, name, kind, fmt}`. `fmt` is optional. |
| `FMT_STD(...)` | The standard renderer: `NAME:<ch>,<val>,<ms>`. |
| `TELEM_INTERNAL_PARAM/ACK/DEF` | Engine-reserved ids (250–252) for `PARAM:`, `#NAME ok`, `#DEF`. **Keep your own ids below 240.** |
| `enum CmdKind` | `CMD_PARAM` (settable value) or `CMD_ACTION` (one-shot). |
| `enum StoreType` | `STORE_U32` (points at `unsigned long`), `STORE_F32` (points at `float`), or `STORE_NONE` (actions, which store nothing). |
| `struct CmdSpec` | One `CMD_TABLE` row: `name`, `kind`, `stype`, `storage` pointer, `lo`/`hi`, `apply` callback, `ack` flag. |
| `PARAM_U32 / PARAM_F32 / ACTION / ACTION_QUIET` | Macros that build a row. The `PARAM_*` macros stringify the variable name, so the wire name is always the C identifier. `ACTION_QUIET` skips the `#NAME ok` reply. |
| `struct CmdMsg` | `{slot, value}` — what crosses from core 0 to core 1. |
| `CMD_SLOT_DUMP` | Reserved slot `0xFF` meaning "re-send schema and parameters". |
| `protoLoad(c)` | Read a parameter's current value through its storage pointer. |
| `protoStore(c, v)` | Write a parameter's value through its storage pointer. |
| `protoFormat(...)` | Render one `TelemRec` into a complete line, prefix included. Handles the three internal types, then looks up the table. Returns bytes written, or 0. A line that would be truncated is dropped whole — never half-emitted. |
| `protoFindCmd(...)` | Look up a command slot by wire name. Returns `-1` if absent. |
| `protoFirstToken(...)` | Copy the first whitespace-delimited token out of a line. |
| `protoParseCommand(...)` | Turn an inbound line into a `CmdMsg`, or fill `err` with the reply body. Handles `DUMP`/`GET`, bare actions, and `SET`. |

### `comms.h` / `comms.cpp` — the engine

| Function | Runs on | Purpose |
|---|---|---|
| `Comms::begin(rigId, tt, ttN, ct, ctN, baud, core)` | setup | Stores the table pointers, calls `Serial.begin()`, creates both queues, launches `commsTask` pinned to `core` (default 0). Tables must be `static`, they are kept by pointer. |
| `Comms::emit(type, ch, value, dev_ms)` | task core | Queue one telemetry record. Non-blocking; drops on a full queue. |
| `Comms::service()` | task core | Drain `cmdQueue` and apply each command: store the value, call `apply`, emit the ack. Call it first in `loop()`. |
| `Comms::dumpParams()` | task core | Emit `PARAM:` for every `CMD_PARAM` row. |
| `Comms::dumpSchema()` | task core | Emit `#DEF` for every `TELEM_TABLE` row. |
| `Comms::dropped()` | anywhere | How many telemetry records were dropped. A health metric — should stay 0. |
| `handleCommandLine(s)` *(private)* | core 0 | Calls `protoParseCommand`, writes `#ERR` on failure, queues on success. |
| `commsTask(pv)` *(private)* | core 0 | Forever: drain the telemetry queue and write it, then read available bytes and dispatch each complete line. |

### `ingest.py` — the host parser

| Function | Purpose |
|---|---|
| `strip_rig_prefix(line)` | Split `"1\|BODY"` into `(1, "BODY")`. Tolerates a line with no prefix. |
| `parse_schema_line(line)` | Learn a kind from `#DEF <NAME>,<S\|E>` into the module-level `_schema`. |
| `parse_hathaway(line)` | The generic parser. Strips the prefix, ignores `#` lines (after feeding them to the schema learner), splits `NAME:<ch>,<val>,<ms>`, and returns record dicts. Needs no per-message knowledge. |
| `KNOWN_KINDS` | Fallback kinds, used only if a rig's `#DEF` burst was missed. |
| `COUNTED_EVENTS` | Types that also produce a cumulative sample. Currently `REWARD` → `REWARD_COUNT`. |

`control_panel.py` sends `DUMP` on every successful port open, which is how a
host that attaches mid-session gets the `#DEF` schema and current `PARAM:` values.

---

## 8. Recipe: add an outbound message

Say you want to log trial onset, with the grating angle as the value.

**Step 1 — add an id** to the enum in `hathaway.ino`. Order does not matter; keep
it below 240.

```cpp
enum : uint8_t {
  TELEM_WEIGHT,
  TELEM_POSITION,
  TELEM_MAGNET,
  TELEM_LICK,
  TELEM_REWARD,
  TELEM_TRIAL,        // <-- new
};
```

**Step 2 — add a table row.** Pick `TELEM_EVENT` for an instant, `TELEM_SAMPLE`
for something that holds its value.

```cpp
static const TelemSpec TELEM_TABLE[] = {
  ...
  { TELEM_TRIAL, "TRIAL", TELEM_EVENT },
};
```

**Step 3 — emit it** from the task:

```cpp
void startTrial() {
  float angle = ANGLES[random(NUM_ANGLES)];
  ...
  Comms::emit(TELEM_TRIAL, 1, angle, millis());
}
```

That is all. You get `1|TRIAL:1,45,12345` on the wire, and `1|#DEF TRIAL,E` is
announced at startup, so **`ingest.py` needs no change** — it learns the new type
from the rig and files it in the `events` table under type `TRIAL`.

**Optional host follow-ups**

- To also record a running count, add `"TRIAL": "TRIAL_COUNT"` to `COUNTED_EVENTS`.
- To plot it, add a Grafana panel querying `type = 'TRIAL'`.
- Add `"TRIAL": "E"` to `KNOWN_KINDS` if you want the fallback to cover it too.

**Step 4 — test:** `cd tools && ./run_golden.sh`.

### Non-standard format

If a message genuinely cannot fit `NAME:<ch>,<val>,<ms>`, write a formatter and
put it in the row's fourth field:

```cpp
static int fmtTrial(char *b, size_t c, const char *name, const TelemRec &r) {
  return snprintf(b, c, "%s:%u,%.1f,%lu\n", name, r.channel,
                  (double)r.value, (unsigned long)r.dev_ms);
}
// ...
{ TELEM_TRIAL, "TRIAL", TELEM_EVENT, fmtTrial },
```

Prefer not to. A second shape puts per-message knowledge back into the host.

## 9. Recipe: remove an outbound message

1. Delete its row from `TELEM_TABLE`.
2. Delete its id from the enum.
3. Delete every `Comms::emit(TELEM_THAT_ONE, ...)` call — the compiler will find
   them for you once the enum value is gone.
4. Delete its `checkWire(...)` line(s) in `tools/golden_test.cpp`. Those are
   deliberate byte-exact assertions for each message, so removing a message is
   the one change that requires touching the test.
5. Optionally remove it from `KNOWN_KINDS` / `COUNTED_EVENTS` in `ingest.py`, and
   any Grafana panel that queries it.

Historical rows already in the database are unaffected.

## 10. Recipe: add a tunable parameter

Say you want the trial duration settable at run time.

**Step 1 — declare the global** in `behavior_task.h`, non-`const` so it can change:

```cpp
unsigned long TRIAL_DURATION = 3;   // seconds; GratingHandler::setDuration takes seconds
```

**Step 2 — add a table row** in `hathaway.ino`. Pass the *variable name*, not a
string — the macro stringifies it, so the wire name can never drift.

```cpp
static const CmdSpec CMD_TABLE[] = {
  ...
  PARAM_U32(TRIAL_DURATION, 1, 600, nullptr),
};
```

Use `PARAM_U32` for an `unsigned long`, `PARAM_F32` for a `float`. The two
numbers are the inclusive accepted range; anything outside gets `#ERR range:`.

**Step 3 — add an apply function only if a device object needs telling.** If the
task simply reads the global each loop, pass `nullptr` and you are done. If an
object caches the value, mirror the existing `applyRewardDuration1` pattern:

```cpp
static void applyTrialDuration(float v) { grating.setDuration((unsigned long)v); }
// ...
PARAM_U32(TRIAL_DURATION, 1, 600, applyTrialDuration),
```

Apply functions run on the **control core**, so they may safely touch device
objects. Define them above the table.

That is everything. `SET TRIAL_DURATION 300` now works, it is range-checked,
acked with `PARAM:TRIAL_DURATION,300`, included in every `DUMP`, and reported at
startup. (`SET TRIAL_DURATION 5000` would be refused with
`#ERR range: TRIAL_DURATION=5000`, since the row's `hi` is 600.) No host change: `control_panel.py` builds its UI from the `PARAM:` lines
each rig reports.

## 11. Recipe: add an action command

An action is a one-shot with no stored value — `TARE` is the existing example.

**Step 1 — write the handler.** It takes a `float` (ignored for a bare command)
and runs on the control core, so it may block and may touch hardware.

```cpp
static void doResetCounts(float) { rewardNum1 = 0; rewardNum2 = 0; }
```

**Step 2 — add the row:**

```cpp
ACTION(RESET_COUNTS, doResetCounts),
```

Sending `RESET_COUNTS` now runs it and replies `1|#RESET_COUNTS ok`. Use
`ACTION_QUIET(...)` instead if you do not want the reply.

## 12. Recipe: remove a parameter or command

1. Delete the row from `CMD_TABLE`.
2. Delete its apply function, if it had one.
3. For a parameter, delete the global from `behavior_task.h` — the compiler will
   flag anything still reading it.
Slots are computed at run time from the table, and the test resolves everything
by name, so no test edit is needed.

---

## 13. Rules you should not break

- **Never call `Serial` from the task.** Use `Comms::emit`. Direct calls
  reintroduce the blocking stall the two-core split exists to remove.
- **Never mutate a tunable global or a device object from an `apply` on core 0.**
  Apply functions are already invoked on the control core; leave it that way.
- **Do not include `behavior_task.h` from a new `.cpp`.** It defines globals; a
  second translation unit including it will not link.
- **Keep your `TELEM_*` ids below 240.** 250–252 are reserved by the engine.
- **Table names must match `[A-Z0-9_]` and be at most 31 characters** — that is
  the token buffer size in `protoParseCommand`.
- **Tables must be `static`** (or otherwise have static storage duration).
  `Comms::begin` keeps them by pointer.
- **Use named `static` functions in the tables, not lambdas.** Arduino's
  auto-prototype generator can trip over file-scope lambdas in a `.ino`.

### Limits worth knowing

| Thing | Limit | What happens past it |
|---|---|---|
| Telemetry queue | 128 records | Oldest work continues; new records dropped, `Comms::dropped()` counts them |
| Command queue | 8 | `#ERR busy:` |
| Outbound line | 96 bytes incl. prefix | Line dropped whole, never truncated |
| Inbound line | 63 characters | The first 63 characters are discarded and the *remainder* is parsed as if it were a fresh line — usually producing a stray `#ERR parse:` |
| Command / parameter name | 31 characters | Silently truncated by `protoFirstToken` / `%31s`, so it stops matching |
| Parameter precision | `float`, 24-bit mantissa | Integers above ~16.7 million lose precision. Current ranges top out at 60000, so this is not yet a concern. |
| `%g` output | 6 significant digits | `12.3456789` prints as `12.3457` |

---

## 14. Testing

```bash
cd tools
./run_golden.sh
```

Needs only `g++` and `python3` — no ESP32, no upload. It:

1. pulls the pre-refactor firmware out of git,
2. slices the real tables out of `hathaway.ino` and the real old functions out of
   the old sketch, so the test compares actual code rather than a transcription,
3. checks every line the firmware can emit against its exact expected bytes,
4. round-trips those lines through the real `parse_hathaway()` and confirms type,
   channel, value and device time survive,
5. checks inbound command handling is still byte-identical to the old firmware.

Expected output: `61 checks, 0 failures` and `ingest.py: 29/29 checks passed`.
The two `NOTE` lines about `TAREXYZ` and `DUMPSTER` are intentional — the old
parser prefix-matched command names and the new one does not.

**The test adapts to your tables automatically.** `extract.py` slices the live
`TELEM_TABLE` and `CMD_TABLE` out of the sketch, and generates the tunable
globals and `apply` stubs the test needs from `CMD_TABLE` plus `behavior_task.h`
(into `new_stubs.inc`). Adding a message, a parameter, or an action therefore
needs **no test edit** — the check count simply goes up. Only *removing* a
message requires deleting its `checkWire` line.

You can also exercise the host end without a rig:

```bash
cd serial_logging_test/pc
python3 ingest.py --source simulate --db print
```

---

## 15. Troubleshooting

| Symptom | Likely cause |
|---|---|
| A new message never appears on the wire | No matching row in `TELEM_TABLE` — `protoFormat` returns 0 and the record is dropped |
| A message comes out as `PARAM:` / `#DEF` / `#... ok` | Its id collides with the engine's reserved 250–252 |
| `#ERR unknown: X` for a parameter you added | The host sent a different name than the row's, or you used `SET` on an `ACTION` row (actions are sent bare) |
| `SET` is acked but nothing changes | The row needs an `apply` function — the object cached the old value |
| `DUMP` reports 0 for a parameter | The row's `storage` pointer or `STORE_*` type is wrong |
| `#ERR busy:` | More than 8 commands queued at once; the task loop is stalled |
| `Comms::dropped()` climbing | Telemetry faster than the UART; raise the baud rate or emit less |
| Nothing parses on the host | The `<rig>\|` prefix is not being stripped — `parse_hathaway` handles it, but a custom consumer may not |
| Host misfiles a new type as an event | Its `#DEF` was missed. `control_panel.py` sends `DUMP` on connect; a plain reader must ask or rely on `KNOWN_KINDS` |
