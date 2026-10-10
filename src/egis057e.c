/*
 * Egis Technology EH57E driver for libfprint
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Hardware: LighTuning Technology Inc. EgisTec EH57E
 * USB ID:   1c7a:057e
 *
 * Architecture note (PHASE 1 restructure):
 *   This is a pure #FpDevice subclass. It owns the USB transport, the image
 *   calibration/init packet chain and the frame-based touch/release detection
 *   loop. It deliberately does NOT use #FpImageDevice: EH57E frames are only
 *   70x57 and normally contain no NBIS minutiae, so the standard image path
 *   cannot be used. Enrollment/verification are phase-2 work and currently
 *   fail with a clear error; capture is fully functional.
 *
 * Ported from the standalone prototype driver (egis057e-linux-driver) which
 * framed the same protocol on top of FpImageDevice. The transport, framing,
 * calibration chain, capture sequence and touch-detection state machine are
 * ported directly; only the libfprint lifecycle glue changed.
 */

#define FP_COMPONENT "egis057e"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "egis057e.h"
#include "egis057e-matcher.h"
#include "drivers_api.h"

/* -------------------------------------------------------------------------
 * Algorithm tuning constants
 *
 * Conservative experimental baselines, NOT a validated security boundary.
 * Deferred matcher thresholds live in the phase-2 enrollment/verification
 * implementation.
 * ------------------------------------------------------------------------- */

/* Per-pixel background baseline (presence authority) */
#define EGIS057E_BG_DEV_FLOOR        18.0  /* min per-pixel deviation gate T */
#define EGIS057E_BG_DEV_K            2.2   /* T = max(FLOOR, K * idle residual mad) */
#define EGIS057E_NOISE_MAD_SEED      4.0
#define EGIS057E_FALLBACK_SD         8.0   /* absolute presence fallback while unseeded */
#define EGIS057E_FALLBACK_GRAD      12.0
#define EGIS057E_PRESENCE_COVERAGE   12.0  /* % pixels beyond T => PRESENT */
#define EGIS057E_ABSENCE_COVERAGE    10.0  /* % => ABSENT + baseline update ok.
                                            * This sensor's idle residue sits at
                                            * 7-9% cov, so 6% never marked the
                                            * finger lifted (RELEASE_STUCK). */
#define EGIS057E_BG_EMA_RATE         0.25  /* per absent frame, per pixel */
#define EGIS057E_BG_WARMUP_FRAMES    4
#define EGIS057E_BG_SEED_MAD_MAX     15.0
#define EGIS057E_STUCK_PRESENCE_MS   10000
#define EGIS057E_RELEASE_STUCK_MS     4000  /* bound the post-stage lift wait */
#define EGIS057E_LATCH_FRAMES   3   /* consecutive byte-identical polls => latched */
#define EGIS057E_IDLE_POLL_FAST_MS    250
#define EGIS057E_IDLE_POLL_MID_MS     500
#define EGIS057E_IDLE_POLL_SLOW_MS   1000
#define EGIS057E_PRESENCE_FRAMES           2
#define EGIS057E_SETTLING_FRAMES           4
#define EGIS057E_TOUCH_SETTLE_MAX_FRAMES   8
#define EGIS057E_RELEASE_FRAMES            2
#define EGIS057E_MIN_IMAGE_STDDEV          15.0
#define EGIS057E_MIN_GRADIENT_RMS          30.0
#define EGIS057E_MIN_INTENSITY_RANGE       30
/* Weak-touch feedback: a touch the baseline detects (cov/mad above the idle
 * floor) but below the settle trigger never starts a settle and would be
 * completely silent. Non-settling; hint only (ENROLL). */
#define EGIS057E_TOUCH_COVERAGE      10.0  /* weak-touch hint: % cov (idle <=9)  */
#define EGIS057E_TOUCH_MAD            9.5  /*  and mean |dev| (idle <=8)         */
#define EGIS057E_TOUCH_FRAMES           2  /* consecutive weak frames to hint    */
#define EGIS057E_TOUCH_HINT_COOLDOWN_MS 1800  /* re-hint at most this often      */

/* VERIFY must always complete: a finger-held press produces no lift, so the
 * probe-collection loop is time-bounded. */
#define EGIS057E_VERIFY_START_TIMEOUT_MS    25000  /* no press yet -> NO_PRESS */
#define EGIS057E_VERIFY_TOTAL_MS            6000   /* from first probe -> done */
/* After a verdict, wait for the finger to be lifted before completing so the
 * next attempt does not re-read the same press. Bounded; if the finger stays
 * down the action still completes and the next action waits for a clear frame
 * (arm_needs_clear) before arming. */
#define EGIS057E_VERIFY_LIFT_TIMEOUT_MS     8000
#define EGIS057E_VERIFY_LIFT_FRAMES         2      /* clear frames => lifted */

/* Per-action watchdog: if no progress (no finger detection, no delivered
 * frame/stage/probe) within this window, fail the action cleanly so the caller
 * exits and the device is closed/released. No in-driver USBDEVFS_RESET. */
#define EGIS057E_ACTION_TIMEOUT_MS          30000

/* -------------------------------------------------------------------------
 * Enrollment / verification tuning
 *
 * Recalibrate in Phase 3 against real EH57E data: the matcher thresholds are
 * inherited from the validated EH575 pipeline but this sensor is 70x57
 * (EH575 is 103x52).
 * ------------------------------------------------------------------------- */

#define EGIS057E_TEMPLATE_VERSION           1
#define EGIS057E_ENROLL_STAGES              5
#define EGIS057E_VERIFY_PROBE_FRAMES        6   /* array capacity (max probes) */
#define EGIS057E_VERIFY_PROBE_TARGET        3   /* default probes to collect   */
/* Early NO-MATCH: a probe whose best gallery score is clearly below the match
 * threshold (this value) is not going to match on another press either, so
 * finalize NO MATCH immediately instead of collecting more probes / waiting the
 * total deadline. Kept below the genuine near-miss floor so a plausible press
 * in [REJECT_SCORE, MATCH_THRESHOLD) still keeps collecting. */
#define EGIS057E_VERIFY_REJECT_SCORE        800
/* Same-spot reject gate. DEFAULT DISABLED (0): the EH57E matcher score scale is
 * ~2-3x the vendor's, so any positive threshold fired on normal different-
 * placement enrollment frames and looped up to EGIS057E_ENROLL_SIM_MAX_REJECTS
 * retries per stage (looks like a hang in GNOME). This is a PRELIMINARY gate;
 * re-enable/tune without a rebuild via the env override
 * EGIS057E_ENROLL_SIM_THRESHOLD (0 disables). */
#define EGIS057E_ENROLL_SIM_THRESHOLD       0
#define EGIS057E_ENROLL_SIM_MAX_REJECTS     4

/* -------------------------------------------------------------------------
 * Device state
 * ------------------------------------------------------------------------- */

typedef struct
{
  double standard_deviation;
  double gradient_rms;
  guint  intensity_range;
  double score;
} Egis057eImageQuality;

struct _FpDeviceEgis057e
{
  FpDevice    parent;

  FpiSsm     *ssm;

  gboolean    running;
  gboolean    stop;
  gboolean    action_reported;

  gboolean    calibration_valid;

  guint       init_step;        /* current index in egis057e_init_pkts[]  */
  guint       capture_step;     /* current index in capture sequence      */
  guint8      int_ep;           /* interrupt endpoint currently polled    */
  guint8      previous_frame[EGIS057E_IMAGE_LEN];
  guint8      touch_best_frame[EGIS057E_IMAGE_LEN];
  gboolean    have_previous_frame;
  gboolean    have_touch_best_frame;
  gboolean    detection_armed;
  guint       finger_frames;
  guint       release_frames;
  guint       touch_settle_frames;
  gboolean    touch_pending;
  double      touch_best_quality;
  Egis057eImageQuality touch_best_q;
  gboolean    retry_reported;   /* one-shot weak-press retry per touch */
  guint       weak_touch_frames; /* consecutive weak-touch frames (WAITING) */
  guint       weak_gap_frames;  /* consecutive clearly-no-touch frames      */
  gint64      weak_hint_deadline; /* next allowed weak-touch hint (cooldown) */
  gboolean    hint_alt;           /* alternate retry code so GNOME re-shows   */
  guint8      calibration_sample;
  gboolean    have_calibration_sample;

  /* Fresh per-activation 0x72 calibration frame (empty-sensor background),
   * retained for the capture tool / diagnostics. */
  guint8      calibration_frame[EGIS057E_IMAGE_LEN];
  gboolean    have_calibration_frame;

  gboolean    awaiting_release;
  gboolean    reported_image;
  gboolean    capture_protocol_error;

  /* Enrollment state (heap: feature sets exceed the GObject instance size) */
  FpPrint              *enroll_print;   /* borrowed from enroll data        */
  Egis057eMFeatureSet  *enroll_feats;   /* EGIS057E_ENROLL_STAGES           */
  guint8               *enroll_raw;     /* STAGES * EGIS057E_IMAGE_LEN      */
  guint                 enroll_stage;
  guint                 enroll_rejects;

  /* Verification state */
  Egis057eMFeatureSet  *verify_gallery; /* EGIS057E_ENROLL_STAGES           */
  guint                 verify_gallery_n;
  Egis057eMFeatureSet  *verify_probes;  /* EGIS057E_VERIFY_PROBE_FRAMES     */
  guint                 verify_probe_count;
  guint8                verify_cal_sample;
  gint64                verify_deadline; /* monotonic us; 0 = none          */
  gboolean              verify_armed;     /* total deadline from first press */
  gboolean              verify_verdict_done; /* verdict already reported     */
  gboolean              verify_waiting_lift; /* waiting for finger to lift   */
  guint                 verify_lift_frames;  /* consecutive clear frames     */
  gint64                verify_lift_deadline;/* lift-wait bound              */
  gboolean              arm_needs_clear;  /* require a clear frame to arm    */
  guint                 arm_clear_frames;  /* consecutive ABSENT frames       */
  guint                 no_contact_frames; /* settle-abort counter            */
  gint64                action_deadline;  /* per-action watchdog progress    */
  guint                 identical_frames;    /* consecutive byte-identical polls   */
  guint                 latch_recoveries;    /* in-action re-init attempts (max 1) */
  gint64                awaiting_release_since; /* release-wait bound anchor       */

  /* Per-pixel background baseline (presence authority) and idle cadence. */
  guint32              *bg_q8;            /* heap: IMAGE_LEN per-pixel bg, Q8 */
  gboolean              bg_valid;
  guint                 bg_warmup;        /* remaining live-warmup frames     */
  double                noise_mad;        /* idle residual EMA                */
  gint64                present_since;    /* 0 = not present                  */
  guint                 absent_streak;
  guint                 idle_absent_streak;
  guint                 next_poll_ms;
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis057e, fpi_device_egis057e,
                      FPI, DEVICE_EGIS057E, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis057e, fpi_device_egis057e, FP_TYPE_DEVICE);

