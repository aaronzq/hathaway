#!/usr/bin/env python3
"""Round-trip the firmware's own output through ingest.py.

Reads the WIRE lines emitted by golden_test, each "<expect>\t<line>", where
<expect> is either "TYPE|ch|value|ms", "#DEF" (must teach the schema and
produce no records), or "#IGNORE" (must produce no records).
"""
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).parent.parent /
                       "serial_logging_test" / "pc"))
import ingest  # noqa: E402

fails = 0
checked = 0

for raw in open(sys.argv[1]):
    raw = raw.rstrip("\n")
    if not raw.startswith("WIRE "):
        continue
    expect, _, line = raw[len("WIRE "):].partition("\t")
    recs = ingest.parse_hathaway(line)
    checked += 1

    if expect in ("#DEF", "#IGNORE"):
        if recs:
            fails += 1
            print(f"  FAIL {line!r} should yield no records, got {recs}")
        continue

    name, ch, val, ms = expect.split("|")
    ch, val, ms = int(ch), float(val), int(ms)
    primary = [r for r in recs if r["type"] == name]
    if not primary:
        fails += 1
        print(f"  FAIL {line!r} -> no {name} record (got {recs})")
        continue
    r = primary[0]
    if r["channel"] != ch or r["value"] != val or r["t_us"] != ms * 1000:
        fails += 1
        print(f"  FAIL {line!r} -> ch={r['channel']} val={r['value']} "
              f"t_us={r['t_us']}, expected ch={ch} val={val} t_us={ms * 1000}")

# The schema must have been learned from the #DEF lines, not the fallback.
for name, kind in (("WEIGHT", "S"), ("POSITION", "S"), ("MAGNET", "S"),
                   ("LICK", "E"), ("REWARD", "E")):
    checked += 1
    if ingest._schema.get(name) != kind:
        fails += 1
        print(f"  FAIL schema not learned for {name}: "
              f"{ingest._schema.get(name)!r} != {kind!r}")

# A message the host has never heard of must still parse once #DEF arrives.
checked += 1
ingest.parse_hathaway("1|#DEF TRIAL,E")
recs = ingest.parse_hathaway("1|TRIAL:1,3,9999")
if not (len(recs) == 1 and recs[0]["type"] == "TRIAL" and
        recs[0]["kind"] == "E" and recs[0]["value"] == 3.0 and
        recs[0]["t_us"] == 9999000):
    fails += 1
    print(f"  FAIL unknown-message-after-#DEF: {recs}")

print(f"ingest.py: {checked - fails}/{checked} checks passed")
sys.exit(1 if fails else 0)
