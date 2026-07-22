# Serial logging test (USB, single fake rig)

A self-contained test of the full data path — **ESP32-S3 → USB serial → ingest
service → time-series database → live dashboard** — using a fake 30 Hz data
generator instead of real sensors. Nothing here touches the main `hathaway`
sketch.

```
serial_logging_test/
├── fake_data_gen/fake_data_gen.ino   # firmware: dual-core, 30 Hz fake data over USB
└── pc/
    ├── docker-compose.yml            # brings up TimescaleDB + Grafana in one command
    ├── schema.sql                    # database tables (sessions / samples / events)
    ├── ingest.py                     # serial -> parse -> database
    ├── fake_serial.py                # board-free simulator (for testing without hardware)
    ├── requirements.txt              # Python deps
    └── grafana/                      # auto-provisioned datasource + dashboard
```

## What the ingest reads

By default (`--format hathaway`) the ingest parses the **real `hathaway.ino`
serial output** directly — the human-readable lines the firmware already prints:

| Firmware line | Stored as | Shown on dashboard |
|---------------|-----------|--------------------|
| `HX711 reading: <v>` | sample `WEIGHT` | weight line |
| `POSITION:<0\|1>,<ms>` | sample `POSITION` | binary step line |
| `REWARD:<n>,<ms>` | sample `REWARD_COUNT` **and** event `REWARD` | cumulative line + event dots |
| `LICK1,<ms>` / `LICK2,<ms>` | event `LICK` (ch 1 / 2) | event dots |

Any other line is ignored. The firmware supplies no sequence number, so the
ingest assigns one; the `<ms>` device time (`millis()`) is stored as microseconds
in `t_us`, while the PC arrival time (`host_ts`) is what the dashboards plot.

> There is also a second format, `--format csv` (`seq,t_us,kind,type,channel,value`),
> used by the optional dual-core `fake_data_gen` sketch. You only need it if you
> flash that sketch instead of your real firmware.

---

## Quick test with NO hardware and NO database (30 seconds)

Confirms the code works before you install anything. The ingest can generate its
own fake data with `--source simulate` (one process, no pipe):

```powershell
cd serial_logging_test\pc
python ingest.py --source simulate --db print
```

You'll see parsed rows scroll by. Swap `--db print` for `--db sqlite` to write a
local `hathaway.db` file (no server needed):

```powershell
python ingest.py --source simulate --db sqlite
```

Press **Ctrl+C** to stop.

> **Windows note:** avoid `python fake_serial.py | python ingest.py ...` in
> PowerShell — its pipe buffers/re-encodes between programs and often passes
> nothing through. `--source simulate` sidesteps this. (`fake_serial.py` still
> works when piped on Linux/macOS, or in `cmd.exe`.)

---

## Full setup on Windows 11

### 1. Flash / run the firmware
Use your **real `hathaway.ino`** — the ingest reads its serial output directly.
Flash it as usual, and note the **COM port** (Tools → Port, or Device Manager →
Ports, e.g. `COM3`). The firmware runs at **115200 baud**.

*(The `fake_data_gen` sketch here is only an optional dual-core reference; you
don't need it for real data.)*

### 2. Start the database + dashboard
Install **Docker Desktop** once (https://www.docker.com/products/docker-desktop),
then:
```powershell
cd serial_logging_test\pc
docker compose up -d
```
This launches:
* **TimescaleDB** at `localhost:5432` (schema auto-applied on first run)
* **Grafana** at **http://localhost:3000** (login `admin` / `admin`)

The "Hathaway – Live Rig Monitor" dashboard is provisioned automatically.

### 3. Run the ingest service
```powershell
cd serial_logging_test\pc
pip install -r requirements.txt
python ingest.py --source serial --port COM3 --db postgres
```
(Replace `COM3` with your port. `--format hathaway` and `--baud 115200` are the
defaults, so nothing else is needed for your firmware.) You'll see a heartbeat
every 2 s; rows stream into TimescaleDB.

### 4. Watch it live
Open **http://localhost:3000**, go to the **Hathaway – Live Rig Monitor**
dashboard. It refreshes every second and shows: weight scale (HX711), mouse
position (0/1 step), cumulative rewards, lick-1 events, and reward events.

### Stopping / resetting
```powershell
docker compose down          # stop (keeps data)
docker compose down -v       # stop and DELETE all stored data
```

---

## Notes / what this validates
* **Non-blocking logging:** the firmware timestamps on Core 1 at 30 Hz and only
  enqueues; Core 0 does all USB writing. A stalled USB host can never delay a
  timestamp — dropped records (if the queue ever fills) are counted and reported
  as `#DROPPED n`.
* **Heterogeneous data:** continuous `LOADCELL` samples share the pipeline with
  bursty `LICK`/`REWARD` events, stored in separate tables.
* **Scales later:** the schema already carries `rig_id` and a `session_id`, and
  the commented `frames` table in `schema.sql` shows where microscope image
  metadata will attach. Wireless transport replaces USB without changing the DB.

### Baud rate
Firmware and ingest are both set to **921600**. If you change one, change both
(`SERIAL_BAUD` in the `.ino`, `--baud` on the ingest).