/* -------------------------------------------------------------------------
 * SSM states
 * ------------------------------------------------------------------------- */

enum sm_states {
  /* Init chain */
  SM_INIT_SEND,       /* send current init step command         */
  SM_INIT_RECV,       /* receive response (conditional)         */
  SM_INIT_NEXT,       /* advance to next step or finish         */

  /* Detect / capture loop */
  SM_CAPTURE_DELAY,   /* interactive grace period after baseline */
  SM_DETECT_WAIT,     /* wait for finger interrupt on EP 0x83   */
  SM_CAPTURE_SEND,    /* send current capture trigger command   */
  SM_CAPTURE_RECV,    /* receive command response               */
  SM_CAPTURE_NEXT,    /* advance capture trigger command        */
  SM_CAPTURE_REQUEST, /* request one 0x0f96-byte image          */
  SM_CAPTURE_IMAGE,   /* receive provisional image frame        */
  SM_CAPTURE_FINISH,  /* acknowledge completion                 */
  SM_CAPTURE_FINISH_RECV,
  SM_DETECT_DONE,     /* restart loop                           */

  SM_STATES_NUM
};

/* -------------------------------------------------------------------------
 * Command packet tables
 * ------------------------------------------------------------------------- */

/*
 * Confirmed ET5XX command packets recovered from EgisTouchFP057E.dll and
 * validated directly against an EH57E.
 */
const Egis057ePacket egis057e_init_pkts[EGIS057E_INIT_PKT_COUNT] = {
  /* A completed capture leaves 0x40 at 0x83.  The vendor recovery sequence
   * is harmless from the idle state and makes activation deterministic. */
  { { 'E', 'G', 'I', 'S', 0x61, 0x0a, 0xf4 }, 7, "recover 0x0a", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0c, 0x44 }, 7, "recover 0x0c", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x40, 0x00 }, 7, "recover 0x40", TRUE },
  { { 'E', 'G', 'I', 'S', 0x71, 0x02, 0x02, 0x01, 0x0c },
    9, "recover xfer_buf 0x02", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x09, 0x0b,
      0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x00, 0x00, 0x52 },
    18, "image calibration mode", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x2c, 0x02, 0x00, 0x57 },
    9, "image calibration sample", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x2d, 0x00 },
    7, "calibration poll 0x2d", TRUE },
  { { 'E', 'G', 'I', 'S', 0x62, 0x67, 0x03 },
    7, "calibration read_buf 0x67", TRUE },
  /* 0x6d is the stable value observed in the successful image-path trials.
   * A follow-up callback substitutes byte 7 of the preceding response. */
  { { 'E', 'G', 'I', 'S', 0x63, 0x33, 0x03, 0x6d, 0x10, 0x01 },
    10, "image calibration apply", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x35, 0x00 }, 7, "poll calibration", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0a, 0xf4 }, 7, "image power 0x0a", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x0c, 0x44 }, 7, "image power 0x0c", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x50, 0x01 }, 7, "image power 0x50", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x50, 0x00 }, 7, "poll image power", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x54, 0x03, 0x01, 0x22, 0x1c },
    10, "enable calibration image", TRUE },
  /* Opcode 0x72 returns a raw 3990-byte calibration frame, not SIGE. */
  { { 'E', 'G', 'I', 'S', 0x72, 0x0f, 0x96 },
    7, "read calibration image", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x54, 0x00 }, 7, "end calibration image", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x09, 0x0b,
      0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12 },
    18, "normal image mode", TRUE },
  { { 'E', 'G', 'I', 'S', 0x63, 0x26, 0x06,
      0x0e, 0x36, 0x04, 0x0a, 0x2e, 0x04 }, 13, "image window", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x23, 0x00 }, 7, "image reg 0x23", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x24, 0x38 }, 7, "image reg 0x24", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x20, 0x00 }, 7, "image reg 0x20", TRUE },
  { { 'E', 'G', 'I', 'S', 0x61, 0x21, 0x45 }, 7, "image reg 0x21", TRUE },
};

const Egis057ePacket egis057e_capture_pkts[EGIS057E_CAPTURE_PKT_COUNT] = {
  { { 'E', 'G', 'I', 'S', 0x63, 0x2c, 0x02, 0x00, 0x13 },
    9, "arm one live frame", TRUE },
  { { 'E', 'G', 'I', 'S', 0x60, 0x00, 0x00 },
    7, "poll live frame", TRUE },
};

static void
egis057e_log_bytes (const char *prefix, const guint8 *buf, gsize len)
{
  g_autoptr(GString) s = g_string_new (NULL);

  for (gsize i = 0; i < len; i++)
    g_string_append_printf (s, "%s%02x", i ? " " : "", buf[i]);

  fp_dbg ("%s%s", prefix, s->str);
}

/* -------------------------------------------------------------------------
 * Finger-status reporting (delta-only, like egis0575)
 *
 * Proven pure-FpDevice drivers never call the bare
 * fpi_device_report_finger_status(), which REPLACES the whole flag set and can
 * wipe PRESENT; they always apply deltas via
 * fpi_device_report_finger_status_changes(). These helpers keep PRESENT and
 * NEEDED independent: report_needed() starts an action wanting a finger while
 * clearing any stale PRESENT; report_present() only toggles PRESENT and leaves
 * NEEDED as-is.
 * ------------------------------------------------------------------------- */

static void
egis057e_report_present (FpDevice *dev, gboolean present)
{
  /* Delta-style, like egis0575: toggles PRESENT, leaves NEEDED as-is. */
  fpi_device_report_finger_status_changes (dev,
    present ? FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NONE,
    present ? FP_FINGER_STATUS_NONE    : FP_FINGER_STATUS_PRESENT);
}

static void
egis057e_report_needed (FpDevice *dev)
{
  /* Action start: waiting for a finger; clear any stale PRESENT. */
  fpi_device_report_finger_status_changes (dev,
    FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_PRESENT);
}

/* GNOME latches identical retry text (a repeated same-code hint is visually
 * invisible), so alternate the retry code across hint emissions to keep
 * consecutive messages distinct. Never reset: it must alternate across
 * attempts. */
static FpDeviceRetry
egis057e_next_hint_code (FpDeviceEgis057e *self)
{
  FpDeviceRetry code = self->hint_alt ? FP_DEVICE_RETRY_GENERAL
                                      : FP_DEVICE_RETRY_CENTER_FINGER;

  self->hint_alt = !self->hint_alt;
  return code;
}

/* -------------------------------------------------------------------------
 * USB transport helpers (EGIS / SIGE framing)
 * ------------------------------------------------------------------------- */

static void
int_finger_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer user_data, GError *error)
{
  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_CANCELLED))
        {
          g_error_free (error);
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

          if (self->int_ep == EGIS057E_EP_INT_FINGER)
            {
              self->int_ep = EGIS057E_EP_INT_ALT;
              g_error_free (error);
              fpi_ssm_jump_to_state (transfer->ssm, SM_DETECT_WAIT);
              return;
            }

          self->int_ep = EGIS057E_EP_INT_FINGER;
          g_error_free (error);
          fp_dbg ("interrupt wait timed out; continuing finger wait");
          fpi_ssm_jump_to_state (transfer->ssm, SM_DETECT_WAIT);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  egis057e_log_bytes ("interrupt payload: ", transfer->buffer,
                      transfer->actual_length);
  egis057e_report_present (dev, TRUE);
  fpi_ssm_next_state (transfer->ssm);
}

static void
cmd_resp_cb (FpiUsbTransfer *transfer, FpDevice *dev,
             gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  guint expected_length = GPOINTER_TO_UINT (user_data);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != expected_length)
    {
      fpi_ssm_mark_failed (
        transfer->ssm,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                  "Unexpected command response length %zd (expected %u)",
                                  transfer->actual_length, expected_length));
      return;
    }

  if (transfer->actual_length < EGIS057E_RESP_LEN ||
      memcmp (transfer->buffer, "SIGE", 4) != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Malformed command response"));
      return;
    }

  if (transfer->buffer[6] != 0x01)
    {
      fpi_ssm_mark_failed (
        transfer->ssm,
        fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                  "Device rejected command (status 0x%02x)",
                                  transfer->buffer[6]));
      return;
    }

  egis057e_log_bytes ("command response: ", transfer->buffer,
                      transfer->actual_length);

  /* read_buf(0x67, 3) returns the per-activation calibration sample as
   * the first payload byte.  Keep it per device instance and substitute it
   * into the following write_buf(0x33, ...) command. */
  if (self->init_step < EGIS057E_INIT_PKT_COUNT &&
      egis057e_init_pkts[self->init_step].bytes[4] == 0x62 &&
      egis057e_init_pkts[self->init_step].bytes[5] == 0x67 &&
      transfer->actual_length >= EGIS057E_RESP_LEN + 1)
    {
      self->calibration_sample = transfer->buffer[EGIS057E_RESP_LEN];
      self->have_calibration_sample = TRUE;
      fp_dbg ("runtime image calibration sample: 0x%02x",
              self->calibration_sample);
    }
  fpi_ssm_next_state (transfer->ssm);
}

/* Write a 70x57 raw frame as a binary PGM, created owner-only from the first
 * byte (biometric data must not be briefly group/world-readable). */
static gboolean
egis057e_write_pgm_0600 (const gchar *path, const guint8 *data, gsize len)
{
  gchar header[64];
  gint header_len;
  gint fd;
  gsize off = 0;

  if (len != EGIS057E_IMAGE_LEN)
    return FALSE;

  fd = g_open (path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    {
      fp_warn ("could not create %s: %s", path, g_strerror (errno));
      return FALSE;
    }
  fchmod (fd, 0600);

  header_len = g_snprintf (header, sizeof (header), "P5 %u %u 255\n",
                           EGIS057E_IMAGE_WIDTH, EGIS057E_IMAGE_HEIGHT);
  if (write (fd, header, (size_t) header_len) != (ssize_t) header_len)
    {
      fp_warn ("short write to %s", path);
      close (fd);
      return FALSE;
    }

  while (off < len)
    {
      ssize_t n = write (fd, data + off, len - off);

      if (n < 0)
        {
          fp_warn ("write failed %s: %s", path, g_strerror (errno));
          close (fd);
          return FALSE;
        }
      off += (gsize) n;
    }

  close (fd);
  return TRUE;
}

static void
calibration_image_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                      gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("calibration image read returned %zd/%d bytes",
          transfer->actual_length, EGIS057E_IMAGE_LEN);
  if (transfer->actual_length != EGIS057E_IMAGE_LEN)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Short calibration image"));
      return;
    }

  /* Retain the fresh per-activation 0x72 frame as the empty-sensor background.
   * The capture/enroll/verify actions each run the init chain, so this is a new
   * frame per activation. Optional debug dump for the capture tool. */
  memcpy (self->calibration_frame, transfer->buffer, EGIS057E_IMAGE_LEN);
  self->have_calibration_frame = TRUE;
  {
    const gchar *dump_path = g_getenv ("EGIS057E_BG_DUMP_PATH");

    if (dump_path && dump_path[0])
      egis057e_write_pgm_0600 (dump_path, self->calibration_frame,
                               EGIS057E_IMAGE_LEN);
  }

  fpi_ssm_next_state (transfer->ssm);
}

