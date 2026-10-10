# EH57E presence baseline redesign — warm per-pixel background (oracle, phase 4i)

## Why
Absolute scalar noise thresholds (stddev / gradient_rms) break in the sensor's
**drift regime**: the idle floor can drift up (stddev ~10.3, still < the 8.0
CONTACT stddev gate is crossed), so an EMA `idle + margin` gate rejected weak
warm presses, and finger frames leaked into the idle EMA. Detection then
stalled ~16 s and the settle aborted (no-press). The proven sibling pure-FpDevice
drivers (`cosct/libfprint-egis0575` EH575, `PHILIPPDEV5396/libfprint-egis0576`
EH576) key presence on a **per-pixel deviation from a warm background**, which
absorbs drift and residue. This phase adopts that model.

Presence is now: `deviation = |frame[i] - bg[i]|` per pixel, threshold
`T = max(FLOOR, K * idle_residual_mad)`, and presence from the **coverage**
(% pixels with deviation >= T). The background `bg` is a per-pixel EMA updated
only on absent frames. USABLE ridge quality (15/30/30) stays static and is used
only for the delivered frame, never for presence.

## Constants (replace the old CONTACT pair; delete `EGIS057E_CAPTURE_POLL_MS`)
```c
/* Per-pixel background baseline (presence authority) */
#define EGIS057E_BG_DEV_FLOOR        18.0  /* min per-pixel deviation gate T */
#define EGIS057E_BG_DEV_K            2.2   /* T = max(FLOOR, K * idle residual mad) */
#define EGIS057E_NOISE_MAD_SEED      4.0
#define EGIS057E_FALLBACK_SD         8.0   /* absolute presence fallback while unseeded */
#define EGIS057E_FALLBACK_GRAD      12.0
#define EGIS057E_PRESENCE_COVERAGE   12.0  /* % pixels beyond T => PRESENT */
#define EGIS057E_ABSENCE_COVERAGE     6.0  /* % => ABSENT + baseline update ok */
#define EGIS057E_BG_EMA_RATE         0.25  /* per absent frame, per pixel */
#define EGIS057E_BG_WARMUP_FRAMES    4
#define EGIS057E_BG_SEED_MAD_MAX     15.0
#define EGIS057E_STUCK_PRESENCE_MS   10000
#define EGIS057E_IDLE_POLL_FAST_MS    250
#define EGIS057E_IDLE_POLL_MID_MS     500
#define EGIS057E_IDLE_POLL_SLOW_MS   1000
```
Also: `EGIS057E_VERIFY_TOTAL_MS 3000 -> 6000` (measured from the first delivered
probe). Unchanged: `EGIS057E_PRESENCE_FRAMES 2` (2 consecutive PRESENT frames),
`EGIS057E_SETTLING_FRAMES 4`, `EGIS057E_RELEASE_FRAMES 2`,
`EGIS057E_TOUCH_SETTLE_MAX_FRAMES 8` (display only), `EGIS057E_VERIFY_START_TIMEOUT_MS 25000`,
lift/watchdog timeouts.

## Presence predicate + hysteresis (+ absolute-gate cross-check)
```c
typedef struct { double mad; double coverage; } Egis057ePresence;
T = MAX(BG_DEV_FLOOR, BG_DEV_K * noise_mad);
abs_gate = quality.stddev >= FALLBACK_SD (8.0) && quality.gradient >= FALLBACK_GRAD (12.0);
if (bg_valid) {
  present = abs_gate && coverage >= PRESENCE_COVERAGE;          /* 12% */
  absent  = !abs_gate || coverage <= ABSENCE_COVERAGE;           /*  6% */
} else {
  present = abs_gate;                 /* never blind before seeding */
  absent  = !abs_gate;
}
```
Semantics: a real finger always passes `abs_gate` (sd 20-40), so an empty or
baseline-contaminated case (sd 3) is ABSENT even if coverage is falsely high;
the per-pixel baseline still supplies drift robustness for idle frames with
elevated sd but low coverage. Between 6% and 12% is a hysteresis band (when
`abs_gate` holds): neither, so state is held; a failed `abs_gate` forces absent.

## Background seed (live-idle only) and persistence
The baseline is seeded **only from live streaming frames** (mode-consistent by
construction) and seeding is **non-blocking** (no `goto image_done`), so the
fallback can still trigger a press while it warms:
- **Idle warmup**: accumulate the running per-pixel mean of the first
  `BG_WARMUP_FRAMES` frames that are clearly idle (`sd < FALLBACK_SD` and
  `grad < FALLBACK_GRAD`); then divide and `bg_valid=TRUE`,
  `fp_info("EGIS057E_BG_SEED idle")`.
