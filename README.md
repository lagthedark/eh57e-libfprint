# EH57E Linux fingerprint driver (libfprint)

Fingerprint login on Linux for the **Egis Technology / LighTuning EH57E**
fingerprint sensor (`USB 1c7a:057e`), found on some recent laptops. The vendor
ships Windows-only drivers; this project reverse-engineers the USB protocol and
the vendor matching engine to provide **fingerprint login on Linux
(GNOME / fprintd)**.

**TL;DR — it works for me.** I use it every day for GNOME login on Fedora
Bluefin (immutable): enroll succeeds, and the lock screen unlocks on the
**first press**, in about a second. Everything needed for the same sensor is in
this repository. If you have this sensor and it works for you too, keep it and
improve it — patch requests and clean-ups are welcome. This is a hobby,
interoperability project; it comes with no warranty.

> **Status / scope: CONVENIENCE TIER ONLY.**
> The sensor is a plain image sensor: there is **no match-on-chip, no liveness
> detection, and the fingerprint image is sent in plaintext over USB**. This
> driver is therefore *not* a security boundary — it is a convenience feature
> (like Windows Hello), and **a password must always remain as fallback**.
> It is **not** eligible for any biometric certification.

---

## Does it work for you?

You almost certainly have this sensor if `lsusb` shows `1c7a:057e`. This driver
targets that one sensor; other Egis parts are out of scope.

Current behaviour on Fedora Bluefin + GNOME + fprintd:

- **Enroll** (Settings → Users → Fingerprint Login): completes the 5 stages,
  with on-screen hints when a press is weak or lifted too early.
- **Lock screen / login**: unlocks on the **first press** in ~1 s.
- **Wrong finger**: clean *"cannot match"* message and re-prompt — no hang.
- **Fully reversible**: `undeploy-fprintd.sh` restores the stock fprintd.

---

## Hardware

| | |
|---|---|
| Vendor / product | Egis Technology EH57E (`1c7a:057e`) |
| Sensor | 70×57 px grayscale image sensor |
| Protocol | plain vendor USB (control transfers + bulk); no match-on-chip |
| Images | sent in plaintext over USB |

---

## Architecture

- **Pure `FpDevice` subclass** — not `FpImageDevice`, and **no core patch** to
  libfprint. EH57E frames are 70×57 and contain no NBIS minutiae, so the
  standard image path is unusable; enroll/verify are implemented directly.
- **USB transport + init/calibration packet chain + touch-detection state
  machine** live in the driver (`src/egis057e.c`).
- **Matcher** (`src/egis057e-matcher.c`) is a faithful reimplementation of the
  vendor engine (`EgisTouchFPEngine057E.dll`), reverse-engineered (tables/blobs
  extracted by signature scan). It is **not** the EH575 reconstruction.
- **Presence detection** uses a per-pixel warm background baseline, seeded at
  the start of every action from the sensor's fresh empty-sensor calibration
  frame (`0x72`), so detection is coverage-based from the first live frame.
  See `docs/analysis/eh57e-presence-baseline.md`.

## How detection works (short version)

The sensor has no usable finger bit, so contact is inferred from images:

1. Every action runs the vendor init chain, which reads a fresh **empty-sensor
   calibration frame** (`0x72`). This frame is used as the per-pixel background.
2. A live frame is compared pixel-by-pixel to that background. The fraction of
   pixels deviating beyond an adaptive threshold is the **coverage**.
3. Coverage ≥ 12 % ⇒ finger present; ≤ 10 % ⇒ absent; in between is hysteresis.
4. Two consecutive present frames start a **settle**, which picks the best
   usable frame; that frame becomes an enroll stage or a verify probe.
5. Verify reports a match on the first matching probe; a wrong press is
   rejected early with a clean *"cannot match"*.

This design matters because the sensor's idle state is noisy and bimodal
(empty frames can look almost like a finger to naive absolute thresholds), and
because the background is wiped on every device open — seeding it from the
calibration frame removes the "blind window" in which an early press used to be
silently missed.

## Repository layout

