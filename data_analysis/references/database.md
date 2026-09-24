# Database access and schema

## Where the data lives

Docker Compose defines the logical volume `hathaway_pgdata`; Docker commonly
shows it as `pc_hathaway_pgdata`. Inside the `hathaway_db` container it is
mounted at `/var/lib/postgresql/data`. On this Windows machine Docker stores
the Linux filesystem inside:

`C:\Users\Z\AppData\Local\Docker\wsl\disk\docker_data.vhdx`

Never open or parse the volume or VHDX for analysis. PostgreSQL owns those
files. Connect to PostgreSQL through its network port.

## Connection

| Setting | Value |
|---|---|
| Host | `localhost` |
| Port | `5432` |
| Database | `hathaway` |
| User | `hathaway` |
| Password | `hathaway` |
| Container | `hathaway_db` |

Default DSN:

```text
host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway
```

Docker Desktop and `hathaway_db` must be running. Diagnose connection failures
from `serial_logging_test/pc/` with `docker compose ps`. Do not edit, move, or
copy the live volume files.

## Python pattern

```python
import psycopg2
import psycopg2.extras
import pandas as pd

DSN = (
    "host=localhost port=5432 dbname=hathaway "
    "user=hathaway password=hathaway"
)

sql = """
SELECT dev_ts, seq, type, channel, value
FROM events_dev
WHERE session_id = %s
ORDER BY t_us, seq
"""

with psycopg2.connect(DSN) as connection:
    with connection.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cursor:
        cursor.execute(sql, (SESSION_ID,))
        events = pd.DataFrame(cursor.fetchall())
```

Use SQL parameters such as `%s`; never insert user values into SQL strings.
Analysis scripts issue `SELECT` statements unless mutation is explicitly
requested.

## Tables and views

### `sessions`

One recording run for one rig.

| Column | Meaning |
|---|---|
| `session_id` | Primary key |
| `rig_id` | Rig identity |
| `started_at` | Host time when the session row was opened |
| `ended_at` | Host time when closed; may be null after an unclean stop |
| `note` | Operator note |

### `samples`

Values that persist until a later sample changes them.

| Column | Meaning |
|---|---|
| `session_id`, `rig_id` | Ownership |
| `seq` | Per-rig ingest sequence used for ordering/gap checks |
| `t_us` | Device capture time converted to Unix epoch microseconds |
| `host_ts` | PC time when the serial line was read |
| `type` | Signal name |
| `channel` | Signal-specific channel |
| `value` | Numeric value |

### `events`

Instantaneous records. It has the same columns as `samples`.

### Views

- `samples_dev`: `samples` plus `dev_ts = to_timestamp(t_us / 1e6)`.
- `events_dev`: `events` plus the same `dev_ts` conversion.
- `trial_params`: parameter value in force at each recorded trial start, in
  long form: `session_id`, `rig_id`, `trial`, `trial_start`, `param`, `value`.

The authoritative definition is
`serial_logging_test/pc/schema.sql`. Database writes are implemented by
`serial_logging_test/pc/ingest.py` and used by `control_panel.py`.

## Time and ordering

- Use `t_us`/`dev_ts` for within-trial intervals. It comes from the device clock
  at capture and is corrected for reboot and `millis()` rollover during ingest.
- `host_ts` is arrival time. Serial reads can stamp a batch nearly together, so
  it is unsuitable for reaction times. It is indexed and useful for broad time
  filtering.
- Sort equal timestamps by `seq` to preserve ingest order.
- `t_us = 0` means no usable device timestamp, most often from older parameter
  messages. Exclude it from timing calculations.
- A reconnect after a dropped USB connection can continue the same session;
  explicit removal closes it. A null `ended_at` does not prove that acquisition
  is still active.

## Initial inspection queries

```sql
SELECT * FROM sessions ORDER BY session_id DESC;

SELECT type, count(*)
FROM samples
WHERE session_id = %s
GROUP BY type ORDER BY type;

SELECT type, count(*)
FROM events
WHERE session_id = %s
GROUP BY type ORDER BY type;

SELECT min(seq), max(seq), count(DISTINCT seq)
FROM (
  SELECT seq FROM samples WHERE session_id = %s
  UNION ALL
  SELECT seq FROM events WHERE session_id = %s
) records;
```
