#!/usr/bin/env bash
# Protocol test.
#   outbound -- exact wire bytes, then a round trip through ingest.py
#   inbound  -- byte-identical to the pre-refactor firmware, pulled from git
#
#   cd tools && ./run_golden.sh
#
# Requires g++ and python3 only -- no ESP32, no upload.
set -euo pipefail
cd "$(dirname "$0")"

echo "[1/4] pulling the pre-refactor firmware out of git"
git -C .. show HEAD:Hathaway.ino  | tr -d '\r' > old/hathaway_old.ino
git -C .. show HEAD:protocol.h    | tr -d '\r' > old/protocol_old.h

echo "[2/4] slicing the real code out of both sketches"
python3 extract.py

echo "[3/4] compiling and running the protocol test"
g++ -std=c++17 -Wall -Wno-unused-function -I. -Ishim -o golden golden_test.cpp
./golden | tee golden_out.txt

echo "[4/4] round-tripping the generated lines through ingest.py"
python3 check_ingest.py golden_out.txt

echo "PASS"