static void
send_packet (FpiSsm *ssm, FpDevice *dev, const Egis057ePacket *pkt)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
  guint8 *buf = g_memdup2 (pkt->bytes, pkt->len);

  fp_dbg ("sending %s", pkt->name);
  egis057e_log_bytes ("packet: ", pkt->bytes, pkt->len);

  fpi_usb_transfer_fill_bulk_full (t, EGIS057E_EP_OUT, buf, pkt->len,
                                   g_free);
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD,
                           fpi_device_get_cancellable (dev),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
recv_command_response (FpiSsm *ssm, FpDevice *dev, guint length)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, length);
  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD,
                           fpi_device_get_cancellable (dev),
                           cmd_resp_cb, GUINT_TO_POINTER (length));
}

static void
recv_calibration_image (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, EGIS057E_IMAGE_LEN);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD,
                           fpi_device_get_cancellable (dev),
                           calibration_image_cb, NULL);
}

/* -------------------------------------------------------------------------
 * Image quality metrics
 * ------------------------------------------------------------------------- */

static void
frame_mean_difference (const guint8 *a, const guint8 *b, double *result)
{
  guint64 difference = 0;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    difference += ABS ((gint) a[i] - (gint) b[i]);
  *result = (double) difference / EGIS057E_IMAGE_LEN;
}

static Egis057eImageQuality
image_quality (const guint8 *image)
{
  Egis057eImageQuality quality = { 0 };
  guint histogram[256] = { 0 };
  guint64 sum = 0, squared_sum = 0, gradient_squared_sum = 0;
  guint gradient_count = 0, below = 0;
  guint low = 0, high = 255;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    {
      guint value = image[i];
      histogram[value]++;
      sum += value;
      squared_sum += value * value;
    }

  for (guint y = 1; y + 1 < EGIS057E_IMAGE_HEIGHT; y++)
    for (guint x = 1; x + 1 < EGIS057E_IMAGE_WIDTH; x++)
      {
        gint gx = image[y * EGIS057E_IMAGE_WIDTH + x + 1] -
                  image[y * EGIS057E_IMAGE_WIDTH + x - 1];
        gint gy = image[(y + 1) * EGIS057E_IMAGE_WIDTH + x] -
                  image[(y - 1) * EGIS057E_IMAGE_WIDTH + x];
        gradient_squared_sum += gx * gx + gy * gy;
        gradient_count++;
      }

  for (low = 0; low < 255; low++)
    {
      below += histogram[low];
      if (below >= EGIS057E_IMAGE_LEN / 20)
        break;
    }
  below = 0;
  for (high = 255; high > 0; high--)
    {
      below += histogram[high];
      if (below >= EGIS057E_IMAGE_LEN / 20)
        break;
    }

  {
    double mean = (double) sum / EGIS057E_IMAGE_LEN;
    double variance = (double) squared_sum / EGIS057E_IMAGE_LEN - mean * mean;

    quality.standard_deviation = sqrt (MAX (variance, 0.0));
  }
  quality.gradient_rms = sqrt ((double) gradient_squared_sum / gradient_count);
  quality.intensity_range = high > low ? high - low : 0;
  quality.score = quality.gradient_rms + quality.intensity_range;
  return quality;
}

static gboolean
image_quality_acceptable (const Egis057eImageQuality *quality)
{
  return quality->standard_deviation >= EGIS057E_MIN_IMAGE_STDDEV &&
         quality->gradient_rms >= EGIS057E_MIN_GRADIENT_RMS &&
         quality->intensity_range >= EGIS057E_MIN_INTENSITY_RANGE;
}

typedef struct { double mad; double coverage; } Egis057ePresence;

/* Per-pixel deviation threshold T = max(FLOOR, K * idle residual mad). */
static double
egis057e_presence_threshold (FpDeviceEgis057e *self)
{
  return MAX (EGIS057E_BG_DEV_FLOOR, EGIS057E_BG_DEV_K * self->noise_mad);
}

/* Presence = coverage of pixels deviating beyond T from the warm background. */
static Egis057ePresence
egis057e_presence_eval (FpDeviceEgis057e *self, const guint8 *frame)
{
  Egis057ePresence p = { 0, 0 };
  guint dev_count = 0;
  guint64 mad_sum = 0;
  double t = egis057e_presence_threshold (self);

  if (!self->bg_valid)
    return p;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    {
      gint diff = (gint) frame[i] - (gint) (self->bg_q8[i] >> 8);
      guint a = ABS (diff);

      mad_sum += a;
      if ((double) a >= t)
        dev_count++;
    }

  p.mad = (double) mad_sum / EGIS057E_IMAGE_LEN;
  p.coverage = 100.0 * dev_count / EGIS057E_IMAGE_LEN;
  return p;
}

static gboolean
egis057e_frame_present (const Egis057ePresence *p)
{
  return p->coverage >= EGIS057E_PRESENCE_COVERAGE;
}

static gboolean
egis057e_frame_absent (const Egis057ePresence *p)
{
  return p->coverage <= EGIS057E_ABSENCE_COVERAGE;
}

/* Per-pixel background EMA + idle residual EMA. Called on ABSENT frames only.
 * Rate 0.25 (shift 2) normally, 0.5 (shift 1) during post-seed warmup polish. */
static void
egis057e_bg_update (FpDeviceEgis057e *self, const guint8 *frame, double mad)
{
  gint shift = self->bg_warmup > 0 ? 1 : 2;

  for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
    {
      gint delta = (gint) (((guint32) frame[i] << 8) - self->bg_q8[i]);

      self->bg_q8[i] = (guint32) ((gint) self->bg_q8[i] + (delta >> shift));
    }

  if (self->bg_warmup > 0)
    self->bg_warmup--;

  self->noise_mad = 0.8 * self->noise_mad + 0.2 * mad;
}

/* -------------------------------------------------------------------------
 * Enrollment / verification helpers
 * ------------------------------------------------------------------------- */

/* Template GVariant (yaayy): format version, raw frames, calibration sample.
 * Any matcher/geometry change bumps EGIS057E_TEMPLATE_VERSION so old prints
 * fail fast with a clear re-enroll error. */
static GVariant *
egis057e_pack_template (const guint8 *frames, guint n_frames, guint8 cal_sample)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("(yaayy)"));
  g_variant_builder_add_value (&builder,
                               g_variant_new_byte (EGIS057E_TEMPLATE_VERSION));
  g_variant_builder_open (&builder, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < n_frames; i++)
    g_variant_builder_add_value (
      &builder,
      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                 frames + (gsize) i * EGIS057E_IMAGE_LEN,
                                 EGIS057E_IMAGE_LEN, 1));
  g_variant_builder_close (&builder);
  g_variant_builder_add_value (&builder, g_variant_new_byte (cal_sample));

  return g_variant_builder_end (&builder);
}

/* Unpack the template into feature sets (extracted from the stored raw frames)
 * and return the runtime calibration sample. */
static gboolean
egis057e_unpack_template (GVariant            *data,
                          Egis057eMFeatureSet *gallery,
                          guint               *gallery_n,
                          guint8              *cal_out)
{
  g_autoptr(GVariant) v_version = NULL;
  g_autoptr(GVariant) v_frames = NULL;
  g_autoptr(GVariant) v_cal = NULL;
  guint8 version, cal;
  guint n;

  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(yaayy)")))
    {
      fp_warn ("Enrolled EH57E print is not an EH57E template; re-enroll");
      return FALSE;
    }

  v_version = g_variant_get_child_value (data, 0);
  v_frames = g_variant_get_child_value (data, 1);
  v_cal = g_variant_get_child_value (data, 2);

  version = g_variant_get_byte (v_version);
  cal = g_variant_get_byte (v_cal);

  if (version != EGIS057E_TEMPLATE_VERSION)
    {
      fp_warn ("Enrolled EH57E print uses template version %u (expected %u); re-enroll",
               version, EGIS057E_TEMPLATE_VERSION);
      return FALSE;
    }

  n = g_variant_n_children (v_frames);
  if (n == 0 || n > EGIS057E_ENROLL_STAGES)
    {
      fp_warn ("Enrolled EH57E print has %u frames (expected 1..%d); re-enroll",
               n, EGIS057E_ENROLL_STAGES);
      return FALSE;
    }

  for (guint i = 0; i < n; i++)
    {
      g_autoptr(GVariant) child = g_variant_get_child_value (v_frames, i);
      gsize len = 0;
      const guint8 *bytes = g_variant_get_fixed_array (child, &len, 1);

      if (len != EGIS057E_IMAGE_LEN)
        {
          fp_warn ("Enrolled EH57E frame %u has %zu bytes (expected %d); re-enroll",
                   i, len, EGIS057E_IMAGE_LEN);
          return FALSE;
        }
      egis057e_m_extract (bytes, EGIS057E_IMAGE_WIDTH, EGIS057E_IMAGE_HEIGHT,
                          &gallery[i]);
    }

  *gallery_n = n;
  if (cal_out)
    *cal_out = cal;
  return TRUE;
}

/* Runtime-tunable verdict thresholds (defaults = compile-time constants). */
static gint egis057e_env_int (const gchar *name, gint def);

/* Settle window (env-tunable): max frames the settle may run before retrying. */
static gint
egis057e_settle_window (void)
{
  return MAX (egis057e_env_int ("EGIS057E_SETTLE_WINDOW",
                                EGIS057E_TOUCH_SETTLE_MAX_FRAMES), 1);
}

static gint
egis057e_verify_total_ms (void)
{
  return MAX (egis057e_env_int ("EGIS057E_VERIFY_TOTAL_MS",
                                EGIS057E_VERIFY_TOTAL_MS), 1);
}

static guint
egis057e_verify_probe_target (void)
{
  gint v = egis057e_env_int ("EGIS057E_VERIFY_PROBES",
                             EGIS057E_VERIFY_PROBE_TARGET);

  if (v < 1)
    v = 1;
  if (v > EGIS057E_VERIFY_PROBE_FRAMES)
    v = EGIS057E_VERIFY_PROBE_FRAMES;
  return (guint) v;
}

