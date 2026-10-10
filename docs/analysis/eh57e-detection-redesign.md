# EH57E detection redesign — quality-based presence (oracle, ora-4)

## Root cause of the 10–27 s post-verdict stalls
`arm_needs_clear` is set on every verdict (`egis057e_complete_verify`, `egis057e_begin_lift_wait`, `loop_complete`) and cleared
only by a "finger present" predicate that is WRONG for a settled finger:
- `initial_frame_has_finger` requires `gradient>=38 && (stddev>=65 || range>=200)` — a *fresh hard press* signature; a settled
  finger is stddev 24–33 / gradient 42–60 -> returns FALSE for a normal held finger.
- the only other signal is the frame-to-frame `mean_difference >= 3.0`, which oscillates (AGC hunting) 2..49 during a held
  press, so `finger==TRUE` on most frames -> `goto image_done` every frame -> `detection_armed` never TRUE -> the trigger is
  unreachable (and the branch is SILENT — no log).
So detection only fires after a full lift+re-press (frames collapse to clear -> gate clears). First attempt works; later stall.

Aggravator: Phase 4e trigger `image_quality_acceptable && mean_difference <= settle_diff_max(6)` requires near-perfect
stillness, frequently false for seconds even when armed (stalls the trigger AND the settle).

Note: `mean_difference` is frame-to-frame (previous_frame overwritten every callback), not vs baseline.

## Data
Clear frames: stddev ~3, gradient ~6. Finger: stddev 24–33, gradient 42–60. Clean ~8x separation -> presence is a STATIC
per-frame property; use quality, not temporal diff.

## Redesign: one predicate, two levels, four states
Levels:
- CONTACT (loose): `stddev >= 8.0 && gradient_rms >= 12.0` -> "something on the sensor" (arm gate, lift-wait, release wait, settle abort).
- USABLE (= `image_quality_acceptable`): `stddev>=15, gradient>=30, range>=30` -> press trigger (2 consecutive) + delivery.

States:
```
WAITING --2 consecutive USABLE frames--> SETTLE --4 frames collected--> DELIVER --> AWAIT_RELEASE --2 CONTACT-free--> WAITING
SETTLE --2 CONTACT-free--> WAITING (abort; nothing delivered)
verify verdict --> LIFT-WAIT --2 CONTACT-free--> complete (or 8 s timeout; arm_needs_clear stays set)
```
- Trigger: 2 consecutive USABLE frames (`EGIS057E_PRESENCE_FRAMES=2`). A lift cannot produce 2 consecutive USABLE frames.
- Settle: collect `EGIS057E_SETTLING_FRAMES=4` frames; best frame by `quality.score - mean_difference` (temporal diff used ONLY
  here as a motion penalty). Deliver best after 4; if no USABLE frame yet, keep up to window (8) then retry (watchdog bounds).
- Never fires on lift: every level requires *positive* USABLE/CONTACT evidence; post-delivery sits in AWAIT_RELEASE/LIFT-WAIT.
- Latency: trigger <=2 frames, settle 4 -> ~1.5 s; detection reported ~0.5 s after contact.
- Re-arm: keep `arm_needs_clear` but clear it after 2 consecutive CONTACT-free frames. Delete the 4-frame baseline arming
  (`egis057e_arm_frames`, `baseline_sum`, `stable_frames`, `change_threshold`) — no temporal baseline needed, arm immediately
  once the gate clears. Do NOT reset `arm_needs_clear` in `egis057e_start_capture` (persists across actions on purpose).

## Exact code changes (file libfprint/drivers/egis057e.c)
Constants (replace ~lines 46–62):
```
#define EGIS057E_CONTACT_MIN_STDDEV        8.0
#define EGIS057E_CONTACT_MIN_GRADIENT_RMS  12.0
#define EGIS057E_PRESENCE_FRAMES           2
#define EGIS057E_SETTLING_FRAMES           4
#define EGIS057E_TOUCH_SETTLE_MAX_FRAMES   8
#define EGIS057E_RELEASE_FRAMES            2
#define EGIS057E_MIN_IMAGE_STDDEV          15.0
#define EGIS057E_MIN_GRADIENT_RMS          30.0
#define EGIS057E_MIN_INTENSITY_RANGE       30
```
Delete obsolete: EGIS057E_FRAME_CHANGE_MARGIN, CLEAR_MAX_DIFFERENCE, CLEAR_REFRESH_FRAMES, RELEASE_MIN_DIFFERENCE,
RELEASE_THRESHOLD_MULTIPLIER, INITIAL_TOUCH_THRESHOLD, TOUCH_STABLE_MAX_DIFFERENCE, TOUCH_STABLE_FRAMES, INITIAL_FINGER_MIN_*.

