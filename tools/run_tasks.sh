#!/usr/bin/env bash
# Task state-machine test.
#
#   cd tools && ./run_tasks.sh
#
# Compiles the task layer (task.cpp + tasks.cpp) for the host and runs the
# scripted event traces in task_test.cpp. Requires g++ only -- no ESP32, no
# upload. Run this before every flash.
set -euo pipefail
cd "$(dirname "$0")"

g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I. -Ishim \
    -o task_test task_test.cpp ../task.cpp ../tasks.cpp

./task_test
