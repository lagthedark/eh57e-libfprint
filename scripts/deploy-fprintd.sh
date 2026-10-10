#!/usr/bin/env bash
# Deploy the EH57E libfprint so the SYSTEM fprintd uses it (Fedora Bluefin, immutable).
# Reversible: see undeploy-fprintd.sh.
#   sudo bash /var/home/matt/eh57e-project/scripts/deploy-fprintd.sh
set -euo pipefail

SRC=/var/home/matt/eh57e-project/libfprint-build/build/libfprint
DEST=/var/opt/egis057e-libfprint/lib          # /opt -> /var/opt (real path for SELinux)
FCTX=/var/opt/egis057e-libfprint
DROPIN_DIR=/etc/systemd/system/fprintd.service.d
DROPIN="$DROPIN_DIR/10-egis057e-libfprint.conf"

if [ "$(id -u)" -ne 0 ]; then echo "Serve root:  sudo bash $0"; exit 1; fi
if [ ! -f "$SRC/libfprint-2.so.2.0.0" ]; then echo "ERRORE: lib non trovata in $SRC (compila prima)"; exit 1; fi

echo "== 1. Installo la lib in $DEST =="
mkdir -p "$DEST"
install -m 0755 -o root -g root "$SRC/libfprint-2.so.2.0.0" "$DEST/libfprint-2.so.2.0.0"
ln -sfn libfprint-2.so.2.0.0 "$DEST/libfprint-2.so.2"
ln -sfn libfprint-2.so.2     "$DEST/libfprint-2.so"
ls -l "$DEST"

echo "== 2. SELinux: contesto lib_t su $FCTX/lib =="
if command -v semanage >/dev/null 2>&1; then
  semanage fcontext -a -t lib_t "$FCTX/lib(/.*)?" 2>/dev/null \
    || semanage fcontext -m -t lib_t "$FCTX/lib(/.*)?" 2>/dev/null || true
  restorecon -Rv "$FCTX/lib" || true
else
  echo "ATTENZIONE: 'semanage' mancante (pacchetto policycoreutils-python-utils). Salto la relabel."
fi
ls -Z "$DEST" 2>/dev/null || true

echo "== 3. Drop-in systemd per LD_LIBRARY_PATH =="
mkdir -p "$DROPIN_DIR"
cat > "$DROPIN" <<EOF
[Service]
Environment=LD_LIBRARY_PATH=$DEST
EOF
cat "$DROPIN"

echo "== 4. Riavvio fprintd =="
systemctl daemon-reload
systemctl stop fprintd 2>/dev/null || true     # D-Bus riattiva al bisogno
systemctl reset-failed fprintd 2>/dev/null || true

echo "== 5. Verifica =="
systemctl show fprintd -p Environment -p ExecStart 2>/dev/null || true
echo "-- attivo ora:"; systemctl is-active fprintd 2>/dev/null || true
echo
echo "Fatto. Ora: Impostazioni > Utenti > Impronta digitale (GNOME) per l'enroll."
echo "Se qualcosa non torna: journalctl -u fprintd -n 80 --no-pager"
