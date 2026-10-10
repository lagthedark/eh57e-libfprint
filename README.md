# EH57E Linux fingerprint driver (libfprint)

This is a small [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
driver for the fingerprint sensor in my **Samsung Galaxy Book Pro 360**, an
**Egis / LighTuning EH57E** (`USB 1c7a:057e`). Egis only ships Windows drivers,
so this project reverse-engineers the USB protocol and the matching engine to
get fingerprint login working on Linux. I use it on **Fedora Bluefin** with
GNOME. Enrolling works, and unlocking usually happens on the first press,
though not always; when it takes, it is under three seconds.

Before anything else: **this is a convenience feature, not a security
feature.** The sensor just sends an image over USB — there is no match-on-chip
and no liveness detection — so it is in the same class as Windows Hello, not a
lock. Keep your password as fallback, always.

## What it actually is

EH57E frames are 70×57 pixels and carry no NBIS minutiae, so libfprint's normal
image path cannot be used here. The driver is therefore a plain `FpDevice`
subclass — no core patch — that speaks the vendor USB protocol directly and
implements enroll and verify itself. The matcher is a reimplementation of the
vendor engine, reverse-engineered from the Windows driver; the touch detection
builds a per-pixel background from the sensor's own empty-sensor calibration
frame and looks at how much of the image deviates from it. More of the
reverse-engineering and design notes live in `docs/`.

It works, but treat it as a hobby project that happens to work on one machine.

## The device

The EH57E is the sensor in my machine, a **Samsung Galaxy Book Pro 360**
(2021; the 13.3" and 15.6" models are NP930QDB and NP950QDB). The same
`1c7a:057e` part also turns up on the non-360 **Galaxy Book Pro** of that year
(models 935XDB and 950XDB), and I haven't found a non-Samsung laptop using it.
Other Egis parts — `1c7a:0575` (EH575), for instance — are different sensors
and out of scope. If `lsusb` shows `1c7a:057e`, this driver is for you.

## Building

It is a source-level driver, not a patch. It needs the libfprint source at
**1.94.100** (what Fedora ships; ABI-compatible with fprintd 1.94.x). Copy the
driver into a libfprint tree, register it, and build:

```sh
cp src/egis057e.c src/egis057e.h src/egis057e-matcher.c src/egis057e-matcher.h \
   <libfprint>/libfprint/drivers/
cp tools/egis057e_*.c tools/Makefile <libfprint>/tools/
```

In `<libfprint>/libfprint/meson.build`, add the driver next to the others:

```meson
'egis057e' : files('drivers/egis057e.c', 'drivers/egis057e-matcher.c'),
```

and in `<libfprint>/meson.build`:

```meson
'egis057e': {},
```

then

```sh
meson setup build -Ddrivers=egis057e
ninja -C build
make -C tools
```

You should end up with `1c7a:057e | Egis Technology EH57E Touch Fingerprint
Sensor` registered.

## Installing it for GNOME login

On an immutable Fedora (Bluefin / Silverblue) you cannot write to `/usr`, so
the included script installs the library under `/opt`, points fprintd at it
with a systemd drop-in, and labels it for SELinux. It is reversible:

```sh
sudo bash scripts/deploy-fprintd.sh
# then: Settings -> Users -> Fingerprint Login, and lock the screen to test
sudo bash scripts/undeploy-fprintd.sh   # back to the stock fprintd
```

## Trying it without touching the system

There is a small CLI, useful while poking at it:

```sh
bash scripts/eh57e.sh monitor 6        # verify loop, 6 = right index finger
bash scripts/eh57e-debug.sh verify 6   # verbose driver log
bash scripts/eh57e-cleanup.sh          # unstick the reader if it gets wedged
```

If you want the driver's own log while using the normal GNOME login, there is
`scripts/fprintd-debug-on.sh` (temporary; see `AGENTS.md`).

## Honest limitations

The matcher is a reimplementation of a proprietary engine, written for
interoperability. It is calibrated on a handful of fingers, so expect the
odd false accept or false reject, and treat the tuning as a work in progress.
The vendor's own binaries (`EgisTouchFP*.dll`, the `.inf`) are **not**
included — they come from the Windows Update Catalog and are not
redistributable; they were used locally only, to study the protocol and
extract tables. Because the matcher is rebuilt rather than original, this
driver cannot be upstreamed to libfprint as it stands; a version built on a
standard open matcher would be the way there. And it has really only been
tried on my one laptop, so expect rough edges elsewhere.

## Credits

This project was built with the help of an AI coding agent: **OpenCode**
(https://opencode.ai) driven by the **oh-my-opencode-slim** setup
(https://github.com/alvinunreal/oh-my-opencode-slim). Much of the protocol work
started from the earlier EH575 effort (`cosct/libfprint-egis0575`), whose
coefficients turned out to be shared across the Egis EH5xx family.

## License

The driver sources are `0BSD` (see `LICENSE`). The reverse-engineering notes
and tooling are provided for interoperability and personal use.
