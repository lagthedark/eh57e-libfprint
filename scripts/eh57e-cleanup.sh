#!/usr/bin/env bash
# Clean up leftover EH57E enroll/verify processes (they can survive Ctrl+C when
# run via `distrobox ... | tee`) and reset the sensor so it is not "Resource busy".
# Usage: bash ~/eh57e-project/scripts/eh57e-cleanup.sh
set -uo pipefail
echo "Termino processi egis057e_enroll residui..."
pkill -x egis057e_enroll 2>/dev/null && echo "  (host) terminati" || true
distrobox enter eh57e-dev -- bash -lc 'pkill -x egis057e_enroll 2>/dev/null; pkill -x enroll 2>/dev/null; pkill -x verify 2>/dev/null; true' 2>/dev/null || true
sleep 1
echo "Holder del device:"
fuser /dev/bus/usb/003/003 2>&1 || echo "  (nessuno)"
echo "Reset sensore:"
bash "$(dirname "$0")/reset-sensor.sh" 2>&1 | tail -2
