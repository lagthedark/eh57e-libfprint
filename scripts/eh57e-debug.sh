#!/usr/bin/env bash
# Debug wrapper: keeps libfprint debug ON and captures everything to a log file
# so the driver's detection decisions can be inspected.
# Usage: bash ~/eh57e-project/scripts/eh57e-debug.sh verify 6
set -euo pipefail
ROOT=/var/home/matt/eh57e-project
BIN="$ROOT/libfprint-build/tools/egis057e_enroll"
LIB="$ROOT/libfprint-build/build/libfprint"
WORK="$ROOT/hwtest"
LOG="$WORK/verify-debug.log"
mkdir -p "$WORK"
echo "Log -> $LOG"
distrobox enter eh57e-dev -- bash -lc "cd '$WORK' && export G_MESSAGES_DEBUG=libfprint-egis057e && export LD_LIBRARY_PATH='$LIB' && '$BIN' $*" 2>&1 | tee "$LOG"