- The `0x72` calibration frame (`self->calibration_frame`) is a **different
  image mode/scale** and must **never** seed the baseline (phase 4k removal of
  `EGIS057E_BG_SEED cal`); it is not referenced for seeding at all.
- **Persistence**: `bg_q8`/`bg_valid`/`bg_warmup`/`noise_mad` live at DEVICE level,
  initialised once in `dev_open` and not reset in `start_capture`, so the
  drift-robust baseline survives across actions. The stuck-presence watchdog
  invalidates it (`bg_valid=FALSE; bg_warmup=0; memset`) so it reseeds.

## Per-pixel background update
On ABSENT frames while `bg_valid`: rate 0.25 exact in Q8
`bg += (frame<<8 - bg) >> 2` and `noise_mad = 0.8*noise_mad + 0.2*mad`.
Informational drift warn once per action when `noise_mad` first crosses 9.0.

## State machine rules (image_recv_cb)
- Per frame: `quality = image_quality(buffer)`, `presence = presence_eval(buffer)`,
  `present`/`absent`; a `fp_dbg("presence: mad … cov … T … present=…")` line.
- Global bookkeeping: present → `absent_streak=0`, set `present_since` if unset;
  absent → `absent_streak++`, clear `present_since` at `>= RELEASE_FRAMES`.
- **Lift-wait**: uses `present` (unchanged otherwise).
- **Arm gate**: uses `present`; `arm_needs_clear` semantics intact.
- **Release-wait**: `present` resets `release_frames`, absent increments.
- **Settle**: abort after `>= 2` ABSENT frames. Best frame = USABLE by
  `quality.score - frame_mean_difference`. At `touch_settle_frames >= SETTLING_FRAMES`
  deliver iff `present && have_touch_best_frame`; if `present && !have_best`,
  **continue collecting** (the settle extends while presence persists — no abort
  at the window); abort to WAITING only on the 2-absent rule. On delivery reset
  `present_since=0` and `watchdog_progress()`.
- **WAITING**: lazy seed, then on absent frames update the background
  (+ polish). `idle_absent_streak++` on absent, `=0` on present. Trigger on
  2 consecutive PRESENT frames (no quality requirement) → `SETTLE_START`;
  `verify_arm` is NOT called here.
- **Stuck-presence watchdog** (WAITING + SETTLE only, NEVER in lift-wait): if
  `present_since != 0` and `now - present_since > STUCK_PRESENCE_MS`, log
  `EGIS057E_STUCK_PRESENCE` and complete the action cleanly (verify → existing
  `EGIS057E_NO_PRESS` path; enroll/capture → clean `fpi_ssm_mark_failed`).
  Current frames are never absorbed into the baseline.

## Verify deadlines
`egis057e_verify_arm()` moves from SETTLE_START to the first delivered probe
(inside `handle_verify` after `verify_probe_count++`); `VERIFY_TOTAL_MS` is now
measured from the first probe. Early-MATCH and early-reject unchanged.

## Duty-cycled poll cadence
`next_poll_ms = present ? FAST : (idle_absent_streak < 8 ? FAST : (idle_absent_streak < 80 ? MID : SLOW))`
(250 / 500 / 1000 ms). `SM_CAPTURE_DELAY` uses `self->next_poll_ms`.

## Per-action reset (start_capture)
Resets the per-action counters only: `present_since=0; absent_streak=0;
idle_absent_streak=0; next_poll_ms=FAST`. `bg_q8`/`bg_valid`/`bg_warmup`/`noise_mad`
(after phase 4j) and `arm_needs_clear` are NOT reset — they persist. `bg_q8` is
allocated and initialised once in `dev_open` (Q8, `IMAGE_LEN`) and freed in
`dev_close`.

## Stuck-presence watchdog (phase 4j)
On `EGIS057E_STUCK_PRESENCE`, invalidate the baseline (`bg_valid=FALSE;
bg_warmup=0; memset(bg_q8)`) before completing the action, so it reseeds from
idle frames next time.

## Log contract (tool unchanged)
`EGIS057E_SETTLE_START` / `EGIS057E_SETTLE n m` / `EGIS057E_SETTLE_DONE` are
preserved (the settle bar may show a full bar while the settle extends — fine).
New lines: `EGIS057E_BG_SEED idle`, `presence: …` (dbg),
`EGIS057E_STUCK_PRESENCE` (warn), `sensor idle floor drifted …` (warn).
