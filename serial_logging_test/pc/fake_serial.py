#!/usr/bin/env python3
"""
Board-free simulator: prints the exact line protocol the firmware emits, at
30 Hz, to stdout. Pipe it into the ingest service to test the whole PC
pipeline without an ESP32 attached.

  python fake_serial.py | python ingest.py --source stdin --db sqlite
  python fake_serial.py --seconds 10 | python ingest.py --source stdin --db print
"""
import argparse
import random
import sys
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--hz", type=float, default=30.0)
    p.add_argument("--rig", type=int, default=1)
    p.add_argument("--seconds", type=float, default=0.0, help="0 = run forever")
    p.add_argument("--realtime", action="store_true",
                   help="sleep between ticks to emit at true wall-clock rate")
    args = p.parse_args()

    print("#HATHAWAY_SERIAL v1")
    print(f"#RIG {args.rig}")
    print("#COLUMNS seq,t_us,kind,type,channel,value")
    sys.stdout.flush()

    seq = 0
    t0 = time.monotonic()
    period = 1.0 / args.hz
    tick = 0
    while True:
        now = time.monotonic()
        t_us = int((now - t0) * 1_000_000)

        seq += 1
        weight = 20.0 + 2.0 * random.random()
        print(f"{seq},{t_us},S,LOADCELL,0,{weight:.4f}")

        if random.random() < 0.12:            # sparse licks
            seq += 1
            spout = random.choice([1, 2])
            print(f"{seq},{t_us},E,LICK,{spout},1.0000")
        if random.random() < 0.01:            # rare rewards
            seq += 1
            print(f"{seq},{t_us},E,REWARD,1,1.0000")

        sys.stdout.flush()
        tick += 1
        if args.seconds and (now - t0) >= args.seconds:
            break
        if args.realtime:
            time.sleep(period)


if __name__ == "__main__":
    main()
