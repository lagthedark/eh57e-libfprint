# AGENTS.md — guide for coding agents & contributors

This file tells a coding agent (or a human contributor) how to work on this
repository without breaking it. Read it before editing the driver.

## What this is

A source-level **libfprint driver** for the **Egis EH57E** fingerprint sensor
(`USB 1c7a:057e`), plus a reverse-engineered matcher, CLI tools, operator
scripts, and reverse-engineering notes. The goal is GNOME / fprintd fingerprint
login on Linux. See `README.md` for the user-facing description.

## Golden rules (do not violate)

1. **Immutable host — never write to `/usr`.** Install the library under
   `/var/opt/egis057e-libfprint/lib` with a systemd drop-in (`LD_LIBRARY_PATH`)
   and SELinux `lib_t` label, via `scripts/deploy-fprintd.sh`; revert with
   `scripts/undeploy-fprintd.sh`. Every deploy step must be reversible.
2. **Never commit or redistribute vendor binaries** (`EgisTouchFP*.dll`,
   `EgisTouchFPEngine057E.dll`, `EgisTouchFPSensor057E.dll`, `.inf`, `.cat`) or
   **biometric data** (dumps, templates, captured images). They are used locally
   only, to extract tables/behaviour.
3. **Keep a password fallback.** This is a convenience feature, not a security
   boundary (no liveness, plaintext image over USB). Never present it otherwise.
4. **No libfprint core patch.** The driver is a pure `FpDevice` subclass; keep it
   that way so it drops into an unmodified libfprint tree.
5. **Do not claim upstreamability as-is.** The matcher reimplements a proprietary
   engine; upstream needs a clean room / standard open matcher.

## Layout

```
src/       egis057e.c/.h (driver), egis057e-matcher.c/.h (matcher)
tools/     CLI tools + Makefile (enroll/verify/monitor/capture/match-test)
scripts/   deploy-fprintd.sh, undeploy-fprintd.sh, fprintd-debug-on.sh,
           eh57e.sh, eh57e-debug.sh, eh57e-cleanup.sh, reset-sensor.sh,
           setup-access.sh
docs/      analysis/ (design + reverse-engineering notes), engine/ (tables)
```

## Build & test environment

The host lacks `ninja`; build inside the dev container (distrobox):

```sh
distrobox enter eh57e-dev -- bash -lc \
  'cd /var/home/matt/eh57e-project/libfprint-build && ninja -C build && make -C tools'
```

(Adjust the path to wherever the libfprint tree with the driver overlaid lives.)
Build against **libfprint 1.94.100**.

Deploy / revert / debug:

```sh
sudo bash scripts/deploy-fprintd.sh        # install for the system fprintd
sudo bash scripts/undeploy-fprintd.sh      # revert
sudo bash scripts/fprintd-debug-on.sh      # enable EGIS057E debug in the journal
sudo systemctl restart fprintd
sudo journalctl -u fprintd --since "3 min ago" --no-pager | grep -E "EGIS057E|Verify|Enroll"
```

## Debug cheatsheet (log markers)

- `EGIS057E_BG_SEED (calibration)` / `(early)` — background baseline seeded.
- `EGIS057E_SETTLE_START`, `EGIS057E_SETTLE_DONE`, `EGIS057E_SETTLE n N`.
- `EGIS057E_VERIFY_WAIT_LIFT`, `_LIFTED`, `_LIFT_TIMEOUT`, `_FAIL early`.
- `EGIS057E_SENSOR_LATCHED`, `_STUCK_PRESENCE`, `_RELEASE_STUCK`.
- `presence: mad .. cov ..% T .. present=..` — per-frame detection.
- `Verify probe n/N captured` / `already matches` / `best K`.

## Invariants & known gotchas

- **The background is wiped in `dev_open` on every open**, so each verify action
  starts blind unless the background is seeded immediately. Seed it from the
  `0x72` **per-action empty-sensor calibration frame** (read in the init chain).
  Without this, a press made right after the prompt lands in the blind window and
  is silently missed → "must lift and press again".
- **Never trust the absolute fallback (`abs_gate`) for presence.** This sensor's
  noisy idle has stddev 24–66, so `stddev>=8 && grad>=12` is *always* true on
  empty frames. Presence must be **coverage-based** (relative to the seeded
  background). Do not count `finger_frames` while `bg_valid` is false.
- **Exactly one `fpi_device_verify_report` per action** (core guard). The
  "retry-exit" pattern (report a retry and `verify_complete`) **wedges the GNOME
  lock screen** — do not reintroduce it. For a weak verify press, report the
  standard `FPI_MATCH_FAIL` and complete after the lift.
- **GNOME latches identical retry text**, so repeated identical retries are
  invisible; the driver alternates the retry code (`hint_alt`).
- Verify settle is **2 frames**; enroll settle is **4** (quality). Active poll is
  80 ms; idle poll is duty-cycled (250/500/1000 ms).

## What "verified" means here (hardware in the loop)

There is no automated test suite for the sensor. Acceptance checks on a real
machine:

- Enroll completes 5/5 (with hints on weak/early-lifted presses).
- Lock screen unlocks on the **first press** (~1 s).
- A wrong finger fails cleanly with *"cannot match"* and re-prompts; no hang.
- 60 s idle produces **no** `EGIS057E_SETTLE_START` (no phantom detection).
- `scripts/undeploy-fprintd.sh` restores the stock fprintd.

## Commit conventions

Small, focused commits; describe the observed symptom and the fix. Reference log
markers (e.g. `EGIS057E_*`) when relevant. Keep the driver and the notes in sync.
