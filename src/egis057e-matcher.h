/*
 * EgisTec EH57E matcher — vendor-faithful pipeline.
 *
 * Pure C (no GLib) so it compiles into both the libfprint driver and the
 * offline harness (tools/egis057e-match-test.c) with bit-identical behaviour.
 *
 * The previous EH575-derived port used an "11-orientation FIR bank + per-pixel
 * argmax" orientation, which produced a near-tied orientation margin (~0.958)
 * and non-discriminative descriptors. A Ghidra reverse-engineering of
 * EgisTouchFPEngine057E.dll (analysis/eh57e-frontend-reverse.md) shows the
 * vendor pipeline is:
 *
 *   1. resample x1.75 (cfg[5]=0x700, Q10) -> normalise (cfg[6]) -> enhance (cfg[7])
 *   2. ONE separable ISOTROPIC ridge-scale smoothing kernel selected by cfg[0]
 *      (the 11 "FIR bank" kernels are widths, not orientations)
 *   3. structure tensor Ix,Iy,Ixx,Ixy,Iyy via separable [smooth]x[derivative]
 *   4. response det = Ixx*Iyy - Ixy^2 (polarity = sign(Ixx))
 *   5. interest points = strict 3x3 maxima of det + density NMS
 *   6. main orientation = response-weighted histogram over the 11x11 pyramid
 *      window (0xd7140), angles via the vendor atan2 LUT (0xd7030, 720-x)
 *   7. binary descriptor = pairwise rank comparisons of orientation-rotated
 *      n x n block means, n in {2,3,4}, 3 channels, LSB-packed (~486 bits)
 *   8. matching = orientation-gated Hamming NN, ratio test, budget 115,
 *      iterative (dx,dy,dphi) alignment, score = sum(128 - hamming);
 *      verdict at EGIS057E_M_MATCH_THRESHOLD (calibrated on real data, §3g)
 *
 * See EGIS057E_M_* for the tunable parameters and their provenance.
 */

#pragma once

#include <stdint.h>

#define EGIS057E_M_DESC_BYTES      64   /* 8 x u64 (486 payload bits used) */
#define EGIS057E_M_MAX_FEATURES    512  /* cfg[0x12] = 0x200 */
#define EGIS057E_M_MIN_FEATURES    5
#define EGIS057E_M_MIN_MATCHED     5    /* cfg[0x1e] */
#define EGIS057E_M_HAMMING_BUDGET  115  /* cfg[0x1a] = 0x73 / 512 */
/* Match verdict thresholds, recalibrated 2026-10 on the EH57E captures
 * (/var/home/matt/eh57e-project/captures; template eval, groups
 * first+ctrl1=A / imp1=B, K=6 enroll frames; sampler=double default):
 *   genuine best-score (A) = 914,1449,1487,2275,3573,4341 ; (B) = 1774,3846
 *   impostor best-score max = 1233 (A) / 1341 (B).
 * FAR=0 needs T > 1341.  MATCH_THRESHOLD=1480 is +10.4% over the worst
 * impostor: at 1480, FAR=0 (both folds), TAR = 0.667 (A) / 1.000 (B)
 * (combined 0.75); AGREE_SCORE=MATCH/2 keeps the >=2-frame rule at FAR=0.
 * NOTE: the EH57E score scale is ~2-3x the vendor's (impostor best reaches
 * ~1300, not ~380), so the vendor defaults 660/330 reject nothing here.
 * PRELIMINARY: only 2 fingers / 20 frames; error bars are large (a single
 * genuine frame at 1449/1487 straddles the threshold) — re-derive with more
 * fingers before relying on this. */
#define EGIS057E_M_MATCH_THRESHOLD 1480
#define EGIS057E_M_AGREE_SCORE     740
#define EGIS057E_M_RATIO_Q10       700  /* cfg[0x18] ratio test 0.7 */
#define EGIS057E_M_POS_TOL_PX      10
#define EGIS057E_M_ANGLE_TOL_DEG   12.0

/* Driver verdict helper (egis057e.c): accept when the best gallery frame score
 * clears MATCH_THRESHOLD AND at least AGREE_FRAMES frames clear AGREE_SCORE.
 * Re-derivation on the 2-finger template eval (2026-10) with MATCH=1480 gives
 * FAR=0 (worst impostor best 1341); the original agreement guard
 * (AGREE_FRAMES=2, AGREE_SCORE=740) rejected genuine probes that matched only
 * one template frame strongly (a genuine probe scored 3653 -> NO-MATCH), so the
 * default is relaxed to AGREE_FRAMES=1 (i.e. the rule becomes best>=MATCH).
 * SECURITY CAVEAT: with 1 frame there is no cross-frame agreement guard; the
 * decision rests solely on MATCH_THRESHOLD, which is calibrated on only two
 * fingers. Env-tunable at runtime (EGIS057E_M_MATCH_THRESHOLD /
 * _M_AGREE_SCORE / _M_AGREE_FRAMES); raise AGREE_FRAMES back to 2 if a stronger
 * guard is needed at the cost of FRR. */
#define EGIS057E_M_AGREE_FRAMES    1

typedef struct
{
  uint16_t x, y;
  uint8_t  orient;                     /* dominant orientation bin */
  uint8_t  bucket;                     /* coarse-orientation bucket (diagnostic) */
  uint8_t  polarity;                   /* ridge polarity = sign(Ixx) (feature[+0xc]) */
  uint16_t angle;                      /* refined orientation degrees */
  uint8_t  desc[EGIS057E_M_DESC_BYTES];/* pairwise-rank binary descriptor */
} Egis057eMFeature;

typedef struct
{
  int             n;
  Egis057eMFeature f[EGIS057E_M_MAX_FEATURES];

  /* Descriptor sampling channels (smoothed intensity, Ix, Iy), owned by the
   * set and freed by egis057e_m_feature_set_clear(). Used by
   * egis057e_m_score() to rebuild a descriptor at the transformed pose
   * (vendor verify_matches_geom). w*h doubles each; NULL when unavailable. */
  int     chan_w, chan_h;
  double *chan_sm, *chan_ix, *chan_iy;
} Egis057eMFeatureSet;

/* Extract features from a raw grayscale image (w x h, row-major).
 * The output set must be zero-initialised (GObject calloc / = {0}); any
 * previously held channels are released. */
void egis057e_m_extract (const uint8_t *img, int w, int h,
                         Egis057eMFeatureSet *out);

/* Release the channel images owned by a set (does not free the set itself). */
void egis057e_m_feature_set_clear (Egis057eMFeatureSet *set);

/* Diagnostic variant (offline harness): additionally returns the mean
 * orientation margin, i.e. mean over features of the second-dominant /
 * dominant bin magnitude of the weighted orientation histogram. */
void egis057e_m_extract_ex (const uint8_t *img, int w, int h,
                            Egis057eMFeatureSet *out,
                            double *orient_margin_out);

/* Vendor-style score: orientation-gated Hamming NN with ratio test, Hamming
 * budget, translation vote; score = sum(128 - hamming) over retained matches.
 * Returns n_good (votes) via *votes_out when non-NULL. */
int egis057e_m_score (const Egis057eMFeatureSet *probe,
                      const Egis057eMFeatureSet *gallery,
                      int *votes_out);

/* Number of orientation bins over 180 degrees (for harness diagnostics). */
int egis057e_m_orient_bins (void);
