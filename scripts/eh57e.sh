#!/usr/bin/env bash
# Friendly EH57E enroll/verify wrapper.
# Usage:
#   bash ~/eh57e-project/scripts/eh57e.sh enroll 6     # 6 = right index (0..9 per examples)
#   bash ~/eh57e-project/scripts/eh57e.sh verify 6
set -euo pipefail
ROOT=/var/home/matt/eh57e-project
BIN="$ROOT/libfprint-build/tools/egis057e_enroll"
LIB="$ROOT/libfprint-build/build/libfprint"
WORK="$ROOT/hwtest"
mkdir -p "$WORK"
exec distrobox enter eh57e-dev -- bash -lc "cd '$WORK' && export G_MESSAGES_DEBUG= && export LD_LIBRARY_PATH='$LIB' && exec '$BIN' $*"