static gint
egis057e_verify_reject_score (void)
{
  return egis057e_env_int ("EGIS057E_VERIFY_REJECT_SCORE",
                           EGIS057E_VERIFY_REJECT_SCORE);
}

static gint
egis057e_verify_lift_timeout_ms (void)
{
  return MAX (egis057e_env_int ("EGIS057E_VERIFY_LIFT_TIMEOUT_MS",
                                EGIS057E_VERIFY_LIFT_TIMEOUT_MS), 1000);
}

/* Arm the TOTAL verify deadline once, at the first detected press. It is not
 * extended per probe, so a non-matching verify finalizes NO MATCH promptly
 * (~EGIS057E_VERIFY_TOTAL_MS after the first press). */
static void
egis057e_verify_arm (FpDeviceEgis057e *self)
{
  if (!self->verify_armed)
    {
      self->verify_armed = TRUE;
      self->verify_deadline = g_get_monotonic_time () +
                              egis057e_verify_total_ms () * 1000;
    }
}

static gint
egis057e_action_timeout_ms (void)
{
  return MAX (egis057e_env_int ("EGIS057E_ACTION_TIMEOUT_MS",
                                EGIS057E_ACTION_TIMEOUT_MS), 1000);
}

/* Restart the per-action watchdog: called on any real progress (finger
 * detection / delivered frame). */
static void
egis057e_watchdog_progress (FpDeviceEgis057e *self)
{
  self->action_deadline = g_get_monotonic_time () +
                          egis057e_action_timeout_ms () * 1000;
}

static gboolean
egis057e_watchdog_expired (FpDeviceEgis057e *self)
{
  return self->action_deadline != 0 &&
         g_get_monotonic_time () > self->action_deadline;
}

/* Verdict: best gallery score clears the threshold AND at least AGREE_FRAMES
 * gallery frames agree above the agree floor. */
static gboolean
probe_matches_gallery (const Egis057eMFeatureSet *probe,
                       const Egis057eMFeatureSet *gallery,
                       guint                      gallery_n,
                       int                       *best_out)
{
  int best = 0;
  guint agree = 0;
  gint thr = egis057e_env_int ("EGIS057E_M_MATCH_THRESHOLD",
                               EGIS057E_M_MATCH_THRESHOLD);
  gint agree_score = egis057e_env_int ("EGIS057E_M_AGREE_SCORE",
                                       EGIS057E_M_AGREE_SCORE);
  gint agree_frames = egis057e_env_int ("EGIS057E_M_AGREE_FRAMES",
                                        EGIS057E_M_AGREE_FRAMES);

  if (agree_frames < 1)
    agree_frames = 1;

  for (guint g = 0; g < gallery_n; g++)
    {
      int s = egis057e_m_score (probe, &gallery[g], NULL);

      if (s > best)
        best = s;
      if (s >= agree_score)
        agree++;
    }

  if (best_out)
    *best_out = best;

  return best >= thr && (gint) agree >= agree_frames;
}

/* Report the verify verdict to the API (fires the match callback) without
 * completing the action; used before the wait-for-lift. Returns FALSE and
 * reports nothing when no probe was collected (single choke point preventing a
 * phantom NO-MATCH verdict from a no-press action). */
static gboolean
egis057e_report_verify_verdict (FpDeviceEgis057e *self, FpDevice *dev)
{
  gboolean match = FALSE;
  int best_overall = 0;

  if (self->verify_probe_count == 0)
    {
      fp_warn ("Verify completed with no probes (no quality press in this action); refusing to report a verdict");
      return FALSE;
    }

  for (guint p = 0; p < self->verify_probe_count; p++)
    {
      int best = 0;

      if (probe_matches_gallery (&self->verify_probes[p], self->verify_gallery,
                                 self->verify_gallery_n, &best))
        match = TRUE;
      if (best > best_overall)
        best_overall = best;
    }

  fp_info ("Verify (multi-frame): probes=%u best_score=%d/%d => %s",
           self->verify_probe_count, best_overall,
           egis057e_env_int ("EGIS057E_M_MATCH_THRESHOLD",
                             EGIS057E_M_MATCH_THRESHOLD),
           match ? "MATCH" : "NO-MATCH");

  self->verify_verdict_done = TRUE;
  /* NULL print: our probe template never equals the enrolled one and a
   * non-NULL print makes fprintd warn "scanned print that is not matching". */
  fpi_device_verify_report (dev,
                            match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                            NULL,
                            NULL);
  return TRUE;
}

/* Complete the verify action (reporting the verdict first if needed). It does
 * NOT set arm_needs_clear: the lift-wait already confirmed the sensor clear on
 * the normal LIFTED path, so the next action must arm immediately. The arm gate
 * is set by the caller ONLY on the lift TIMEOUT path (finger still down).
 * A no-press action completes with EGIS057E_NO_PRESS instead of a phantom
 * NO-MATCH verdict. */
static gboolean
egis057e_complete_verify (FpDeviceEgis057e *self, FpDevice *dev)
{
  if (!self->verify_verdict_done)
    {
      if (self->verify_probe_count == 0)
        {
          fp_info ("EGIS057E_NO_PRESS");
          self->verify_waiting_lift = FALSE;
          self->verify_lift_frames = 0;
          self->verify_lift_deadline = 0;
          self->verify_deadline = 0;
          self->action_reported = TRUE;
          fpi_device_verify_complete (dev,
            fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
              "EGIS057E_NO_PRESS: no usable press completed within the verify window"));
          egis057e_report_present (dev, FALSE);
          return TRUE;
        }
      egis057e_report_verify_verdict (self, dev);
    }

  self->verify_waiting_lift = FALSE;
  self->verify_lift_frames = 0;
  self->verify_lift_deadline = 0;
  self->verify_deadline = 0;
  self->action_reported = TRUE;
  fpi_device_verify_complete (dev, NULL);
  egis057e_report_present (dev, FALSE);
  return TRUE;
}

/* A verdict has been decided but the finger may still be down: report it
 * promptly, then keep the capture loop alive waiting for the finger to lift
 * (bounded), so the next attempt starts from a lift. arm_needs_clear is set
 * later, only if the wait times out with the finger still down. */
static void
egis057e_begin_lift_wait (FpDeviceEgis057e *self, FpDevice *dev)
{
  self->verify_verdict_done = FALSE;
  egis057e_report_verify_verdict (self, dev);

  /* The probe total deadline no longer applies; the lift wait has its own. */
  self->verify_deadline = 0;
  self->verify_waiting_lift = TRUE;
  self->verify_lift_frames = 0;
  self->verify_lift_deadline = g_get_monotonic_time () +
                               egis057e_verify_lift_timeout_ms () * 1000;
  fp_info ("EGIS057E_VERIFY_WAIT_LIFT");
}

/* Verify retry-exit: the ONE report of this action is a retry; the action
 * completes immediately after it and fprintd (a) shows the hint via
 * VerifyStatus(done=FALSE) and (b) auto-restarts a fresh verify action.
 * The core allows exactly one verify_report per action (result_reported
 * guard) — never report anything else after this. arm_needs_clear makes the
 * restarted action wait for a clear frame, so a still-held finger cannot be
 * re-read into a restart loop. */
static void
egis057e_verify_retry_exit (FpDeviceEgis057e *self, FpDevice *dev,
                            FpDeviceRetry   code,
                            const char      *msg)
{
  GError *error = fpi_device_retry_new_msg (code, "%s", msg);

  self->verify_waiting_lift = FALSE;
  self->verify_lift_frames = 0;
  self->verify_lift_deadline = 0;
  self->verify_deadline = 0;
  self->verify_verdict_done = TRUE;   /* the single report slot is used */
  self->action_reported = TRUE;
  self->arm_needs_clear = TRUE;       /* next action arms only after a clear frame */
  fp_info ("EGIS057E_VERIFY_RETRY exit (code %d)", (gint) code);
  fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
  fpi_device_verify_complete (dev, NULL);
}

static guint
egis057e_enroll_sim_threshold (void)
{
  const gchar *e = g_getenv ("EGIS057E_ENROLL_SIM_THRESHOLD");

  if (e && *e)
    {
      gint64 v = g_ascii_strtoll (e, NULL, 10);

      if (v >= 0 && v <= G_MAXUINT)
        return (guint) v;
    }
  return EGIS057E_ENROLL_SIM_THRESHOLD;
}

/* Runtime-tunable verdict thresholds (defaults = compile-time constants). */
static gint
egis057e_env_int (const gchar *name, gint def)
{
  const gchar *e = g_getenv (name);

  if (e && *e)
    {
      gint64 v = g_ascii_strtoll (e, NULL, 10);

      if (v >= G_MININT && v <= G_MAXINT)
        return (gint) v;
    }
  return def;
}

static gboolean
egis057e_handle_enroll (FpDeviceEgis057e *self, FpDevice *dev,
                        const guint8 *frame)
{
  Egis057eImageQuality quality = image_quality (frame);
  Egis057eMFeatureSet *fs;
  gint stages = fp_device_get_nr_enroll_stages (dev);

  if (stages <= 0 || (guint) stages > EGIS057E_ENROLL_STAGES)
    stages = EGIS057E_ENROLL_STAGES;

  if (!image_quality_acceptable (&quality))
    {
      fp_dbg ("enroll: image below quality gate (stddev %.2f, gradient %.2f, range %u)",
              quality.standard_deviation, quality.gradient_rms,
              quality.intensity_range);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new_msg (egis057e_next_hint_code (self),
                                                            "Fingerprint image has insufficient ridge detail; reposition and retry"));
      return FALSE;
    }

  fs = &self->enroll_feats[self->enroll_stage];
  egis057e_m_extract (frame, EGIS057E_IMAGE_WIDTH, EGIS057E_IMAGE_HEIGHT, fs);

  /* Same-spot repeat check. The slot is overwritten by the next attempt, so a
   * rejection simply does not advance the stage. After
   * EGIS057E_ENROLL_SIM_MAX_REJECTS consecutive rejects the gate is relaxed
   * (accept the frame) so enrollment can always finish. Threshold is
   * env-tunable (see egis057e_enroll_sim_threshold). */
  if (self->enroll_stage > 0 &&
      self->enroll_rejects < EGIS057E_ENROLL_SIM_MAX_REJECTS)
    {
      guint sim_thr = egis057e_enroll_sim_threshold ();
      int best_sim = 0;

      for (guint i = 0; i < self->enroll_stage; i++)
        {
          int sim = egis057e_m_score (fs, &self->enroll_feats[i], NULL);

          if (sim > best_sim)
            best_sim = sim;
        }

      if (sim_thr > 0 && best_sim >= (int) sim_thr)
        {
          self->enroll_rejects++;
          fp_info ("Enroll frame rejected as same-spot repeat (sim=%d >= %u, reject %u/%u); move the finger",
                   best_sim, sim_thr,
                   self->enroll_rejects, EGIS057E_ENROLL_SIM_MAX_REJECTS);
          fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                      fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                                "Same finger placement as an earlier stage; move to a different area and retry"));
          return FALSE;
        }
    }

  self->enroll_rejects = 0;
  memcpy (self->enroll_raw + (gsize) self->enroll_stage * EGIS057E_IMAGE_LEN,
          frame, EGIS057E_IMAGE_LEN);
  self->enroll_stage++;

  fpi_device_enroll_progress (dev, self->enroll_stage, self->enroll_print, NULL);
  fp_info ("Enroll stage %u/%d captured (%d features)",
           self->enroll_stage, stages, fs->n);

  /* A non-final accepted stage is followed by the settle's awaiting_release
   * lift wait; no retry hint must follow a successful stage (GNOME reacts
   * inconsistently to a retry mapped to enroll-retry-scan after acceptance). */
  if (self->enroll_stage < (guint) stages)
    return FALSE;

  {
    GVariant *tmpl = egis057e_pack_template (self->enroll_raw, self->enroll_stage,
                                             self->have_calibration_sample ?
                                               self->calibration_sample : 0x6d);

    fpi_print_set_type (self->enroll_print, FPI_PRINT_RAW);
    g_object_set (self->enroll_print, "fpi-data", tmpl, NULL);
  }

  self->action_reported = TRUE;
  fpi_device_enroll_complete (dev, g_object_ref (self->enroll_print), NULL);
  egis057e_report_present (dev, FALSE);
  return TRUE;
}