```
src/                      driver sources (drop into libfprint)
  egis057e.c / .h
  egis057e-matcher.c / .h
tools/                    CLI tools + Makefile (drop into libfprint/tools)
  egis057e_enroll.c       enroll / verify / monitor / capture CLI
  egis057e_capture.c
  egis057e-match-test.c
scripts/                  operator + deploy helpers
  deploy-fprintd.sh       install lib for the system fprintd (reversible)
  undeploy-fprintd.sh     revert
  fprintd-debug-on.sh     enable driver debug in the fprintd journal
  eh57e.sh / eh57e-debug.sh / eh57e-cleanup.sh / reset-sensor.sh / setup-access.sh
docs/analysis/            reverse-engineering + design notes
docs/engine/              engine table extraction artifacts (no vendor binaries)
AGENTS.md                 guide for coding agents / contributors
README_it.md              Italian translation of this file
```

## Build

Requires libfprint source **1.94.100** (matches the Fedora SRPM; ABI-compatible
with fprintd 1.94.x). This is a source-level driver, not a patch.

1. Get the libfprint tree at 1.94.100 and overlay the sources:
   ```sh
   cp src/egis057e.c src/egis057e.h src/egis057e-matcher.c src/egis057e-matcher.h \
      <libfprint>/libfprint/drivers/
   cp tools/egis057e_*.c tools/Makefile <libfprint>/tools/
   ```
2. Register the driver. In `<libfprint>/libfprint/meson.build` add a driver entry
   (the driver map keys on the sensor's model number):
   ```meson
   'egis057e' : files('drivers/egis057e.c', 'drivers/egis057e-matcher.c'),
   ```
   and in `<libfprint>/meson.build` (the driver options map):
   ```meson
   'egis057e': {},
   ```
3. Build:
   ```sh
   meson setup build -Ddrivers=egis057e
   ninja -C build
   make -C tools
   ```
   Registration check: `1c7a:057e | Egis Technology EH57E Touch Fingerprint Sensor`.

## Deploy to the system fprintd (GNOME login)

On immutable Fedora (Bluefin / Silverblue) there are **no writes to `/usr`**.
The included script installs the library under `/opt`, adds a systemd drop-in
(`LD_LIBRARY_PATH`) and an SELinux `lib_t` label; it is fully reversible.

```sh
sudo bash scripts/deploy-fprintd.sh
# then: Settings -> Users -> Fingerprint Login (enroll), lock the screen and test
sudo bash scripts/undeploy-fprintd.sh   # to revert to the stock fprintd
```

## CLI usage during development

```sh
bash scripts/eh57e.sh monitor 6        # continuous verify loop, finger 6 = right index
bash scripts/eh57e-debug.sh verify 6   # verbose driver debug -> hwtest/verify-debug.log
bash scripts/eh57e-cleanup.sh          # release a wedged device (kills leftovers + USB reset)
```

Verbose driver logs during normal fprintd use (temporary):

```sh
sudo bash scripts/fprintd-debug-on.sh
sudo systemctl restart fprintd
# reproduce, then:
sudo journalctl -u fprintd --since "3 min ago" --no-pager | grep -E "EGIS057E|Verify|Enroll"
```

## Limitations & honesty

- **Convenience tier only** (see top). No liveness; a lifted latent image could be
  replayed. Always keep a password fallback.
- The matcher is a reimplementation of a proprietary engine, done for
  interoperability; end-to-end separation is **preliminary** (calibrated on a
  small set of fingers). Treat mis-accept / mis-reject tuning as ongoing.
- **Vendor binaries are NOT included.** `EgisTouchFP057E.dll`,
  `EgisTouchFPEngine057E.dll`, `EgisTouchFPSensor057E.dll` and the `.inf` came
  from the Windows Update Catalog and are **not redistributable**; they are used
  locally only to extract native tables.
- Because it reimplements a proprietary matcher, this driver is **not
  upstreamable to libfprint as-is**. A future cleaned-up version (e.g. using a
  standard open matcher, if the frames can be adapted to it) would be the road
  to upstream.
- Tested on exactly one machine and one sensor; expect rough edges elsewhere.

## Contributing

Issues, test reports, and patches are welcome. See `AGENTS.md` for the build
and debug workflow, the hard constraints, and the known gotchas. Reports are
most useful when they include `EGIS057E`-marked logs from
`scripts/fprintd-debug-on.sh`.

## License

Driver sources are marked `SPDX-License-Identifier: 0BSD` (see `LICENSE`).
Reverse-engineering notes and tooling are provided for interoperability and
personal use.