Struct: add `guint arm_clear_frames; guint no_contact_frames;`; remove captured_frame/clear_frame/have_*/baseline_sum/
change_threshold/touch_best_difference/touch_stable_frames/clear_stable_frames/resume_release. Keep previous_frame/
have_previous_frame, detection_armed, finger_frames, release_frames, touch_settle_frames, touch_pending,
touch_best_frame/have_touch_best_frame/touch_best_quality, awaiting_release, arm_needs_clear, verify_*.

Helpers: delete `initial_frame_has_finger`; delete env helpers for settle_diff_max/settle_stable_frames/settle_decay/arm_frames
(their env vars are obsolete); keep `egis057e_settle_window`. Add:
```c
static gboolean egis057e_frame_has_contact (const Egis057eImageQuality *q){
  return q->standard_deviation >= EGIS057E_CONTACT_MIN_STDDEV &&
         q->gradient_rms >= EGIS057E_CONTACT_MIN_GRADIENT_RMS;
}
```

`image_recv_cb` `else` block: replace with the state machine:
- lift-wait branch (verify_waiting_lift): CONTACT -> report PRESENT, reset lift frames; else ++lift_frames; if >=2 -> report NONE,
  complete_verify, mark_completed; else if past deadline -> complete + mark_completed; goto image_done.
- arming: if !detection_armed: if arm_needs_clear: if contact -> arm_clear_frames=0, goto image_done; else ++arm_clear_frames;
  if < RELEASE_FRAMES goto image_done; arm_needs_clear=FALSE, log. Then detection_armed=TRUE, log.
- release-wait (awaiting_release): contact->release_frames=0; else ++; if >=2 -> awaiting_release=FALSE, report NONE (remove
  PRESENT), and NEEDED if !action_reported; goto image_done.
- settle (touch_pending): if !contact ++no_contact_frames else =0; if no_contact_frames>=2 -> abort to WAITING (reset
  touch_pending/settle/finger/best), goto image_done. ++touch_settle_frames; if usable -> pick_score=score-mean_difference,
  keep best. Log settle (fp_info EGIS057E_SETTLE elapsed/window). If touch_settle_frames>=SETTLING_FRAMES: if no best -> if
  >=window reset to 0; goto image_done. Else EGIS057E_SETTLE_DONE, reset, report PRESENT (remove NEEDED), deliver_frame; if TRUE
  mark_completed+return; else awaiting_release=TRUE, watchdog_progress.
- waiting: if usable ++finger_frames else =0; if >=PRESENCE_FRAMES -> reset, touch_pending=TRUE, verify_arm if VERIFY,
  watchdog_progress, log EGIS057E_SETTLE_START.

`egis057e_start_capture`: drop resets of deleted fields; add arm_clear_frames=0, no_contact_frames=0; keep all others; do NOT
touch arm_needs_clear. Remove the `resume_release` read in SM_INIT_SEND.

Do NOT change: complete_verify/begin_lift_wait/loop_complete, handle_enroll/handle_verify, dev_verify deadline, watchdog, SSM,
transport. Tool (tools/egis057e_enroll.c) needs NO changes (log contract preserved).

## Validation (monitor, hardware)
1. verdict -> lift -> re-press: rilevato within ~0.5–1 s every time, no stalls.
2. verdict -> hold: no phantom; EGIS057E_VERIFY_LIFT_TIMEOUT at 8 s; arms only after real lift.
3. jittery/rolling press after verdict: triggers immediately.
4. quick tap: nothing delivered, prompt returns.
5. lift never yields SETTLE_START/SETTLE_DONE.
6. 5-stage enroll ok with lift between stages.
7. hold >30 s on fresh verify: bounded exit, no wedge.