static gboolean
egis057e_handle_verify (FpDeviceEgis057e *self, FpDevice *dev,
                        const guint8 *frame)
{
  Egis057eImageQuality quality = image_quality (frame);
  Egis057eMFeatureSet *fs;

  if (!image_quality_acceptable (&quality))
    {
      fp_dbg ("verify: image below quality gate; continuing to poll");
      return FALSE;
    }

  fs = &self->verify_probes[self->verify_probe_count];
  egis057e_m_extract (frame, EGIS057E_IMAGE_WIDTH, EGIS057E_IMAGE_HEIGHT, fs);
  self->verify_probe_count++;
  /* Start the verify total deadline at the FIRST delivered probe (not at
   * detection): a weak press that never yields a usable frame must not arm it. */
  egis057e_verify_arm (self);
  fp_info ("Verify probe %u/%u captured (%d features)",
           self->verify_probe_count, egis057e_verify_probe_target (), fs->n);

  {
    int best = 0;
    gint reject = egis057e_verify_reject_score ();
    gboolean matched = probe_matches_gallery (fs, self->verify_gallery,
                                              self->verify_gallery_n, &best);

    /* Early MATCH: report the verdict and wait for the lift before completing. */
    if (matched)
      {
        fp_info ("Verify probe %u already matches (best %d); verdict MATCH",
                 self->verify_probe_count, best);
        egis057e_begin_lift_wait (self, dev);
        return FALSE;
      }

    /* Early NO-MATCH: a probe clearly below the reject bound will not match on
     * another press either. Scores in [reject, threshold) are a plausible
     * near-miss and keep collecting. */
    if (best < reject)
      {
        fp_info ("Verify probe %u best %d < reject %d; verdict NO MATCH",
                 self->verify_probe_count, best, reject);
        egis057e_begin_lift_wait (self, dev);
        return FALSE;
      }

    /* Gray zone [reject, threshold): this press was read but did not match.
     * The core allows exactly ONE verify report per action, so a hint cannot
     * be sent mid-action while still collecting: end the action with a retry
     * instead — fprintd shows the hint and auto-restarts, gated on a clear
     * frame by arm_needs_clear. */
    fp_info ("Verify probe %u gray zone (best %d in [%d, threshold)); retry exit",
             self->verify_probe_count, best, reject);
    egis057e_verify_retry_exit (self, dev, FP_DEVICE_RETRY_REMOVE_FINGER,
      "Finger not recognized; remove it and try again");
    return TRUE;
  }

  if (self->verify_probe_count >= egis057e_verify_probe_target ())
    {
      fp_info ("Verify probe budget reached (%u); verdict NO MATCH",
               self->verify_probe_count);
      egis057e_begin_lift_wait (self, dev);
      return FALSE;
    }

  return FALSE;
}

/* Route a settled frame to the active action. Returns TRUE when the action has
 * been completed (the SSM must then stop). */
static gboolean
egis057e_deliver_frame (FpDeviceEgis057e *self, FpDevice *dev,
                        const guint8 *frame)
{
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      return egis057e_handle_enroll (self, dev, frame);

    case FPI_DEVICE_ACTION_VERIFY:
      return egis057e_handle_verify (self, dev, frame);

    case FPI_DEVICE_ACTION_CAPTURE:
      {
        g_autoptr(FpImage) img = fp_image_new (EGIS057E_IMAGE_WIDTH,
                                               EGIS057E_IMAGE_HEIGHT);

        memcpy (img->data, frame, EGIS057E_IMAGE_LEN);
        self->action_reported = TRUE;
        fpi_device_capture_complete (dev, g_steal_pointer (&img), NULL);
        return TRUE;
      }

    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      self->action_reported = TRUE;
      return TRUE;
    }
}

/* -------------------------------------------------------------------------
 * Frame capture callback + touch / release temporal state machine
 * ------------------------------------------------------------------------- */

