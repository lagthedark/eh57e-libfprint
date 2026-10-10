# EH57E Linux fingerprint driver (libfprint)

A [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) driver for the
**Egis Technology / LighTuning EH57E** fingerprint sensor (`USB 1c7a:057e`),
found on some recent laptops. The vendor ships Windows-only drivers; this
project reverse-engineers the USB protocol and the vendor matching engine to
provide **fingerprint login on Linux (GNOME / fprintd)**.

> **Status / scope: CONVENIENCE TIER ONLY.**
> The sensor is a plain image sensor: there is **no match-on-chip, no liveness
> detection, and the fingerprint image is sent in plaintext over USB**. This
> driver is therefore *not* a security boundary — it is a convenience feature
> (like Windows Hello), and **a password must always remain as fallback**.
> It is **not** eligible for any biometric certification.

## Hardware

| | |
|---|---|
| Vendor / product | Egis Technology EH57E (`1c7a:057e`) |
| Sensor | 70×57 px grayscale, ~250 ms/frame |
| Protocol | plain vendor USB (control transfers + bulk), no match-on-chip |

## Architecture

- **Pure `FpDevice` subclass** — no `FpImageDevice`, and **no core patch** to
  libfprint. EH57E frames are 70×57 and contain no NBIS minutiae, so the
  standard image path is unusable; enroll/verify are implemented directly.
- **USB transport + init/calibration packet chain + touch-detection state
  machine** live in the driver (`src/egis057e.c`).
- **Matcher** (`src/egis057e-matcher.c`) is a faithful reimplementation of the
  vendor engine (`EgisTouchFPEngine057E.dll`), reverse-engineered
  (tables/blobs extracted by signature scan). It is **not** the EH575
  reconstruction.
- **Presence detection** uses a per-pixel warm background baseline plus an
  absolute-quality fallback, with duty-cycled polling (see
  `docs/analysis/eh57e-presence-baseline.md`).

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
  eh57e.sh / eh57e-debug.sh / eh57e-cleanup.sh / reset-sensor.sh / setup-access.sh
docs/analysis/            reverse-engineering + design notes
docs/engine/              engine table extraction artifacts (no vendor binaries)
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

On immutable Fedora (Bluefin/Silverblue), fprintd must use this libfprint. The
included script installs it under `/opt` (no writes to `/usr`) with a systemd
drop-in (`LD_LIBRARY_PATH`) and an SELinux `lib_t` label; it is fully reversible.

```sh
sudo bash scripts/deploy-fprintd.sh
# then: Settings -> Users -> Fingerprint Login (enroll), lock the screen and test
sudo bash scripts/undeploy-fprintd.sh   # to revert
```

## CLI usage during development

```sh
bash scripts/eh57e.sh monitor 6     # continuous verify loop, finger index 6 = right index
bash scripts/eh57e-debug.sh verify 6   # verbose driver debug -> hwtest/verify-debug.log
bash scripts/eh57e-cleanup.sh       # release a wedged device (kills leftovers + USB reset)
```

## Limitations & honesty

- **Convenience tier only** (see top). No liveness; a lifted latent image could be
  replayed. Always keep a password fallback.
- The matcher is a reimplementation of a proprietary engine, done for
  interoperability; end-to-end separation is **preliminary** (calibrated on a
  small set of fingers). Treat mis-accept/mis-reject tuning as ongoing.
- **Vendor binaries are NOT included.** `EgisTouchFP057E.dll`,
  `EgisTouchFPEngine057E.dll`, `EgisTouchFPSensor057E.dll` and the `.inf` came
  from the Windows Update Catalog and are **not redistributable**; they are used
  locally only to extract native tables.
- Because it reimplements a proprietary matcher, this driver is **not
  upstreamable** to libfprint as-is.

## License

Driver sources are marked `SPDX-License-Identifier: 0BSD` (see `LICENSE`).
Reverse-engineering notes and tooling are provided for interoperability and
personal use.
