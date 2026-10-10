#!/usr/bin/env bash
# Grant the local user direct access to the EgisTec EH57E (1c7a:057e) sensor for experiments.
# The container runs as uid 1000 (same as the host user), so the uaccess ACL also applies inside it.
#
# Usage:   sudo bash /var/home/matt/eh57e-project/scripts/setup-access.sh
# Undo:    sudo rm /etc/udev/rules.d/99-eh57e-test.rules && sudo udevadm control --reload
set -euo pipefail
if [[ $EUID -ne 0 ]]; then exec sudo -E bash "$0" "$@"; fi
RULE=/etc/udev/rules.d/99-eh57e-test.rules
cat > "$RULE" <<'EOF'
# EH57E dev: allow the seated user to talk to the EgisTec EH57E (1c7a:057e).
# TAG+=uaccess grants an ACL to the active-session user; does not affect fprintd (root).
SUBSYSTEM=="usb", ATTRS{idVendor}=="1c7a", ATTRS{idProduct}=="057e", TAG+="uaccess", MODE="0660"
EOF
udevadm control --reload
udevadm trigger --action=add --attr-match=idVendor=1c7a --attr-match=idProduct=057e 2>/dev/null || true
sleep 2
DEV=$(lsusb -d 1c7a:057e 2>/dev/null | awk 'NR==1{gsub(/:/,"",$4); print "/dev/bus/usb/"$2"/"$4}')
echo "rule installed: $RULE"
echo "device node  : ${DEV:-NOT FOUND}"
if [ -n "${DEV:-}" ] && [ -c "$DEV" ]; then
  getfacl "$DEV" 2>/dev/null | grep -E "^user:" || ls -l "$DEV"
fi