static void
image_recv_cb (FpiUsbTransfer *transfer, FpDevice *dev,
               gpointer user_data, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("image read returned %zd/%d bytes", transfer->actual_length,
          EGIS057E_IMAGE_LEN);

  if (transfer->actual_length < EGIS057E_IMAGE_LEN)
    {
      fp_warn ("short image payload: received %zd/%d bytes",
               transfer->actual_length, EGIS057E_IMAGE_LEN);
      egis057e_report_present (dev, FALSE);
      self->capture_protocol_error = TRUE;
      fpi_ssm_jump_to_state (transfer->ssm, SM_CAPTURE_FINISH);
      return;
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY &&
      self->verify_deadline != 0 &&
      g_get_monotonic_time () > self->verify_deadline)
    {
      fp_dbg ("verify deadline reached (%u probes); completing",
              self->verify_probe_count);
      egis057e_complete_verify (self, dev);
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  /* Per-action watchdog: if frames keep flowing but no user interaction /
   * delivered frame makes progress within the window, fail cleanly instead of
   * hanging. The caller then exits and the device is closed/released. No
   * in-driver USBDEVFS_RESET (it worsens convergence); just fail + reset state
   * so a later action can retry. */
  if (egis057e_watchdog_expired (self))
    {
      fp_warn ("action watchdog expired (no progress within %d ms); failing cleanly",
               egis057e_action_timeout_ms ());
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "EH57E: no finger/progress within the action timeout; the sensor state was reset — retry, or run scripts/reset-sensor.sh if it stays wedged"));
      return;
    }

  if (!self->have_previous_frame)
    {
      memcpy (self->previous_frame, transfer->buffer, EGIS057E_IMAGE_LEN);
      self->have_previous_frame = TRUE;
      fp_dbg ("stored initial retained frame; waiting for sensor update");
    }
  else
    {
      double mean_difference;
      Egis057eImageQuality quality;
      Egis057ePresence presence;
      gboolean present;
      gboolean absent;
      gboolean abs_gate;
      gint64 now;

      frame_mean_difference (transfer->buffer, self->previous_frame,
                             &mean_difference);
      memcpy (self->previous_frame, transfer->buffer, EGIS057E_IMAGE_LEN);

      quality = image_quality (transfer->buffer);
      abs_gate = quality.standard_deviation >= EGIS057E_FALLBACK_SD &&
                 quality.gradient_rms >= EGIS057E_FALLBACK_GRAD;
      presence = egis057e_presence_eval (self, transfer->buffer);
      {
        if (self->bg_valid)
          {
            /* Coverage present, but cross-checked against the absolute gate so
             * a contaminated/mismatched baseline can never produce phantom
             * presence (an empty/held-out case fails abs_gate -> ABSENT). */
            present = abs_gate && egis057e_frame_present (&presence);
            absent = !abs_gate || egis057e_frame_absent (&presence);
          }
        else
          {
            /* Never be blind before the baseline is seeded. */
            present = abs_gate;
            absent = !abs_gate;
          }
      }
      now = g_get_monotonic_time ();

      fp_dbg ("presence: mad %.1f cov %.1f%% T %.0f present=%d (stddev %.1f grad %.1f range %u)",
              presence.mad, presence.coverage,
              egis057e_presence_threshold (self), present,
              quality.standard_deviation, quality.gradient_rms,
              quality.intensity_range);

      /* Liveness: a live sensor never repeats a frame byte-exactly. */
      if (mean_difference == 0.0)
        self->identical_frames++;
      else
        self->identical_frames = 0;

      if (self->identical_frames >= EGIS057E_LATCH_FRAMES)
        {
          self->identical_frames = 0;
          if (self->latch_recoveries < 1)
            {
              fp_warn ("EGIS057E_SENSOR_LATCHED: %u byte-identical frames; re-running init chain",
                       EGIS057E_LATCH_FRAMES);
              self->latch_recoveries++;
              self->init_step = 0;
              self->have_previous_frame = FALSE;
              self->detection_armed = FALSE;
              self->arm_needs_clear = TRUE;
              self->arm_clear_frames = 0;
              self->touch_pending = FALSE;
              self->touch_settle_frames = 0;
              self->have_touch_best_frame = FALSE;
              self->awaiting_release = FALSE;
              self->awaiting_release_since = 0;
              self->present_since = 0;
              self->bg_valid = FALSE;
              self->bg_warmup = 0;
              if (self->bg_q8)
                memset (self->bg_q8, 0, EGIS057E_IMAGE_LEN * sizeof (guint32));
              fpi_ssm_jump_to_state (transfer->ssm, SM_INIT_SEND);
              return;
            }
          fp_warn ("EGIS057E_SENSOR_LATCHED: sensor still stuck after re-init; failing action");
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                 "EGIS057E_SENSOR_LATCHED: sensor keeps returning a stale frame; "
                                 "power-cycle the reader or run scripts/reset-sensor.sh"));
          return;
        }

      /* Seed the per-pixel background from the first live frames of the action
       * (the sensor is idle at action start) BEFORE the arm gate can block, so
       * presence can be decided by coverage instead of the absolute gate, which
       * is trivially satisfied by this sensor's idle frames. No-ops once valid. */
      if (!self->bg_valid)
        {
          for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
            self->bg_q8[i] += (guint32) transfer->buffer[i] << 8;
          self->bg_warmup++;
          if (self->bg_warmup >= EGIS057E_BG_WARMUP_FRAMES)
            {
              for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
                self->bg_q8[i] /= EGIS057E_BG_WARMUP_FRAMES;
              self->bg_valid = TRUE;
              self->bg_warmup = 0;
              fp_info ("EGIS057E_BG_SEED (early)");
            }
        }

      /* Global presence bookkeeping (drives the stuck-presence watchdog). */
      if (present)
        {
          self->absent_streak = 0;
          if (self->present_since == 0)
            self->present_since = now;
        }
      else if (absent)
        {
          self->absent_streak++;
          if (self->absent_streak >= EGIS057E_RELEASE_FRAMES)
            self->present_since = 0;
        }
      /* hysteresis band (6%..12%): hold present_since / absent_streak */

      /* Stuck-presence watchdog: WAITING + SETTLE only; NEVER in lift-wait. */
      if (self->detection_armed && !self->verify_waiting_lift &&
          !self->awaiting_release && self->present_since != 0 &&
          now - self->present_since >
            (gint64) EGIS057E_STUCK_PRESENCE_MS * 1000)
        {
          fp_warn ("EGIS057E_STUCK_PRESENCE: presence for %d ms without a usable frame; completing action cleanly",
                   EGIS057E_STUCK_PRESENCE_MS);
          /* Invalidate the baseline so it reseeds from idle frames next time. */
          self->bg_valid = FALSE;
          self->bg_warmup = 0;
          if (self->bg_q8)
            memset (self->bg_q8, 0, EGIS057E_IMAGE_LEN * sizeof (guint32));
          if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
            {
              egis057e_complete_verify (self, dev);
              fpi_ssm_mark_completed (transfer->ssm);
              return;
            }
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "EH57E: stuck presence without a usable frame"));
          return;
        }

      /* State: post-verdict lift-wait. Completes only after ABSENT frames (or
       * the bounded timeout); reports NONE only on real absence. */
      if (self->verify_waiting_lift)
        {
          if (present)
            {
              self->verify_lift_frames = 0;
              egis057e_report_present (dev, TRUE);
            }
          else if (absent)
            {
              self->verify_lift_frames++;
            }

          if (self->verify_lift_frames >= EGIS057E_VERIFY_LIFT_FRAMES)
            {
              fp_dbg ("finger lifted after verdict; completing verify");
              fp_info ("EGIS057E_VERIFY_LIFTED");
              egis057e_report_present (dev, FALSE);
              egis057e_complete_verify (self, dev);
              fpi_ssm_mark_completed (transfer->ssm);
              return;
            }

          if (now > self->verify_lift_deadline)
            {
              fp_dbg ("finger still down after %d ms; completing verify anyway",
                      egis057e_verify_lift_timeout_ms ());
              fp_info ("EGIS057E_VERIFY_LIFT_TIMEOUT");
              /* The finger was never confirmed lifted: guard the next action. */
              self->arm_needs_clear = TRUE;
              egis057e_complete_verify (self, dev);
              fpi_ssm_mark_completed (transfer->ssm);
              return;
            }
          goto image_done;
        }

      /* State: arming. Immediate once arm_needs_clear has seen a clear sensor. */
      if (!self->detection_armed)
        {
          if (self->arm_needs_clear)
            {
              if (present)
                {
                  self->arm_clear_frames = 0;
                  fp_dbg ("arm gate: presence (mad %.1f cov %.1f%%); waiting for a clear sensor",
                          presence.mad, presence.coverage);
                  goto image_done;
                }

              self->arm_clear_frames++;
              fp_dbg ("arm gate: non-present frames %u/%u (mad %.1f cov %.1f%%)",
                      self->arm_clear_frames, EGIS057E_RELEASE_FRAMES,
                      presence.mad, presence.coverage);
              if (self->arm_clear_frames < EGIS057E_RELEASE_FRAMES)
                goto image_done;

              self->arm_needs_clear = FALSE;
              fp_dbg ("arm gate cleared after %u non-present frames",
                      self->arm_clear_frames);
            }

          self->detection_armed = TRUE;
          fp_dbg ("presence detection armed (per-pixel background baseline)");
          goto image_done;
        }

      /* State: release-wait. Wait for the captured contact to leave. */
      if (self->awaiting_release)
        {
          if (present)
            self->release_frames = 0;
          else if (absent)
            self->release_frames++;
          else
            goto image_done;   /* hysteresis band: hold */

          fp_dbg ("release wait: ABSENT frames %u/%u",
                  self->release_frames, EGIS057E_RELEASE_FRAMES);
          if (self->release_frames >= EGIS057E_RELEASE_FRAMES)
            {
              fp_dbg ("finger released");
              self->awaiting_release = FALSE;
              self->release_frames = 0;
              egis057e_report_present (dev, FALSE);
              if (!self->action_reported)
                egis057e_report_needed (dev);
            }

          /* Defense in depth: a live permanently-present condition (residue
           * ghost) cannot be caught by byte-identity; bound the wait instead.
           * A normal lift (1-3 s) never trips this. */
          if (self->awaiting_release)
            {
              if (self->awaiting_release_since == 0)
                self->awaiting_release_since = now;
              if (now - self->awaiting_release_since >
                  (gint64) EGIS057E_RELEASE_STUCK_MS * 1000)
                {
                  fp_warn ("EGIS057E_RELEASE_STUCK: presence for %d ms after a stage; "
                           "re-arming and reseeding", EGIS057E_RELEASE_STUCK_MS);
                  self->awaiting_release = FALSE;
                  self->release_frames = 0;
                  self->awaiting_release_since = 0;
                  self->present_since = 0;
                  self->arm_needs_clear = TRUE;
                  self->arm_clear_frames = 0;
                  self->bg_valid = FALSE;
                  self->bg_warmup = 0;
                  if (self->bg_q8)
                    memset (self->bg_q8, 0, EGIS057E_IMAGE_LEN * sizeof (guint32));
                  egis057e_report_present (dev, FALSE);
                  if (!self->action_reported)
                    egis057e_report_needed (dev);
                }
            }
          goto image_done;
        }

      /* State: settle. Collect frames; best USABLE by quality.score minus the
       * temporal motion penalty. Deliver iff present AND a best exists; while
       * present without a best, keep collecting (stuck watchdog bounds a weak
       * press). Abort to WAITING after EGIS057E_RELEASE_FRAMES ABSENT frames. */
      if (self->touch_pending)
        {
          gboolean usable = image_quality_acceptable (&quality);
          guint window = (guint) egis057e_settle_window ();

          if (absent)
            self->no_contact_frames++;
          else if (present)
            self->no_contact_frames = 0;

          if (self->no_contact_frames >= EGIS057E_RELEASE_FRAMES)
            {
              fp_dbg ("presence lost during settle; aborting to waiting");
              self->touch_pending = FALSE;
              self->touch_settle_frames = 0;
              self->finger_frames = 0;
              self->no_contact_frames = 0;
              self->have_touch_best_frame = FALSE;
              egis057e_report_present (dev, FALSE);
              /* VERIFY: a press that left before producing a usable frame ends
               * the action with a visible retry hint (fprintd re-arms). */
              if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
                {
                  fp_info ("EGIS057E_VERIFY_RETRY exit (lifted before a usable frame)");
                  egis057e_verify_retry_exit (self, dev, FP_DEVICE_RETRY_GENERAL,
                    "Finger lifted too early; place it back on the reader and hold");
                  fpi_ssm_mark_completed (transfer->ssm);
                  return;
                }
              /* One-shot feedback for a press that left before producing a
               * usable frame. Safe by construction: this branch only runs while
               * the stage is still pending (nothing was delivered). The
               * retry_reported check dedups against the settle-window hint. */
              if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL &&
                  !self->retry_reported)
                {
                  fp_info ("EGIS057E_SETTLE_ABORT hint (lifted before a usable frame, code %s)",
                           self->hint_alt ? "general" : "centered");
                  fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                    fpi_device_retry_new_msg (egis057e_next_hint_code (self),
                      "Finger lifted too early; keep it on the sensor until the stage completes"));
                }
              self->retry_reported = FALSE;
              goto image_done;
            }

          self->touch_settle_frames++;

          if (usable)
            {
              double pick_score = quality.score - mean_difference;

              if (!self->have_touch_best_frame ||
                  pick_score > self->touch_best_quality)
                {
                  self->touch_best_quality = pick_score;
                  self->touch_best_q = quality;
                  memcpy (self->touch_best_frame, transfer->buffer,
                          EGIS057E_IMAGE_LEN);
                  self->have_touch_best_frame = TRUE;
                }
            }

          fp_dbg ("settle (present=%d, usable=%d): activity %.2f, best %.2f, elapsed %u/%u",
                  present, usable, mean_difference, self->touch_best_quality,
                  MIN (self->touch_settle_frames, window), window);
          fp_info ("EGIS057E_SETTLE %u %u",
                   MIN (self->touch_settle_frames, window), window);

          if (self->touch_settle_frames >= EGIS057E_SETTLING_FRAMES)
            {
              if (!(present && self->have_touch_best_frame))
                {
                  /* Presence persists but no USABLE frame yet: keep collecting.
                   * When the window is exhausted, give ENROLL one-shot on-screen
                   * feedback; the retry is now emitted during the settle while
                   * finger-present is reported (from SETTLE_START). */
                  /* VERIFY: weak press held with no usable frame — end the
                   * action with a visible "not centered" hint (fprintd re-arms)
                   * instead of collecting silently until the deadline. */
                  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY &&
                      self->touch_settle_frames >= window)
                    {
                      fp_info ("EGIS057E_VERIFY_RETRY exit (weak press, no usable frame)");
                      egis057e_verify_retry_exit (self, dev, FP_DEVICE_RETRY_CENTER_FINGER,
                        "Weak or off-center press; reposition the finger and try again");
                      fpi_ssm_mark_completed (transfer->ssm);
                      return;
                    }
                  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL &&
                      self->touch_settle_frames >= window &&
                      !self->retry_reported)
                    {
                      self->retry_reported = TRUE;
                      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                        fpi_device_retry_new_msg (egis057e_next_hint_code (self),
                          "Weak or off-center fingerprint; reposition your finger"));
                    }
                  /* The stuck-presence watchdog bounds a weak press. */
                  goto image_done;
                }

              fp_info ("EGIS057E_SETTLE_DONE");
              fp_dbg ("contact settled; delivering best frame (pick %.2f)",
                      self->touch_best_quality);
              self->touch_pending = FALSE;
              self->finger_frames = 0;
              self->touch_settle_frames = 0;
              self->no_contact_frames = 0;
              self->reported_image = TRUE;
              self->present_since = 0;
              self->retry_reported = FALSE;
              egis057e_report_present (dev, TRUE);

              if (egis057e_deliver_frame (self, dev, self->touch_best_frame))
                {
                  self->have_touch_best_frame = FALSE;
                  fpi_ssm_mark_completed (transfer->ssm);
                  return;
                }

              self->have_touch_best_frame = FALSE;
              self->awaiting_release = TRUE;
              self->awaiting_release_since = now;
              egis057e_watchdog_progress (self);
              goto image_done;
            }
          goto image_done;
        }

      /* State: waiting. Non-blocking background seed, then presence trigger. */
      {
        if (!self->bg_valid)
          {
            /* Seed from any frame that is not a LIVE structured press (classic
             * idle frames, or latched/stale frames — both are valid change-
             * detection references). A live structured frame is excluded so a
             * finger resting on the sensor at action start cannot poison the
             * baseline; the arm gate holds until it lifts, then idle frames seed
             * normally. */
            if (!(abs_gate && mean_difference > 0.0))
              {
                for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
                  self->bg_q8[i] += (guint32) transfer->buffer[i] << 8;
                self->bg_warmup++;
                if (self->bg_warmup >= EGIS057E_BG_WARMUP_FRAMES)
                  {
                    for (guint i = 0; i < EGIS057E_IMAGE_LEN; i++)
                      self->bg_q8[i] /= EGIS057E_BG_WARMUP_FRAMES;
                    self->bg_valid = TRUE;
                    self->bg_warmup = 0;
                    fp_info ("EGIS057E_BG_SEED (idle or stale reference)");
                  }
              }
            /* NOTE: do NOT goto image_done here — fall through so the presence
             * trigger can still fire while the baseline warms up. */
          }

        /* Absent frames refine the background and the idle residual floor. */
        if (absent)
          {
            double prev_mad = self->noise_mad;

            egis057e_bg_update (self, transfer->buffer, presence.mad);
            if (prev_mad < 9.0 && self->noise_mad >= 9.0)
              fp_warn ("sensor idle floor drifted (residual mad %.1f); presence gates are baseline-relative",
                       self->noise_mad);
          }

        if (absent)
          self->idle_absent_streak++;
        else if (present)
          self->idle_absent_streak = 0;

        self->next_poll_ms = present ? EGIS057E_IDLE_POLL_FAST_MS :
                             (self->idle_absent_streak < 8 ?
                                EGIS057E_IDLE_POLL_FAST_MS :
                              (self->idle_absent_streak < 80 ?
                                 EGIS057E_IDLE_POLL_MID_MS :
                                 EGIS057E_IDLE_POLL_SLOW_MS));

        if (present)
          self->finger_frames++;
        else
          self->finger_frames = 0;

        /* Weak-touch hint (ENROLL only): a touch the baseline detects (cov/mad
         * above the idle floor) but below the settle trigger never starts a
         * settle and would otherwise be completely silent. One hint per touch;
         * the latch recharges after EGIS057E_RELEASE_FRAMES clearly-no-touch
         * frames. WAITING runs strictly between touches, so this can never fire
         * after an accepted stage. */
        if (self->bg_valid &&
            fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL)
          {
            gboolean weak_touch = !present && !absent &&
                                  presence.coverage >= EGIS057E_TOUCH_COVERAGE &&
                                  presence.mad >= EGIS057E_TOUCH_MAD;

            if (present)
              {
                self->weak_touch_frames = 0;
                self->weak_gap_frames = 0;
              }
            else if (weak_touch)
              {
                self->weak_touch_frames++;
                self->weak_gap_frames = 0;
                if (self->weak_touch_frames >= EGIS057E_TOUCH_FRAMES &&
                    now >= self->weak_hint_deadline)
                  {
                    self->weak_hint_deadline = now +
                      (gint64) EGIS057E_TOUCH_HINT_COOLDOWN_MS * 1000;
                    fp_info ("EGIS057E_WEAK_TOUCH hint (cov %.1f%% mad %.1f, code %s)",
                             presence.coverage, presence.mad,
                             self->hint_alt ? "general" : "centered");
                    fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                      fpi_device_retry_new_msg (egis057e_next_hint_code (self),
                        "Touch too light: press flatter so more of the finger touches the sensor"));
                  }
              }
            else
              {
                self->weak_touch_frames = 0;
                self->weak_gap_frames++;
                if (self->weak_gap_frames >= EGIS057E_RELEASE_FRAMES)
                  {
                    /* Touch ended: re-arm so the next touch hints immediately. */
                    self->weak_gap_frames = 0;
                    self->weak_hint_deadline = 0;
                  }
              }
          }

        fp_dbg ("waiting (present=%d absent=%d): consecutive present %u/%u, idle_absent %u, poll %u ms",
                present, absent, self->finger_frames, EGIS057E_PRESENCE_FRAMES,
                self->idle_absent_streak, self->next_poll_ms);

        if (self->bg_valid &&
            self->finger_frames >= EGIS057E_PRESENCE_FRAMES)
          {
            self->finger_frames = 0;
            self->touch_settle_frames = 0;
            self->no_contact_frames = 0;
            self->have_touch_best_frame = FALSE;
            self->retry_reported = FALSE;
            self->weak_touch_frames = 0;
            self->weak_gap_frames = 0;
            self->weak_hint_deadline = 0;
            self->touch_pending = TRUE;
            /* The finger is now on the sensor: report PRESENT when the turn opens
             * (matches proven drivers; the weak-press retry is then emitted while
             * finger-present is true). */
            egis057e_report_present (dev, TRUE);
            egis057e_watchdog_progress (self);
            fp_info ("EGIS057E_SETTLE_START");
            fp_dbg ("press detected (%u consecutive PRESENT frames); starting settle",
                    EGIS057E_PRESENCE_FRAMES);
          }
      }
      goto image_done;
    }

