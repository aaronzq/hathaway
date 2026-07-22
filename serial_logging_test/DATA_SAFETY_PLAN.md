# Data durability & safety — plan (deferred until beyond proof-of-concept)

**Current status:** proof of concept. No real study data. The default setup
(TimescaleDB in a Docker named volume on the local Windows PC) is acceptable as
is — no backup machinery needed yet.

**Trigger to implement this plan:** *before the first session of real,
keep-forever study data is collected.* At that point the data becomes
irreplaceable (many researchers, thousands of hours) and the layers below should
be in place first.

---

## Guiding principle

The database is never the *only* copy and never the *source of truth*. Data is
tiered so that any single failure is recoverable:

1. **Raw immutable capture** — data exactly as it came off the rigs, never
   edited (rig SD cards; raw serial logs; raw microscope image files). Ground
   truth. The DB can always be rebuilt from these.
2. **Database** — the fast, queryable *working* store. Valuable but
   reconstructable, so it doesn't carry the whole risk.
3. **Backups** — automated copies in more than one physical place.

Docker is not a risk factor here: the DB files are ordinary files on disk that
need backups whether PostgreSQL runs in Docker or natively. The only
Docker-specific footgun is `docker compose down -v` (erases the volume) — avoid
it once real data exists.

---

## Layers to implement (when triggered)

- [ ] **Raw append-only logging.** Ingest also writes the raw serial stream
      verbatim to dated files (e.g. `raw/2026-08-01_rig01.log`). The DB is then
      never the only copy of the raw data. (Small change to `ingest.py`.)
- [ ] **Automated `pg_dump` backups.** Scheduled dumps (e.g. nightly) to a
      backup location, keeping several dated copies. The core safety net.
- [ ] **Bind-mount the DB to a visible folder.** Store DB files in a normal
      Windows folder (e.g. `.\pgdata`) instead of a hidden Docker volume, so
      they're browsable and easy to copy. (One line in `docker-compose.yml`.)
- [ ] **Per-session NWB/HDF5 export.** A self-contained standard archive file
      per session for long-term analysis, independent of any database — the
      format that will outlive any particular DB and that aligns behavior with
      future imaging.

## Storage migration

- **Now:** local Windows PC only.
- **Before real studies:** move the primary or replicated copy onto the
  **institutional / lab server or NAS** (redundant storage + institutional
  backup). The Windows PC can remain the day-to-day runtime, replicating to it.
- **Rule of thumb:** at least two copies, in at least two physical locations,
  with at least one on institutionally-backed-up storage. Verify a restore
  actually works — an untested backup is not a backup.

## Image data (future microscope)

Raw image stacks are large and go to the NAS/file store as files; the database
holds only metadata + file paths (see `frames` table in `schema.sql`). Back up
the image files with the same two-locations rule.
