# Hathaway — new PC setup

Step-by-step setup for a Windows PC that will flash rigs (ESP32-S3 boards) and/or
run the PC-side database, dashboard, and control panel. Do the sections you need:

* **Section 1 (Arduino)** — only needed on a PC that flashes firmware onto a rig.
* **Sections 2–3 (Docker + Python)** — only needed on the PC that runs the
  database/dashboard/control panel (usually one central PC for all rigs).

---

## 0. Get the code

Install [Git for Windows](https://git-scm.com/download/win), then in PowerShell:

```powershell
cd C:\Users\<you>\Documents\Arduino
git clone https://github.com/aaronzq/hathaway.git
```

---

## 1. Arduino firmware setup

The rigs are **Adafruit Metro ESP32-S3** boards.

### 1.1 Install Arduino IDE
Download and install the [Arduino IDE](https://www.arduino.cc/en/software) (2.x).

### 1.2 Add the ESP32 board package
1. Open **File → Preferences**.
2. In **Additional Boards Manager URLs**, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**.
4. Go to **Tools → Board → Boards Manager**, search **esp32**, install the
   package by Espressif Systems (latest version).

### 1.3 Select the board
**Tools → Board → ESP32 Arduino → Adafruit Metro ESP32-S3**.

The board plugs in over native USB — Windows 10/11 needs no separate driver.
Once plugged in, note its **COM port** (Tools → Port, or Device Manager → Ports).

### 1.4 Install libraries
**Tools → Manage Libraries**, install:

| Library | Used for |
|---|---|
| `HX711` | load-cell / weight sensor |
| `TFT_eSPI` (Bodmer) | the display (grating stimulus) |
| `FastAccelStepper` | the rail stepper motor |

**TFT_eSPI needs its pins configured before it will compile/work.** This project
uses `TFT_DC=14, TFT_CS=15, TFT_BL=16` (see `behavior_board.h`), which must be
set in the library's `User_Setup.h` (in
`Documents\Arduino\libraries\TFT_eSPI\User_Setup.h`). Copy the `User_Setup.h`
already used on a working rig PC rather than redoing this from scratch — I
don't have the rest of the display wiring (MOSI/SCLK/RST/driver) written down
anywhere in this repo to reconstruct it from.

### 1.5 Flash it
Open `hathaway.ino`, select the COM port (**Tools → Port**), click **Upload**.

> **After upload, press the board's physical reset button once.** This is a
> known ESP32-S3 quirk — code doesn't start running until you do.

Open **Tools → Serial Monitor** at **115200 baud** to confirm it's printing data.

---

## 2. Docker (database + dashboard)

Runs on the PC that ingests and displays data from all rigs.

### 2.1 Install Docker Desktop
1. Download from [docker.com/products/docker-desktop](https://www.docker.com/products/docker-desktop/).
2. During install, keep the default **"Use WSL 2"** option.
3. Reboot if asked.
4. Launch **Docker Desktop** and wait for the whale icon (system tray) to say
   **"Engine running"**. Docker Desktop must stay running while you use the
   database.

If it complains about virtualization: reboot into BIOS/UEFI, enable
**Intel VT-x** / **AMD-V**, retry.

Verify:
```powershell
docker --version
docker run --rm hello-world
```

### 2.2 Bring up the containers
```powershell
cd C:\Users\<you>\Documents\Arduino\hathaway\serial_logging_test\pc
docker compose up -d
```
This pulls two images (first run only, a few minutes) and starts two containers:

| Container | Image | Purpose | URL/port |
|---|---|---|---|
| `hathaway_db` | `timescale/timescaledb:latest-pg16` | time-series database | `localhost:5432` |
| `hathaway_grafana` | `grafana/grafana:latest` | live dashboards | `http://localhost:3000` (login `admin`/`admin`) |

Tables are created automatically from `schema.sql` on first start. Data persists
in Docker volumes across restarts.

Check status:
```powershell
docker compose ps
```
Both should show **running** (`hathaway_db` also **healthy**).

### 2.3 Everyday Docker commands
Run from `serial_logging_test\pc`:

| Goal | Command |
|---|---|
| Start | `docker compose up -d` |
| Status | `docker compose ps` |
| Database logs | `docker compose logs timescaledb` |
| Stop (keep data) | `docker compose down` |
| Stop and **erase all data** | `docker compose down -v` |
| Apply a `schema.sql` change | `docker compose down -v` then `up -d` |

---

## 3. Python environment (control panel)

The control panel reads all rigs' serial ports, writes to the database, and
serves a small local web UI.

### 3.1 Install Python
Install [Python 3.10+](https://www.python.org/downloads/) — check
**"Add python.exe to PATH"** during install.

### 3.2 Install the Python libraries
```powershell
cd C:\Users\<you>\Documents\Arduino\hathaway\serial_logging_test\pc
pip install -r requirements.txt
```

### 3.3 Run it
With Docker running (section 2) and rigs plugged in:
```powershell
python control_panel.py --auto
```
`--auto` finds all connected rigs' COM ports by itself. To pick ports by hand
instead:
```powershell
python control_panel.py --port COM4 --port COM5
```
Open the web UI at **http://127.0.0.1:8000**, and Grafana at
**http://localhost:3000** for historical dashboards.

---

## Quick sanity check (no hardware needed)

To confirm the PC-side pieces work before any rig is plugged in:
```powershell
cd C:\Users\<you>\Documents\Arduino\hathaway\serial_logging_test\pc
python ingest.py --source simulate --db print
```
Rows should print immediately. Ctrl+C to stop. See
`serial_logging_test\README.md` and `serial_logging_test\pc\DOCKER_SETUP_WINDOWS.md`
for more detail on this test path.

---

## Not covered here
* Wiring/pin diagram for the sensors, motor, and display (not written down in
  this repo — copy from a working rig).
* Grafana dashboard editing (see the provisioned "Hathaway – Live Rig Monitor"
  dashboard once Grafana is up).
