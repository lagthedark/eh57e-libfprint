#!/usr/bin/env bash
# Revert the EH57E libfprint deployment: restore the SYSTEM libfprint for fprintd.
#   sudo bash /var/home/matt/eh57e-project/scripts/undeploy-fprintd.sh
set -euo pipefail

DEST=/var/opt/egis057e-libfprint
DROPIN=/etc/systemd/system/fprintd.service.d/10-egis057e-libfprint.conf

if [ "$(id -u)" -ne 0 ]; then echo "Serve root:  sudo bash $0"; exit 1; fi

echo "== 1. Rimuovo il drop-in systemd =="
rm -f "$DROPIN"
rmdir /etc/systemd/system/fprintd.service.d 2>/dev/null || true
systemctl daemon-reload

echo "== 2. Rimuovo la lib =="
rm -rf "$DEST"

echo "== 3. Rimuovo il contesto SELinux =="
if command -v semanage >/dev/null 2>&1; then
  semanage fcontext -d "/var/opt/egis057e-libfprint/lib(/.*)?" 2>/dev/null || true
fi

echo "== 4. Riavvio fprintd (torna alla lib di sistema) =="
systemctl stop fprintd 2>/dev/null || true
systemctl reset-failed fprintd 2>/dev/null || true

echo "Fatto: fprintd usa di nuovo la libfprint di sistema."