image_done:
  fpi_ssm_next_state (transfer->ssm);
}

static void
recv_image (FpiSsm *ssm, FpDevice *dev)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EP_IN, EGIS057E_IMAGE_LEN);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_CMD,
                           fpi_device_get_cancellable (dev),
                           image_recv_cb, NULL);
}

static void
capture_delay_done (FpDevice *dev, gpointer user_data)
{
  FpiSsm *ssm = user_data;
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (self->stop || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Capture cancelled"));
      return;
    }

  /* Watchdog backstop even when the device stops returning frames entirely. */
  if (egis057e_watchdog_expired (self))
    {
      fp_warn ("action watchdog expired in poll (no progress within %d ms); failing cleanly",
               egis057e_action_timeout_ms ());
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "EH57E: no finger/progress within the action timeout; the sensor state was reset — retry, or run scripts/reset-sensor.sh if it stays wedged"));
      return;
    }

  fp_dbg ("polling retained image for automatic finger detection");
  fpi_ssm_jump_to_state (ssm, SM_CAPTURE_SEND);
}

/* -------------------------------------------------------------------------
 * Main state machine (init chain + capture loop)
 * ------------------------------------------------------------------------- */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* --- Init phase ---------------------------------------------------- */

    case SM_INIT_SEND:
      if (self->init_step >= EGIS057E_INIT_PKT_COUNT)
        {
          self->calibration_valid = TRUE;
          fp_dbg ("image-path init complete; starting quality-based presence detection");
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
          break;
        }
      if (egis057e_init_pkts[self->init_step].bytes[4] == 0x63 &&
          egis057e_init_pkts[self->init_step].bytes[5] == 0x33 &&
          self->have_calibration_sample)
        {
          Egis057ePacket apply = egis057e_init_pkts[self->init_step];

          apply.bytes[7] = self->calibration_sample;
          send_packet (ssm, dev, &apply);
        }
      else
        send_packet (ssm, dev, &egis057e_init_pkts[self->init_step]);
      break;

    case SM_INIT_RECV:
      if (egis057e_init_pkts[self->init_step].bytes[4] == 0x72)
        {
          recv_calibration_image (ssm, dev);
          break;
        }
      recv_command_response (ssm, dev,
                             egis057e_init_pkts[self->init_step].bytes[4] == 0x63 ||
                             egis057e_init_pkts[self->init_step].bytes[4] == 0x71
                             ? egis057e_init_pkts[self->init_step].len
                             : egis057e_init_pkts[self->init_step].bytes[4] == 0x62
                               ? EGIS057E_RESP_LEN + egis057e_init_pkts[self->init_step].bytes[6]
                               : EGIS057E_RESP_LEN);
      break;

    case SM_INIT_NEXT:
      self->init_step++;
      fpi_ssm_jump_to_state (ssm, SM_INIT_SEND);
      break;

    /* --- Detect / capture loop ----------------------------------------- */

    case SM_CAPTURE_DELAY:
      fpi_device_add_timeout (dev, self->next_poll_ms,
                              capture_delay_done, ssm, NULL);
      break;

    case SM_DETECT_WAIT:
      if (self->stop)
        {
          fp_dbg ("Stop requested; ending capture loop");
          fpi_ssm_mark_completed (ssm);
          break;
        }
      {
        /*
         * Wait for a finger-present interrupt on EP 0x83/0x84. The interrupt
         * payload length is vendor-defined; 16 bytes is a safe upper bound.
         */
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

        fp_dbg ("Waiting for finger interrupt on endpoint 0x%02x",
                self->int_ep);
        fpi_usb_transfer_fill_interrupt (t, self->int_ep, 16);
        t->ssm            = ssm;
        t->short_is_error = FALSE;
        fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT_INT,
                                 fpi_device_get_cancellable (dev),
                                 int_finger_cb, NULL);
      }
      break;

    case SM_CAPTURE_SEND:
      if (self->capture_step >= EGIS057E_CAPTURE_PKT_COUNT)
        {
          self->capture_step = 0;
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_REQUEST);
          break;
        }
      send_packet (ssm, dev, &egis057e_capture_pkts[self->capture_step]);
      break;

    case SM_CAPTURE_RECV:
      if (!egis057e_capture_pkts[self->capture_step].expect_response)
        {
          fpi_ssm_jump_to_state (ssm, SM_CAPTURE_NEXT);
          break;
        }
      recv_command_response (ssm, dev,
                             egis057e_capture_pkts[self->capture_step].bytes[4] == 0x63
                             ? egis057e_capture_pkts[self->capture_step].len
                             : EGIS057E_RESP_LEN);
      break;

    case SM_CAPTURE_NEXT:
      self->capture_step++;
      fpi_ssm_jump_to_state (ssm, SM_CAPTURE_SEND);
      break;

    case SM_CAPTURE_IMAGE:
      recv_image (ssm, dev);
      break;

    case SM_CAPTURE_REQUEST:
      {
        static const Egis057ePacket request = {
          { 'E', 'G', 'I', 'S', 0x64, 0x0f, 0x96 },
          7, "request live image", FALSE
        };
        send_packet (ssm, dev, &request);
      }
      break;

    case SM_CAPTURE_FINISH:
      {
        static const Egis057ePacket finish = {
          { 'E', 'G', 'I', 'S', 0x61, 0x2d, 0x20 },
          7, "finish live image", TRUE
        };
        send_packet (ssm, dev, &finish);
      }
      break;

    case SM_CAPTURE_FINISH_RECV:
      recv_command_response (ssm, dev, EGIS057E_RESP_LEN);
      break;

    case SM_DETECT_DONE:
      self->capture_step = 0;
      if (self->capture_protocol_error)
        {
          self->capture_protocol_error = FALSE;
          fpi_ssm_mark_failed (
            ssm,
            fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                      "Short live image; capture transaction closed"));
          break;
        }
      self->reported_image = FALSE;
      fpi_ssm_jump_to_state (ssm, SM_CAPTURE_DELAY);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* -------------------------------------------------------------------------
 * SSM completion callback
 * ------------------------------------------------------------------------- */

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  self->running = FALSE;
  self->ssm = NULL;

  if (self->action_reported)
    {
      /* The action already completed successfully from the frame callback. */
      if (error)
        {
          self->calibration_valid = FALSE;
          g_error_free (error);
        }
      return;
    }

  /* A verify verdict was already reported but the action did not complete:
   * complete it cleanly rather than failing (avoids a spurious error after a
   * genuine MATCH/NO-MATCH). */
  if (self->verify_verdict_done &&
      fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    {
      if (error)
        {
          self->calibration_valid = FALSE;
          g_error_free (error);
        }
      self->verify_waiting_lift = FALSE;
      self->verify_lift_deadline = 0;
      self->action_reported = TRUE;
      fpi_device_verify_complete (dev, NULL);
      egis057e_report_present (dev, FALSE);
      return;
    }

  self->action_reported = TRUE;
  self->calibration_valid = FALSE;

  if (!error)
    error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                      "Action stopped before an image was acquired");

  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      fpi_device_enroll_complete (dev, NULL, error);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
      fpi_device_verify_complete (dev, error);
      egis057e_report_present (dev, FALSE);
      break;

    case FPI_DEVICE_ACTION_CAPTURE:
      fpi_device_capture_complete (dev, NULL, error);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      g_error_free (error);
      break;
    }
}

