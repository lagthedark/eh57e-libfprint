#!/usr/bin/env bash
# Attiva il debug del driver EH57E nel journal di fprintd (temporaneo).
#   sudo bash /var/home/matt/eh57e-project/scripts/fprintd-debug-on.sh
# Per tornare puliti:  sudo bash .../fprintd-debug-off.sh
set -euo pipefail

DEST=/var/opt/egis057e-libfprint/lib
DROPIN=/etc/systemd/system/fprintd.service.d/10-egis057e-libfprint.conf
[ "$(id -u)" -eq 0 ] || { echo "serve root: sudo bash $0"; exit 1; }

mkdir -p "$(dirname "$DROPIN")"
cat > "$DROPIN" <<EOF
[Service]
Environment=LD_LIBRARY_PATH=$DEST
Environment=G_MESSAGES_DEBUG=libfprint-egis057e
EOF
systemctl daemon-reload
systemctl stop fprintd 2>/dev/null || true
systemctl reset-failed fprintd 2>/dev/null || true
echo "Debug ON. Ora esegui:  fprintd-enroll   (premi piu' volte, solleva tra gli stage)"
echo "Poi:  journalctl -u fprintd -n 300 --no-pager"
