#!/usr/bin/env bash
# Recover the EH57E (1c7a:057e) from a wedged USB frame read.
# Issues USBDEVFS_RESET on the device node. Needs write access to the node
# (the udev uaccess ACL from scripts/setup-access.sh is enough).
#
# Usage:  bash /var/home/matt/eh57e-project/scripts/reset-sensor.sh
set -uo pipefail
NODE=$(lsusb -d 1c7a:057e 2>/dev/null | awk 'NR==1{gsub(/:/,"",$4); print "/dev/bus/usb/"$2"/"$4}')
if [ -z "${NODE:-}" ] || [ ! -c "$NODE" ]; then
  echo "EH57E device node not found (unplugged or different bus/device)."
  exit 1
fi
echo "resetting $NODE ..."
python3 - "$NODE" <<'PY'
import sys, fcntl, struct
USBDEVFS_RESET = 0x5514  # _IO('U', 20)
node = sys.argv[1]
try:
    f = open(node, "wb", buffering=0)
    fcntl.ioctl(f, USBDEVFS_RESET, 0)
    print("USBDEVFS_RESET ok ->", node)
except PermissionError:
    print("permission denied; run: sudo bash", __file__)
    sys.exit(2)
except OSError as e:
    print("reset failed:", e)
    sys.exit(3)
PY
sleep 2
lsusb -d 1c7a:057e
