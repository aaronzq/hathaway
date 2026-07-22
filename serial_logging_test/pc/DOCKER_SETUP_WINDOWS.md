# Setting up PostgreSQL + TimescaleDB + Grafana on Windows 11 (Docker, for beginners)

You will **not** install PostgreSQL or Grafana by hand. Docker runs them for you
inside "containers" (pre-packaged, pre-configured bundles). You run one command
and both come up. This guide assumes zero Docker experience.

---

## Step 1 — Install Docker Desktop (once)

1. Download **Docker Desktop for Windows** from
   https://www.docker.com/products/docker-desktop/ and run the installer.
2. Keep the default option **"Use WSL 2"** when asked. (WSL 2 is a lightweight
   Linux layer Windows uses to run containers; the installer sets it up for you.)
3. **Reboot** if it asks you to.
4. Launch **Docker Desktop** from the Start menu. Wait until the whale icon in
   the system tray (bottom-right) stops animating and says **"Engine running"**.
   Docker Desktop must be running whenever you use the database.

> If the installer complains about virtualization: reboot into your PC's BIOS/UEFI
> and enable "Virtualization" / "Intel VT-x" / "AMD-V" (it's on by default on
> most machines), then retry.

### Confirm it works
Open **PowerShell** (Start menu → type "PowerShell") and run:
```powershell
docker --version
docker run --rm hello-world
```
The second command downloads a tiny test container and prints "Hello from
Docker!". If you see that, you're ready.

---

## Step 2 — Start the database + Grafana

In PowerShell, go to the `pc` folder of this project and bring the stack up.
Replace the path with wherever this repo lives:
```powershell
cd C:\Users\Z\Documents\Arduino\hathaway\serial_logging_test\pc
docker compose up -d
```
What happens:
* The first run **downloads** the TimescaleDB and Grafana images (a few minutes,
  one time only). Later runs start in seconds.
* `-d` means "detached" — they run in the background.
* On this first start, the database automatically creates your tables from
  `schema.sql`.

### Check they're running
```powershell
docker compose ps
```
You should see `hathaway_db` and `hathaway_grafana` with state **running**
(the database also shows **healthy**).

---

## Step 3 — Log into Grafana

1. Open a browser to **http://localhost:3000**
2. Login: **admin** / **admin** (it will ask you to set a new password — you can
   skip or set one).
3. Left menu → **Dashboards** → open **"Hathaway – Live Rig Monitor"**.
   It's empty until data arrives — that's expected.

---

## Step 4 — Feed it data

**Option A — with the ESP32 board** (from PowerShell, in the same `pc` folder):
```powershell
pip install -r requirements.txt
python ingest.py --source serial --port COM3 --db postgres
```
(Use your actual COM port. Docker Desktop must be running.)

**Option B — no board yet**, prove the database works with simulated data.
Use the built-in simulator (one process, no pipe):
```powershell
python ingest.py --source simulate --db postgres
```
You'll see a heartbeat like `[ingest] running... 120 rows written` every 2
seconds, and the Grafana dashboard will update every second. Press **Ctrl+C**
to stop.

> Do **not** use `python fake_serial.py | python ingest.py ...` on Windows.
> PowerShell's pipe buffers/re-encodes data between two programs and often
> passes nothing through — that's a PowerShell quirk, not a bug. `--source
> simulate` avoids the pipe entirely.

The ingest connects to the database at `localhost:5432` with user `hathaway`,
password `hathaway`, database `hathaway` (already the defaults in `ingest.py`).

---

## Everyday commands (run from the `pc` folder)

| Goal | Command |
|------|---------|
| Start the stack | `docker compose up -d` |
| Check status | `docker compose ps` |
| See database logs | `docker compose logs timescaledb` |
| Stop (keep data) | `docker compose down` |
| Stop and **erase all data** | `docker compose down -v` |
| Restart after changing `schema.sql` | `docker compose down -v` then `up -d` |

> The schema is only applied when the database volume is empty (first start).
> If you edit `schema.sql`, you must `docker compose down -v` (which erases data)
> and start again for changes to take effect.

---

## Peeking at the data directly (optional)

To run SQL by hand inside the database container:
```powershell
docker exec -it hathaway_db psql -U hathaway -d hathaway
```
Then at the `hathaway=#` prompt:
```sql
SELECT count(*) FROM samples;
SELECT * FROM events ORDER BY host_ts DESC LIMIT 10;
\q      -- quit
```

---

## Troubleshooting

* **"docker: command not found" / "cannot connect to the Docker daemon"** —
  Docker Desktop isn't running. Launch it and wait for "Engine running".
* **Port 5432 or 3000 already in use** — another PostgreSQL or Grafana is
  already running on your PC. Either stop it, or change the left-hand number in
  the `ports:` lines of `docker-compose.yml` (e.g. `"5433:5432"`) and use that
  new port.
* **Grafana dashboard stays empty** — make sure the ingest service is running
  and actually printing that it wrote rows; check `docker compose ps` shows the
  db as healthy.
* **Ingest can't connect to the database** — confirm `docker compose ps` shows
  `hathaway_db` running, and that you installed deps with
  `pip install -r requirements.txt`.