/* -------------------------------------------------------------------------
 * Device lifecycle (pure FpDevice)
 * ------------------------------------------------------------------------- */

/* Free an array of feature sets, releasing the channel images each owns. */
static void
egis057e_clear_feature_sets (Egis057eMFeatureSet *sets, guint n)
{
  if (!sets)
    return;
  for (guint i = 0; i < n; i++)
    egis057e_m_feature_set_clear (&sets[i]);
  g_free (sets);
}

static void
dev_open (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  GError *error = NULL;

  if (!self->bg_q8)
    self->bg_q8 = g_new0 (guint32, EGIS057E_IMAGE_LEN);
  self->bg_valid = FALSE;
  self->bg_warmup = 0;
  self->noise_mad = EGIS057E_NOISE_MAD_SEED;
  memset (self->bg_q8, 0, EGIS057E_IMAGE_LEN * sizeof (guint32));

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  GError *error = NULL;

  egis057e_clear_feature_sets (self->enroll_feats, EGIS057E_ENROLL_STAGES);
  self->enroll_feats = NULL;
  g_clear_pointer (&self->enroll_raw, g_free);
  egis057e_clear_feature_sets (self->verify_gallery, EGIS057E_ENROLL_STAGES);
  self->verify_gallery = NULL;
  egis057e_clear_feature_sets (self->verify_probes, EGIS057E_VERIFY_PROBE_FRAMES);
  self->verify_probes = NULL;
  g_clear_pointer (&self->bg_q8, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
dev_cancel (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  /* The in-flight transfer uses the action cancellable and will fail out of
   * the state machine; this flag also stops the between-frame poll timer. */
  self->stop = TRUE;
}

static FpiSsm *
egis057e_start_capture (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  self->stop = FALSE;
  self->action_reported = FALSE;
  self->init_step = 0;
  self->capture_step = 0;
  self->int_ep = EGIS057E_EP_INT_FINGER;
  self->have_previous_frame = FALSE;
  self->calibration_sample = 0x6d;
  self->have_calibration_sample = FALSE;
  self->have_calibration_frame = FALSE;
  self->have_touch_best_frame = FALSE;
  self->detection_armed = FALSE;
  self->finger_frames = 0;
  self->release_frames = 0;
  self->touch_settle_frames = 0;
  self->touch_pending = FALSE;
  self->touch_best_quality = -1.0;
  self->awaiting_release = FALSE;
  self->reported_image = FALSE;
  self->capture_protocol_error = FALSE;
  self->verify_deadline = 0;
  self->verify_armed = FALSE;
  self->verify_verdict_done = FALSE;
  self->verify_waiting_lift = FALSE;
  self->verify_lift_frames = 0;
  self->verify_lift_deadline = 0;
  self->arm_clear_frames = 0;
  self->no_contact_frames = 0;
  self->present_since = 0;
  self->absent_streak = 0;
  self->idle_absent_streak = 0;
  self->next_poll_ms = EGIS057E_IDLE_POLL_FAST_MS;
  self->identical_frames = 0;
  self->latch_recoveries = 0;
  self->awaiting_release_since = 0;
  self->weak_touch_frames = 0;
  self->weak_gap_frames = 0;
  self->weak_hint_deadline = 0;
  /* NOTE: arm_needs_clear is NOT set here. The early baseline seed (below in
   * image_recv_cb) makes the presence trigger coverage-based, so the old
   * clear-to-arm gate would deadlock on a sensor whose idle frames look
   * "present" to the absolute gate. It is still set by the latch/release-stuck
   * recovery paths. */
  /* bg_q8/bg_valid/bg_warmup/noise_mad persist across actions (seeded once in
   * dev_open) so the drift-robust baseline survives; arm_needs_clear is now set
   * per action and by the latch/release-stuck recovery paths. */
  egis057e_watchdog_progress (self);

  self->ssm = fpi_ssm_new (dev, ssm_run_state, SM_STATES_NUM);
  self->running = TRUE;
  fpi_ssm_start (self->ssm, loop_complete);

  return self->ssm;
}

static void
dev_capture (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  gboolean wait_for_finger;

  if (self->running)
    {
      fpi_device_capture_complete (dev, NULL,
                                   fpi_device_error_new (FP_DEVICE_ERROR_BUSY));
      return;
    }

  fpi_device_get_capture_data (dev, &wait_for_finger);
  fp_dbg ("capture requested (wait_for_finger=%d)", wait_for_finger);

  /* The EH57E has no usable hardware finger bit in image mode, so the capture
   * loop always performs software touch detection. */
  egis057e_report_needed (dev);
  egis057e_start_capture (dev);
}

static void
dev_enroll (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  fpi_device_get_enroll_data (dev, &self->enroll_print);

  egis057e_clear_feature_sets (self->enroll_feats, EGIS057E_ENROLL_STAGES);
  self->enroll_feats = NULL;
  g_clear_pointer (&self->enroll_raw, g_free);
  self->enroll_feats = g_new0 (Egis057eMFeatureSet, EGIS057E_ENROLL_STAGES);
  self->enroll_raw = g_new0 (guint8,
                             (gsize) EGIS057E_ENROLL_STAGES * EGIS057E_IMAGE_LEN);
  self->enroll_stage = 0;
  self->enroll_rejects = 0;

  fp_dbg ("Enroll requested (%d stages)", EGIS057E_ENROLL_STAGES);
  egis057e_report_needed (dev);
  egis057e_start_capture (dev);
}

static void
dev_verify (FpDevice *dev)
{
  FpDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  FpPrint *verify_print = NULL;
  g_autoptr(GVariant) stored = NULL;

  fpi_device_get_verify_data (dev, &verify_print);
  g_object_get (verify_print, "fpi-data", &stored, NULL);

  egis057e_clear_feature_sets (self->verify_gallery, EGIS057E_ENROLL_STAGES);
  self->verify_gallery = NULL;
  egis057e_clear_feature_sets (self->verify_probes, EGIS057E_VERIFY_PROBE_FRAMES);
  self->verify_probes = NULL;
  self->verify_gallery = g_new0 (Egis057eMFeatureSet, EGIS057E_ENROLL_STAGES);
  self->verify_gallery_n = 0;
  self->verify_probes = g_new0 (Egis057eMFeatureSet, EGIS057E_VERIFY_PROBE_FRAMES);
  self->verify_probe_count = 0;

  /* Fail fast on template presence/version/type mismatch: running a whole
   * capture turn only to report a bad template wastes seconds. */
  if (!stored ||
      !egis057e_unpack_template (stored, self->verify_gallery,
                                 &self->verify_gallery_n, &self->verify_cal_sample))
    {
      fp_warn ("Verify print has no valid EH57E template; re-enroll");
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "enrolled print has no usable EH57E template (outdated or invalid format); delete it and re-enroll"));
      egis057e_report_present (dev, FALSE);
      return;
    }

  if (self->verify_gallery_n < EGIS057E_M_AGREE_FRAMES)
    {
      fp_warn ("Verify print has only %u gallery frame(s); minimum is %d; re-enroll",
               self->verify_gallery_n, EGIS057E_M_AGREE_FRAMES);
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "enrolled print has too few EH57E frames to ever match; delete it and re-enroll"));
      egis057e_report_present (dev, FALSE);
      return;
    }

  fp_dbg ("Verify requested (multi-frame probes), gallery=%u frames",
          self->verify_gallery_n);
  egis057e_report_needed (dev);
  egis057e_start_capture (dev);
  /* No-press bound; replaced by the total deadline at the first press. */
  self->verify_deadline = g_get_monotonic_time () +
                          EGIS057E_VERIFY_START_TIMEOUT_MS * 1000;
}

/* -------------------------------------------------------------------------
 * Driver registration
 * ------------------------------------------------------------------------- */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x057e, .driver_data = 0 },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

static void
fpi_device_egis057e_init (FpDeviceEgis057e *self)
{
}

static void
fpi_device_egis057e_class_init (FpDeviceEgis057eClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id        = "egis057e";
  dev_class->full_name = "Egis Technology EH57E Touch Fingerprint Sensor";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 5;
  dev_class->temp_hot_seconds = -1;

  dev_class->open    = dev_open;
  dev_class->close   = dev_close;
  dev_class->cancel  = dev_cancel;
  dev_class->enroll  = dev_enroll;
  dev_class->verify  = dev_verify;
  dev_class->capture = dev_capture;

  fpi_device_class_auto_initialize_features (dev_class);
  /* Pure FpDevice; there is no gallery/identify path on this sensor. */
  dev_class->features &= ~FP_DEVICE_FEATURE_IDENTIFY;
}
