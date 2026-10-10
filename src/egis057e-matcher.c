/*
 * EgisTec EH57E matcher — vendor-faithful implementation.
 *
 * Implemented from analysis/eh57e-frontend-spec.md (follow-up to
 * eh57e-frontend-reverse.md), which pins the descriptor geometry, the
 * orientation histogram and the exact atan2 LUT. Provenance:
 *   - FIR width kernels @0xd6ed0 (85 i32, lens [5,5,5,7,7,7,9,9,9,11,11])
 *   - descriptor pyramid weights @0xd7140 (121 i32, symmetric, peak 425)
 *   - atan2 LUT @0xd7030 (128 u16 = round(8 * degrees(atan2(i,128))))
 *   - cos/sin Q16 @0xca0a0/@0xd19a0 = round(cos/sin(i deg) * 65536)
 *
 * Pipeline:
 *   resample x1.75 -> normalise (cfg[6]=1) -> enhance (cfg[7]=2)
 *   -> one separable isotropic FIR ridge-scale kernel (cfg[0]=0x10 -> idx 6)
 *   -> structure tensor Ix,Iy,Ixx,Ixy,Iyy (scale cfg[0]*15/100 = 2)
 *   -> det = Ixx*Iyy - Ixy^2 ; interest points = strict 3x3 maxima + NMS
 *   -> orientation: 45-bin (shift 3) histogram of pyramid-weighted Ix/Iy via
 *      the exact atan2 LUT, 9-bin sliding-window argmax, refined via atan2
 *   -> descriptor: 3 channels (smoothed intensity, Ix, Iy), grid centres at
 *      -9.5 px stepping (2*cfg[4]+1)/n for n in {2,3,4}, rotated block means,
 *      pairwise rank comparisons, 486 bits LSB-first
 *   -> matching: orientation-gated Hamming NN, ratio 0.7, budget 115,
 *      translation vote, score = sum(128 - hamming), verdict 660
 *
 * SPDX-License-Identifier: 0BSD
 */

#include "egis057e-matcher.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------
 * Vendor tables
 * ------------------------------------------------------------------------- */

#define N_KERNELS 11
static const int FIR_LEN[N_KERNELS] = { 5, 5, 5, 7, 7, 7, 9, 9, 9, 11, 11 };
static const int FIR_OFF[N_KERNELS] = { 0, 5, 10, 15, 22, 29, 36, 45, 54, 63, 74 };
static const int FIR_FLAT[85] = {
  1785, 8003, 13193, 8002, 1785,
  2319, 8011, 12109, 8010, 2319,
  2806, 7951, 11253, 7952, 2806,
  706, 3097, 7524, 10114, 7524, 3097, 706,
  950, 3402, 7313, 9438, 7313, 3402, 950,
  1200, 3646, 7104, 8870, 7102, 3646, 1200,
  361, 1415, 3757, 6748, 8205, 6749, 3757, 1415, 361,
  486, 1632, 3877, 6517, 7746, 6515, 3877, 1632, 486,
  622, 1832, 3962, 6295, 7346, 6295, 3962, 1832, 622,
  216, 753, 1985, 3967, 6011, 6904, 6011, 3967, 1985, 753, 216,
  289, 889, 2134, 3986, 5800, 6572, 5800, 3986, 2134, 889, 289,
};

static const int PYR[11][11] = {
  {   0,   0,  28,  42,  53,  57,  53,  42,  28,   0,   0 },
  {   0,  33,  57,  86, 109, 118, 109,  86,  57,  33,   0 },
  {  28,  57, 101, 150, 191, 207, 191, 150, 101,  57,  28 },
  {  42,  86, 150, 224, 285, 308, 285, 224, 150,  86,  42 },
  {  53, 109, 191, 285, 362, 392, 362, 285, 191, 109,  53 },
  {  57, 118, 207, 308, 392, 425, 392, 308, 207, 118,  57 },
  {  53, 109, 191, 285, 362, 392, 362, 285, 191, 109,  53 },
  {  42,  86, 150, 224, 285, 308, 285, 224, 150,  86,  42 },
  {  28,  57, 101, 150, 191, 207, 191, 150, 101,  57,  28 },
  {   0,  33,  57,  86, 109, 118, 109,  86,  57,  33,   0 },
  {   0,   0,  28,  42,  53,  57,  53,  42,  28,   0,   0 },
};

/* atan2 LUT @0xd7030 = round(8 * degrees(atan2(i,128))). */
static uint16_t
atan2_lut_value (int i)
{
  return (uint16_t) lround (atan2 ((double) i, 128.0) * 180.0 / M_PI * 8.0);
}

/* -------------------------------------------------------------------------
 * Vendor configuration defaults (spec §5 default row)
 * ------------------------------------------------------------------------- */

#define EGIS057E_CFG_RESAMPLE_Q10   1792  /* cfg[5]=0x700 -> x1.75 */
#define EGIS057E_CFG_FIR_SEL        16    /* cfg[0]=0x10 -> kernel idx 6 */
#define EGIS057E_CFG_ENHANCE        2     /* cfg[7] */
#define EGIS057E_CFG_NORM           1     /* cfg[6] */
#define EGIS057E_CFG_DESC_CFG4      9     /* cfg[4] -> spacing 2 */
#define EGIS057E_CFG_ORIENT_SHIFT   3     /* cfg[0xf] -> shift */
#define EGIS057E_CFG_ORIENT_BINS    45    /* 360 >> shift */
#define EGIS057E_CFG_ORIENT_OFFSET  8     /* (cfg[0xf]/10) >> shift */
#define EGIS057E_CFG_ORIENT_SPACING 2     /* (6 < cfg[4]) + 1 */
#define EGIS057E_CFG_TENSOR_SCALE   2     /* cfg[0]*15/100 */
#define EGIS057E_CFG_NMS_R2         36
#define EGIS057E_CFG_NORM_RADIUS    8
#define EGIS057E_CFG_ENHANCE_RADIUS 8
#define EGIS057E_CFG_ORIENT_GATE    1

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

/* Runtime-tunable knobs (defaults = vendor config); the driver never sets
 * them, so its behaviour is the compile defaults. */
typedef struct
{
  int    resample_q10;
  int    fir_sel;
  int    enhance;
  int    norm_on;
  int    norm_radius;
  int    enh_radius;
  int    tensor_scale;
  int    nms_r2;
  double resp_frac;
  double resp_thresh;
  int    desc_cfg4;
  int    orient_shift;
  int    orient_mode;     /* 0 = signed360, 1 = fold2theta */
  int    fold_bins;       /* bins over 180 deg for fold2theta */
  int    coarse_bins;     /* feature[+0xd] coarse-orientation buckets */
  int    subpix;          /* subpixel Hessian refine on/off */
  int    geom_check;      /* geometric reprojection verification on/off */
  int    multihyp;        /* multi-orientation hypothesis expansion on/off */
  double hyp_thr;         /* cfg[0x38] threshold, per-1024 */
  int    polarity_gate;   /* compare probe/gallery ridge polarity */
  double align_atol;      /* residual angle tolerance after rotation (deg) */
  double align_ptol;      /* residual position tolerance after rotation (px) */
  int    budget;          /* Hamming budget (default 115) */
  int    ratio_q10;       /* ratio test numerator (default 700) */
  int    reextract;       /* geometric re-extraction verification on/off */
  int    reextract_slack; /* allowed hamming increase vs stored match */
  int    sampler;         /* 1 = vendor 16.16 fixed-point, 0 = double bilinear */
} Pcfg;

static Pcfg g_pcfg;
static int  g_pcfg_loaded = 0;

static int
env_i (const char *name, int def)
{
  const char *e = getenv (name);

  return (e && *e) ? atoi (e) : def;
}

static double
env_d (const char *name, double def)
{
  const char *e = getenv (name);

  return (e && *e) ? atof (e) : def;
}

static const Pcfg *
pcfg (void)
{
  if (!g_pcfg_loaded)
    {
      g_pcfg.resample_q10 = env_i ("EGIS057E_P_RESAMPLE", EGIS057E_CFG_RESAMPLE_Q10);
      g_pcfg.fir_sel = env_i ("EGIS057E_P_FIR_SEL", EGIS057E_CFG_FIR_SEL);
      g_pcfg.enhance = env_i ("EGIS057E_P_ENHANCE", EGIS057E_CFG_ENHANCE);
      g_pcfg.norm_on = env_i ("EGIS057E_P_NORM", EGIS057E_CFG_NORM);
      g_pcfg.norm_radius = env_i ("EGIS057E_P_NORM_RADIUS", EGIS057E_CFG_NORM_RADIUS);
      g_pcfg.enh_radius = env_i ("EGIS057E_P_ENH_RADIUS", EGIS057E_CFG_ENHANCE_RADIUS);
      g_pcfg.tensor_scale = env_i ("EGIS057E_P_TENSOR_SCALE", EGIS057E_CFG_TENSOR_SCALE);
      g_pcfg.nms_r2 = env_i ("EGIS057E_P_NMS2", EGIS057E_CFG_NMS_R2);
      g_pcfg.resp_frac = env_d ("EGIS057E_P_RESP_FRAC", 0.0);
      g_pcfg.resp_thresh = env_d ("EGIS057E_P_RESP_THRESH", 0.0);
      g_pcfg.desc_cfg4 = env_i ("EGIS057E_P_DESC_CFG4", EGIS057E_CFG_DESC_CFG4);
      g_pcfg.orient_shift = env_i ("EGIS057E_P_ORIENT_SHIFT", EGIS057E_CFG_ORIENT_SHIFT);
      g_pcfg.orient_mode = env_i ("EGIS057E_P_ORIENT_MODE", 0);
      g_pcfg.fold_bins = env_i ("EGIS057E_P_FOLD_BINS", 18);
      g_pcfg.coarse_bins = env_i ("EGIS057E_P_COARSE_BINS", 8);
      g_pcfg.subpix = env_i ("EGIS057E_P_SUBPIX", 1);
      g_pcfg.geom_check = env_i ("EGIS057E_P_GEOM", 1);
      g_pcfg.multihyp = env_i ("EGIS057E_P_MULTIHYP", 1);
      g_pcfg.hyp_thr = env_d ("EGIS057E_P_HYP_THR", 300.0);
      g_pcfg.polarity_gate = env_i ("EGIS057E_P_POLARITY_GATE", 1);
      g_pcfg.align_atol = env_d ("EGIS057E_P_ALIGN_ATOL", 45.0);
      g_pcfg.align_ptol = env_d ("EGIS057E_P_ALIGN_PTOL", 20.0);
      g_pcfg.budget = env_i ("EGIS057E_P_BUDGET", 115);
      g_pcfg.ratio_q10 = env_i ("EGIS057E_P_RATIO", 850);
      g_pcfg.reextract = env_i ("EGIS057E_P_REEXTRACT", 0);
      g_pcfg.reextract_slack = env_i ("EGIS057E_P_REEXTRACT_SLACK", 40);
      g_pcfg.sampler = env_i ("EGIS057E_P_SAMPLER", 0);
      g_pcfg_loaded = 1;
    }
  return &g_pcfg;
}

/* -------------------------------------------------------------------------
 * Float image helpers
 * ------------------------------------------------------------------------- */

typedef struct
{
  int     w, h;
  double *d;
} Img;

static int
img_alloc (Img *im, int w, int h, int zero)
{
  im->w = w;
  im->h = h;
  im->d = zero ? calloc ((size_t) w * h, sizeof (double))
               : malloc ((size_t) w * h * sizeof (double));
  return im->d != NULL;
}

static void
img_free (Img *im)
{
  free (im->d);
  im->d = NULL;
}

static double
sample_bilinear (const Img *im, double x, double y)
{
  int x0 = (int) floor (x), y0 = (int) floor (y);
  double fx = x - (double) x0, fy = y - (double) y0;
  int x1 = x0 + 1, y1 = y0 + 1;
  double a, b, c, d;

  x0 = CLAMP (x0, 0, im->w - 1);
  x1 = CLAMP (x1, 0, im->w - 1);
  y0 = CLAMP (y0, 0, im->h - 1);
  y1 = CLAMP (y1, 0, im->h - 1);

  a = im->d[y0 * im->w + x0];
  b = im->d[y0 * im->w + x1];
  c = im->d[y1 * im->w + x0];
  d = im->d[y1 * im->w + x1];
  return a * (1 - fy) * (1 - fx) + b * (1 - fy) * fx +
         c * fy * (1 - fx) + d * fy * fx;
}

/* Separable convolution (index-clamped), same kernel both axes. */
static void
sep_conv (const Img *src, const double *kx, int kxl,
          const double *ky, int kyl, Img *dst)
{
  Img tmp;
  int cx = kxl / 2, cy = kyl / 2;

  img_alloc (&tmp, src->w, src->h, 1);
  for (int y = 0; y < src->h; y++)
    for (int x = 0; x < src->w; x++)
      {
        double acc = 0.0;

        for (int t = 0; t < kxl; t++)
          acc += kx[t] * src->d[y * src->w + CLAMP (x + t - cx, 0, src->w - 1)];
        tmp.d[y * src->w + x] = acc;
      }

  for (int y = 0; y < src->h; y++)
    for (int x = 0; x < src->w; x++)
      {
        double acc = 0.0;

        for (int t = 0; t < kyl; t++)
          acc += ky[t] * tmp.d[CLAMP (y + t - cy, 0, src->h - 1) * src->w + x];
        dst->d[y * src->w + x] = acc;
      }

  img_free (&tmp);
}

/* -------------------------------------------------------------------------
 * Stage 1-2: resample x1.75, normalise, enhance
 * ------------------------------------------------------------------------- */

static void
resample_x175 (const Img *src, Img *dst)
{
  const Pcfg *c = pcfg ();
  int dw = (c->resample_q10 * src->w + 0x1ff) >> 10;
  int dh = (c->resample_q10 * src->h + 0x1ff) >> 10;
  double scx, scy;

  if (dw < 1)
    dw = 1;
  if (dh < 1)
    dh = 1;

  img_alloc (dst, dw, dh, 0);
  scx = (double) src->w / dw;
  scy = (double) src->h / dh;

  for (int y = 0; y < dh; y++)
    {
      double sy = ((double) y + 0.5) * scy - 0.5;

      sy = CLAMP (sy, 0.0, (double) src->h - 1);
      for (int x = 0; x < dw; x++)
        {
          double sx = ((double) x + 0.5) * scx - 0.5;

          sx = CLAMP (sx, 0.0, (double) src->w - 1);
          dst->d[y * dw + x] = sample_bilinear (src, sx, sy);
        }
    }
}

static void
normalize_local (Img *im, int radius)
{
  int w = im->w, h = im->h;
  Img mean;

  img_alloc (&mean, w, h, 1);
  {
    double *ii = calloc ((size_t) (w + 1) * (h + 1), sizeof (double));

    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        ii[(y + 1) * (w + 1) + (x + 1)] =
          im->d[y * w + x] + ii[y * (w + 1) + (x + 1)] +
          ii[(y + 1) * (w + 1) + x] - ii[y * (w + 1) + x];

    for (int y = 0; y < h; y++)
      {
        int y0 = CLAMP (y - radius, 0, h), y1 = CLAMP (y + radius + 1, 0, h);
        for (int x = 0; x < w; x++)
          {
            int x0 = CLAMP (x - radius, 0, w), x1 = CLAMP (x + radius + 1, 0, w);
            double s = ii[y1 * (w + 1) + x1] - ii[y0 * (w + 1) + x1] -
                       ii[y1 * (w + 1) + x0] + ii[y0 * (w + 1) + x0];
            mean.d[y * w + x] = s / (double) ((x1 - x0) * (y1 - y0));
          }
      }
    free (ii);
  }

  for (int i = 0; i < w * h; i++)
    im->d[i] = CLAMP (im->d[i] - mean.d[i] + 128.0, 0.0, 255.0);
  img_free (&mean);
}

static void
enhance_local (Img *im, int radius)
{
  int w = im->w, h = im->h;
  Img out;

  img_alloc (&out, w, h, 0);
  for (int y = 0; y < h; y++)
    {
      int y0 = CLAMP (y - radius, 0, h - 1), y1 = CLAMP (y + radius, 0, h - 1);
      for (int x = 0; x < w; x++)
        {
          int x0 = CLAMP (x - radius, 0, w - 1), x1 = CLAMP (x + radius, 0, w - 1);
          double mn = 1e30, mx = -1e30, v;

          for (int yy = y0; yy <= y1; yy++)
            for (int xx = x0; xx <= x1; xx++)
              {
                double t = im->d[yy * w + xx];

                if (t < mn)
                  mn = t;
                if (t > mx)
                  mx = t;
              }
          v = (mx - mn > 1e-6) ? (im->d[y * w + x] - mn) * 255.0 / (mx - mn)
                               : im->d[y * w + x];
          out.d[y * w + x] = CLAMP (v, 0.0, 255.0);
        }
    }
  memcpy (im->d, out.d, (size_t) w * h * sizeof (double));
  img_free (&out);
}

static void
enhance_global (Img *im)
{
  double mn = 1e30, mx = -1e30;
  int n = im->w * im->h;

  for (int i = 0; i < n; i++)
    {
      if (im->d[i] < mn)
        mn = im->d[i];
      if (im->d[i] > mx)
        mx = im->d[i];
    }
  if (mx - mn <= 1e-6)
    return;
  for (int i = 0; i < n; i++)
    im->d[i] = CLAMP ((im->d[i] - mn) * 255.0 / (mx - mn), 0.0, 255.0);
}

/* -------------------------------------------------------------------------
 * Stage 3-4: structure tensor and response
 * ------------------------------------------------------------------------- */

static void
smooth_kernel (int sel, double *k)
{
  int idx = sel - 10;

  if (idx < 0)
    idx = 0;
  if (idx >= N_KERNELS)
    idx = N_KERNELS - 1;
  for (int t = 0; t < FIR_LEN[idx]; t++)
    k[t] = FIR_FLAT[FIR_OFF[idx] + t] / 32768.0;
}

/* Vendor first-derivative operator FUN_180011330 (p=2): 5x5 Sobel-like.
 * axis 0 -> out1 = Ix = dA/dx ; axis 1 -> out2 = Iy = dA/dy (positive up). */
static void
deriv5 (const Img *src, int axis, Img *dst)
{
  int w = src->w, h = src->h;
  const int p = 2;

#define SRC_A(dy, dx) \
  src->d[CLAMP (y + (dy), 0, h - 1) * w + CLAMP (x + (dx), 0, w - 1)]

  img_alloc (dst, w, h, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        double v;

        if (axis == 0)
          v = 3.0 * ((SRC_A (p, p) - SRC_A (-p, -p)) -
                     (SRC_A (p, -p) - SRC_A (-p, p))) +
              10.0 * (SRC_A (0, p) - SRC_A (0, -p));
        else
          v = 3.0 * ((SRC_A (p, -p) - SRC_A (-p, -p)) - SRC_A (-p, p) +
                     SRC_A (p, p)) +
              10.0 * (SRC_A (p, 0) - SRC_A (-p, 0));
        dst->d[y * w + x] = (v + 32.0) / 64.0;
      }
#undef SRC_A
}

static void
structure_tensor (const Img *sm,
                  Img *Ix, Img *Iy, Img *Ixx, Img *Ixy, Img *Iyy)
{
  deriv5 (sm, 0, Ix);
  deriv5 (sm, 1, Iy);
  deriv5 (Ix, 0, Ixx);
  deriv5 (Ix, 1, Ixy);
  deriv5 (Iy, 1, Iyy);
}

static void
hessian_response (const Img *Ixx, const Img *Ixy, const Img *Iyy, Img *det)
{
  int n = Ixx->w * Ixx->h;

  img_alloc (det, Ixx->w, Ixx->h, 0);
  for (int i = 0; i < n; i++)
    det->d[i] = (Ixx->d[i] * Iyy->d[i] - Ixy->d[i] * Ixy->d[i] + 7.0) / 16.0;
}

/* Quadratic (Hessian) sub-pixel refine of an interest point. */
static void
refine_subpixel (const Img *det, int x, int y, double *dx, double *dy)
{
  int w = det->w;
  double dxx, dyy, dxy, gx, gy, dt;

  *dx = 0.0;
  *dy = 0.0;
  dxx = det->d[y * w + x - 1] - 2.0 * det->d[y * w + x] + det->d[y * w + x + 1];
  dyy = det->d[(y - 1) * w + x] - 2.0 * det->d[y * w + x] + det->d[(y + 1) * w + x];
  dxy = (det->d[(y + 1) * w + x + 1] - det->d[(y + 1) * w + x - 1] -
         det->d[(y - 1) * w + x + 1] + det->d[(y - 1) * w + x - 1]) / 4.0;
  gx = (det->d[y * w + x + 1] - det->d[y * w + x - 1]) / 2.0;
  gy = (det->d[(y + 1) * w + x] - det->d[(y - 1) * w + x]) / 2.0;
  dt = dxx * dyy - dxy * dxy;
  if (fabs (dt) < 1e-12)
    return;
  *dx = -(dyy * gx - dxy * gy) / dt;
  *dy = -(-dxy * gx + dxx * gy) / dt;
  *dx = CLAMP (*dx, -0.5, 0.5);
  *dy = CLAMP (*dy, -0.5, 0.5);
}

/* -------------------------------------------------------------------------
 * Stage 6: exact atan2 LUT
 * ------------------------------------------------------------------------- */

static int
atan2_deg_lut (int cx, int cy)
{
  int ay = cy < 0 ? -cy : cy;
  int ax = cx < 0 ? -cx : cx;
  int t, idx;

  if (ay == 0)
    return cx > 0 ? 90 : 270;
  if (ax == 0)
    return cy > 0 ? 0 : 180;

  if (ax > ay)
    {
      idx = (ax / 2 + ay * 0x80) / ax;
      if (idx < 0)
        idx = 0;
      if (idx > 127)
        idx = 127;
      t = 0x2d0 - (int) atan2_lut_value (idx);   /* 720 - LUT */
    }
  else
    {
      idx = (ay / 2 + ax * 0x80) / ay;
      if (idx < 0)
        idx = 0;
      if (idx > 127)
        idx = 127;
      t = (int) atan2_lut_value (idx);
    }

  if (cy <= 0)
    {
      if (cx < 0)
        return (t + 0x5a4) >> 3;                 /* 1444 */
      t = 0x5a0 - t;                             /* 1440 - t */
    }
  else
    {
      if (cx >= 0)
        return (t + 4) >> 3;
      if (t < 5)
        return 0;
      t = 0xb40 - t;                             /* 2880 - t */
    }
  return (t + 4) >> 3;
}

/* -------------------------------------------------------------------------
 * Stage 6: orientation histogram (45 bins, shift 3, 9-bin sliding window)
 * ------------------------------------------------------------------------- */

static void
feature_orientation (const Img *Ix, const Img *Iy, int x, int y,
                     const Pcfg *c, int *orient_out, int *angle_out,
                     double *margin_out,
                     double *records, int *dominant_out)
{
  int spacing = EGIS057E_CFG_ORIENT_SPACING;

  if (c->orient_mode == 1)
    {
      /* fold2theta: accumulate at doubled angle so opposite gradients
       * coincide (ridge orientation is 180deg-ambiguous). */
      int fb = c->fold_bins > 0 ? c->fold_bins : 1;
      double c2 = 0.0, s2 = 0.0, sm = 0.0;
      double mag2, phi, deg;

      for (int gy = -5; gy <= 5; gy++)
        for (int gx = -5; gx <= 5; gx++)
          {
            int px = x + gx * spacing, py = y + gy * spacing;
            double w, a, b, m, th;
            int ang;

            if (px < 0 || px >= Ix->w || py < 0 || py >= Ix->h)
              continue;
            w = (double) PYR[gy + 5][gx + 5];
            a = w * Ix->d[py * Ix->w + px];
            b = w * Iy->d[py * Ix->w + px];
            m = sqrt (a * a + b * b);
            if (m < 1e-12)
              continue;
            {
              double sa = CLAMP (a * 256.0, -1.0e7, 1.0e7);
              double sb = CLAMP (b * 256.0, -1.0e7, 1.0e7);

              ang = atan2_deg_lut ((int) lround (sb), (int) lround (sa));
            }
            th = ang * M_PI / 180.0;
            c2 += m * cos (2.0 * th);
            s2 += m * sin (2.0 * th);
            sm += m;
          }

      mag2 = sqrt (c2 * c2 + s2 * s2);
      phi = 0.5 * atan2 (s2, c2);
      deg = phi * 180.0 / M_PI;
      if (deg < 0.0)
        deg += 180.0;

      *orient_out = (int) (deg / (180.0 / fb));
      if (*orient_out < 0)
        *orient_out = 0;
      if (*orient_out >= fb)
        *orient_out = fb - 1;
      *angle_out = ((int) lround (deg)) % 180;
      *margin_out = sm > 1e-12 ? 1.0 - mag2 / sm : 1.0;
      if (dominant_out)
        *dominant_out = *orient_out;
      return;
    }

  {
    int shift = c->orient_shift;
    int bins = 360 >> shift;
    int offset = (EGIS057E_CFG_ORIENT_OFFSET);   /* cfg[0xf]/10>>shift = 8 */
    int win = offset + 1;
    int total = bins + offset;
    double *hx = calloc ((size_t) total, sizeof (double));
    double *hy = calloc ((size_t) total, sizeof (double));
    int best = 0;
    double best_mag = -1.0, second_mag = -1.0;
    double bx = 0, by = 0;

    if (!hx || !hy)
      {
        free (hx);
        free (hy);
        *orient_out = 0;
        *angle_out = 0;
        *margin_out = 1.0;
        return;
      }

    for (int gy = -5; gy <= 5; gy++)
      for (int gx = -5; gx <= 5; gx++)
        {
          int px = x + gx * spacing, py = y + gy * spacing;
          double w, a, b;
          int ang, bin;

          if (px < 0 || px >= Ix->w || py < 0 || py >= Ix->h)
            continue;
          w = (double) PYR[gy + 5][gx + 5];
          a = w * Ix->d[py * Ix->w + px];
          b = w * Iy->d[py * Ix->w + px];
          {
            double sa = CLAMP (a * 256.0, -1.0e7, 1.0e7);
            double sb = CLAMP (b * 256.0, -1.0e7, 1.0e7);

            ang = atan2_deg_lut ((int) lround (sb), (int) lround (sa));
          }
          bin = ang >> shift;
          if (bin < 0)
            bin = 0;
          if (bin >= bins)
            bin = bins - 1;
          hx[bin] += a;
          hy[bin] += b;
        }

    for (int b = 0; b < offset; b++)
      {
        hx[bins + b] = hx[b];
        hy[bins + b] = hy[b];
      }

    for (int start = 0; start < bins; start++)
      {
        double sx = 0, sy = 0, m;

        for (int k = 0; k < win; k++)
          {
            sx += hx[start + k];
            sy += hy[start + k];
          }
        m = (double) (((long long) (sx + 0x1000) >> 13) * ((long long) (sx + 0x1000) >> 13) +
                      ((long long) (sy + 0x1000) >> 13) * ((long long) (sy + 0x1000) >> 13));
        if (records)
          {
            records[start * 3 + 0] = sx;
            records[start * 3 + 1] = sy;
            records[start * 3 + 2] = m;
          }
        if (m > best_mag)
          {
            best_mag = m;
            best = start;
            bx = sx;
            by = sy;
          }
      }
    second_mag = -1.0;
    for (int start = 0; start < bins; start++)
      {
        double sx = 0, sy = 0, m;
        int d1 = (start - best + bins) % bins;
        int d2 = (best - start + bins) % bins;

        if (d1 <= win - 1 || d2 <= win - 1)
          continue;
        for (int k = 0; k < win; k++)
          {
            sx += hx[start + k];
            sy += hy[start + k];
          }
        m = (double) (((long long) (sx + 0x1000) >> 13) * ((long long) (sx + 0x1000) >> 13) +
                      ((long long) (sy + 0x1000) >> 13) * ((long long) (sy + 0x1000) >> 13));
        if (m > second_mag)
          second_mag = m;
      }

    *orient_out = best;
    if (dominant_out)
      *dominant_out = best;
    {
      long long ibx = (long long) lround (bx * 256.0);
      long long iby = (long long) lround (by * 256.0);

      ibx = CLAMP (ibx, -10000000LL, 10000000LL);
      iby = CLAMP (iby, -10000000LL, 10000000LL);
      /* vendor: feature[+0xe] = atan2_deg(sumY, sumX) */
      *angle_out = atan2_deg_lut ((int) iby, (int) ibx);
    }
    *margin_out = (best_mag > 1e-12 && second_mag > 0)
                    ? sqrt (second_mag / best_mag) : 1.0;

    free (hx);
    free (hy);
  }
}

/* -------------------------------------------------------------------------
 * Stage 7: descriptor (rotated n x n block means, pairwise rank comparisons)
 * ------------------------------------------------------------------------- */

/*
 * Vendor block_means_grid grid geometry (spec §2/§6a): n x n centres at
 * (start + k*step), start = -9.5 px, step = (2*cfg4+1)/n px; per block a
 * rotated cell of side ~step is bilinearly averaged from the 3 channels
 * (ch0 smoothed intensity, ch1 Ix, ch2 Iy), and the Ix/Iy means are rotated
 * into the feature orientation. vals[block*3 + channel].
 */
/* Vendor FUN_180018380 16.16 fixed-point bilinear sample with the two-stage
 * 16-bit truncation (spec §6a). Returns +inf to signal out-of-bounds (the
 * vendor excludes OOB samples from the block average). */
static double
bilinear_vendor (const Img *im, double x, double y)
{
  long X = lround (x * 65536.0);
  long Y = lround (y * 65536.0);
  int ix = (int) (X >> 16);
  int iy = (int) (Y >> 16);
  int fx = (int) (X & 0xffff);
  int fy = (int) (Y & 0xffff);
  int ix1, iy1;
  long top, bot;

  if (ix < 0 || ix >= im->w || iy < 0 || iy >= im->h)
    return 1.0 / 0.0;   /* +inf: out of bounds, excluded */

  ix1 = ix + 1 < im->w ? ix + 1 : im->w - 1;
  iy1 = iy + 1 < im->h ? iy + 1 : im->h - 1;

  top = ((long) ((double) fx * im->d[iy1 * im->w + ix1] +
                 (double) (0x10000 - fx) * im->d[iy1 * im->w + ix])) >> 16;
  bot = ((long) ((double) (0x10000 - fx) * im->d[iy * im->w + ix] +
                 (double) fx * im->d[iy * im->w + ix1])) >> 16;
  return (double) (((long) top * fy + (long) bot * (0x10000 - fy)) >> 16);
}

static void
block_means_grid (const Img *ch[3], double x, double y, double angle,
                  int n, double cfg4, double *vals)
{
  const Pcfg *pc = pcfg ();
  double cs = cos (angle * M_PI / 180.0);
  double sn = sin (angle * M_PI / 180.0);
  double start_px = -(cfg4 * 1024.0 + 512.0) / 1024.0;   /* -9.5 px */
  double step_px = ((2.0 * cfg4 + 1.0) / n);
  double half = step_px / 2.0;

  for (int gy = 0; gy < n; gy++)
    for (int gx = 0; gx < n; gx++)
      {
        int block = gy * n + gx;
        double ccx = x + start_px + gx * step_px;
        double ccy = y + start_px + gy * step_px;
        double si = 0, sx = 0, sy = 0;
        int cnt = 0;

        for (double v = -half; v <= half + 1e-9; v += 1.0)
          for (double u = -half; u <= half + 1e-9; u += 1.0)
            {
              double rx = u * cs - v * sn;
              double ry = u * sn + v * cs;
              double px = ccx + rx, py = ccy + ry;
              double vi, vxg, vyg;

              if (pc->sampler == 1)
                {
                  vi = bilinear_vendor (ch[0], px, py);
                  if (isinf (vi))
                    continue;   /* out of bounds: excluded */
                  vxg = bilinear_vendor (ch[1], px, py);
                  vyg = bilinear_vendor (ch[2], px, py);
                }
              else
                {
                  vi = sample_bilinear (ch[0], px, py);
                  vxg = sample_bilinear (ch[1], px, py);
                  vyg = sample_bilinear (ch[2], px, py);
                }
              si += vi;
              sx += vxg;
              sy += vyg;
              cnt++;
            }

        if (cnt < 12)
          {
            vals[block * 3 + 0] = -1.0;
            vals[block * 3 + 1] = -1.0;
            vals[block * 3 + 2] = -1.0;
            continue;
          }
        {
          double mi = si / cnt, mix = sx / cnt, miy = sy / cnt;

          vals[block * 3 + 0] = mi;
          vals[block * 3 + 1] = mix * cs - miy * sn;   /* rotate gradient +angle */
          vals[block * 3 + 2] = mix * sn + miy * cs;
        }
      }
}

static void
set_bit (uint8_t *desc, int bit)
{
  if (bit < EGIS057E_M_DESC_BYTES * 8)
    desc[bit >> 3] |= (uint8_t) (1u << (bit & 7));
}

static void
build_descriptor (const Img *ch[3], double x, double y, int angle,
                  const Pcfg *c, uint8_t *desc)
{
  int bit = 0;

  memset (desc, 0, EGIS057E_M_DESC_BYTES);

  for (int n = 2; n <= 4; n++)
    {
      double vals[16 * 3];
      int n2 = n * n;

      block_means_grid (ch, x, y, (double) angle, n, (double) c->desc_cfg4, vals);

      for (int c_i = 0; c_i < 3; c_i++)
        for (int i = 0; i < n2; i++)
          for (int j = i + 1; j < n2; j++)
            {
              if (vals[c_i + j * 3] < vals[c_i + i * 3])
                set_bit (desc, bit);
              bit++;
            }
    }
}

/* -------------------------------------------------------------------------
 * Interest points + full extraction
 * ------------------------------------------------------------------------- */

typedef struct
{
  int    x, y;
  double v;
} Cand;

static int
cmp_cand (const void *a, const void *b)
{
  const Cand *ca = a, *cb = b;

  if (ca->v < cb->v)
    return 1;
  if (ca->v > cb->v)
    return -1;
  return 0;
}

void
egis057e_m_extract_ex (const uint8_t *img, int w, int h,
                       Egis057eMFeatureSet *out, double *orient_margin_out)
{
  Img src, rs, sm, Ix, Iy, Ixx, Ixy, Iyy, det;
  const Img *channels[3];
  const Pcfg *c = pcfg ();
  int nfeat = 0;
  Cand *cands;
  int ncand = 0;
  double margin_sum = 0.0;
  int margin_cnt = 0;

  out->n = 0;
  egis057e_m_feature_set_clear (out);
  if (orient_margin_out)
    *orient_margin_out = 1.0;
  if (w < 8 || h < 8)
    return;

  src.w = w;
  src.h = h;
  src.d = malloc ((size_t) w * h * sizeof (double));
  if (!src.d)
    return;
  for (int i = 0; i < w * h; i++)
    src.d[i] = (double) img[i];

  resample_x175 (&src, &rs);
  img_free (&src);

  if (c->norm_on)
    normalize_local (&rs, c->norm_radius);

  if (c->enhance == 2)
    enhance_local (&rs, c->enh_radius);
  else if (c->enhance == 1)
    enhance_global (&rs);

  {
    double k[11];
    int ki = CLAMP (c->fir_sel - 10, 0, N_KERNELS - 1);

    smooth_kernel (c->fir_sel, k);
    img_alloc (&sm, rs.w, rs.h, 1);
    sep_conv (&rs, k, FIR_LEN[ki], k, FIR_LEN[ki], &sm);
  }
  img_free (&rs);

  structure_tensor (&sm, &Ix, &Iy, &Ixx, &Ixy, &Iyy);
  hessian_response (&Ixx, &Ixy, &Iyy, &det);

  cands = malloc (sizeof (Cand) * (size_t) (det.w * det.h));
  if (!cands)
    goto cleanup;
  {
    double resp_min = c->resp_thresh;

    if (resp_min <= 0.0 && c->resp_frac > 0.0)
      {
        double mx = 0.0;

        for (int i = 0; i < det.w * det.h; i++)
          if (det.d[i] > mx)
            mx = det.d[i];
        resp_min = c->resp_frac * mx;
      }

    for (int y = 1; y + 1 < det.h; y++)
      for (int x = 1; x + 1 < det.w; x++)
        {
          double v = det.d[y * det.w + x];
          int is_max = 1;

          if (v <= resp_min)
            continue;
          for (int dy = -1; dy <= 1 && is_max; dy++)
            for (int dx = -1; dx <= 1; dx++)
              {
                if (dx == 0 && dy == 0)
                  continue;
                if (det.d[(y + dy) * det.w + x + dx] > v)
                  {
                    is_max = 0;
                    break;
                  }
              }
          if (is_max)
            {
              cands[ncand].x = x;
              cands[ncand].y = y;
              cands[ncand].v = v;
              ncand++;
            }
        }
  }
  qsort (cands, (size_t) ncand, sizeof (Cand), cmp_cand);

  channels[0] = &sm;   /* ch0 = smoothed intensity */
  channels[1] = &Ix;   /* ch1 = Ix */
  channels[2] = &Iy;   /* ch2 = Iy */

  {
    int obins = (c->orient_mode == 0) ? (360 >> c->orient_shift) : 0;
    double *records = (c->multihyp && obins > 0)
                        ? malloc (sizeof (double) * (size_t) obins * 3) : NULL;

    for (int i = 0; i < ncand && nfeat < EGIS057E_M_MAX_FEATURES; i++)
      {
        int x = cands[i].x, y = cands[i].y;
        int ok = 1, orient, angle, dominant = 0, pol;
        double margin, cxr = (double) x, cyr = (double) y;

        for (int k = 0; k < nfeat; k++)
          {
            int ddx = x - out->f[k].x, ddy = y - out->f[k].y;

            if (ddx * ddx + ddy * ddy < c->nms_r2)
              {
                ok = 0;
                break;
              }
          }
        if (!ok)
          continue;

        if (c->subpix)
          {
            double pdx, pdy;

            refine_subpixel (&det, x, y, &pdx, &pdy);
            cxr = (double) x + pdx;
            cyr = (double) y + pdy;
          }

        feature_orientation (&Ix, &Iy, x, y, c, &orient, &angle, &margin,
                             records, &dominant);
        pol = Ixx.d[y * Ixx.w + x] < 0.0 ? 1 : 0;

        {
          double pmax = (records && obins > 0) ? records[dominant * 3 + 2] : 0.0;
          double thr = (c->hyp_thr * pmax) / 1024.0;
          int lastk = dominant, emitted = 0;
          int cur_angle = angle, cur_orient = orient;

          while (nfeat < EGIS057E_M_MAX_FEATURES)
            {
              int arange = (c->orient_mode == 1) ? 180 : 360;
              int cb = c->coarse_bins > 0 ? c->coarse_bins : 1;
              int b = (int) ((double) cur_angle * cb / (double) arange);

              if (b < 0)
                b = 0;
              if (b >= cb)
                b = cb - 1;
              out->f[nfeat].x = (uint16_t) lround (cxr);
              out->f[nfeat].y = (uint16_t) lround (cyr);
              out->f[nfeat].orient = (uint8_t) cur_orient;
              out->f[nfeat].angle = (uint16_t) cur_angle;
              out->f[nfeat].bucket = (uint8_t) b;
              out->f[nfeat].polarity = (uint8_t) pol;
              build_descriptor (channels, cxr, cyr, cur_angle, c,
                                out->f[nfeat].desc);
              if (emitted == 0)
                {
                  margin_sum += margin;
                  margin_cnt++;
                }
              nfeat++;
              emitted++;

              if (!(c->multihyp && records && obins > 0) || emitted >= 3)
                break;

              /* suppress a 9-bin window centred at lastk-4 (vendor) */
              for (int kk = 0; kk < 9; kk++)
                {
                  int idx = ((lastk - 4 + kk) % obins + obins) % obins;

                  records[idx * 3 + 2] = 0.0;
                }
              {
                int kbest = -1;
                double bm = -1.0;

                for (int s = 0; s < obins; s++)
                  if (records[s * 3 + 2] > bm)
                    {
                      bm = records[s * 3 + 2];
                      kbest = s;
                    }
                if (kbest < 0 || bm < thr)
                  break;
                {
                  double sX = records[kbest * 3 + 0];
                  double sY = records[kbest * 3 + 1];
                  long long isx = (long long) lround (sX * 256.0);
                  long long isy = (long long) lround (sY * 256.0);

                  isx = CLAMP (isx, -10000000LL, 10000000LL);
                  isy = CLAMP (isy, -10000000LL, 10000000LL);
                  cur_angle = atan2_deg_lut ((int) isy, (int) isx);
                  cur_orient = kbest;
                }
                lastk = kbest;
              }
            }
        }
      }
    free (records);
  }

  out->n = nfeat;
  if (orient_margin_out)
    *orient_margin_out = margin_cnt > 0 ? margin_sum / margin_cnt : 1.0;

  /* Retain the descriptor sampling channels for geometric re-extraction. */
  {
    int cw = sm.w, chh = sm.h;
    size_t nb = (size_t) cw * chh * sizeof (double);

    out->chan_w = cw;
    out->chan_h = chh;
    out->chan_sm = malloc (nb);
    out->chan_ix = malloc (nb);
    out->chan_iy = malloc (nb);
    if (out->chan_sm && out->chan_ix && out->chan_iy)
      {
        memcpy (out->chan_sm, sm.d, nb);
        memcpy (out->chan_ix, Ix.d, nb);
        memcpy (out->chan_iy, Iy.d, nb);
      }
    else
      egis057e_m_feature_set_clear (out);
  }

cleanup:
  free (cands);
  img_free (&sm);
  img_free (&Ix);
  img_free (&Iy);
  img_free (&Ixx);
  img_free (&Ixy);
  img_free (&Iyy);
  img_free (&det);
}

void
egis057e_m_extract (const uint8_t *img, int w, int h,
                    Egis057eMFeatureSet *out)
{
  egis057e_m_extract_ex (img, w, h, out, NULL);
}

void
egis057e_m_feature_set_clear (Egis057eMFeatureSet *set)
{
  if (!set)
    return;
  free (set->chan_sm);
  set->chan_sm = NULL;
  free (set->chan_ix);
  set->chan_ix = NULL;
  free (set->chan_iy);
  set->chan_iy = NULL;
  set->chan_w = 0;
  set->chan_h = 0;
}

int
egis057e_m_orient_bins (void)
{
  const Pcfg *c = pcfg ();

  return c->orient_mode == 1 ? c->fold_bins : (360 >> c->orient_shift);
}

/* -------------------------------------------------------------------------
 * Stage 8: matching
 * ------------------------------------------------------------------------- */

static int
hamming512 (const uint8_t *a, const uint8_t *b)
{
  int d = 0;

  for (int i = 0; i < EGIS057E_M_DESC_BYTES; i += 8)
    {
      uint64_t x, y;

      memcpy (&x, a + i, 8);
      memcpy (&y, b + i, 8);
      d += __builtin_popcountll (x ^ y);
    }
  return d;
}

/* Vendor verify_matches_geom core: rebuild the probe descriptor at the pose
 * predicted by the estimated alignment (position mapped through -dphi about
 * the probe centroid and the (tx,ty) translation; orientation recomputed on
 * the probe channels) and re-check the Hamming budget against the gallery
 * descriptor. Returns 1 when the match is confirmed (or unverifiable). */
static int
reextract_ok (const Egis057eMFeatureSet *probe, int i,
              const Egis057eMFeatureSet *gallery, int j, int stored_h,
              double dphi, double tx, double ty,
              double pcx, double pcy, const Pcfg *c)
{
  Img sm, ix, iy;
  const Img *ch[3];
  double cs, sn, rx, ry, qx, qy;
  int angle;
  uint8_t d[EGIS057E_M_DESC_BYTES];

  (void) j;
  if (!c->reextract || !gallery->chan_sm || !gallery->chan_ix ||
      !gallery->chan_iy)
    return 1;

  /* sample the counterpart (gallery) image at the probe feature's pose */
  sm.w = gallery->chan_w;
  sm.h = gallery->chan_h;
  sm.d = gallery->chan_sm;
  ix.w = gallery->chan_w;
  ix.h = gallery->chan_h;
  ix.d = gallery->chan_ix;
  iy.w = gallery->chan_w;
  iy.h = gallery->chan_h;
  iy.d = gallery->chan_iy;
  ch[0] = &sm;
  ch[1] = &ix;
  ch[2] = &iy;

  cs = cos (dphi * M_PI / 180.0);
  sn = sin (dphi * M_PI / 180.0);
  rx = pcx + (probe->f[i].x - pcx) * cs + (probe->f[i].y - pcy) * sn;
  ry = pcy - (probe->f[i].x - pcx) * sn + (probe->f[i].y - pcy) * cs;
  qx = rx - tx;
  qy = ry - ty;

  angle = ((int) probe->f[i].angle - (int) lround (dphi)) % 360;
  if (angle < 0)
    angle += 360;

  build_descriptor (ch, qx, qy, angle, c, d);
  {
    int rh = hamming512 (d, probe->f[i].desc);

    return rh <= c->budget || rh <= stored_h + c->reextract_slack;
  }
}

/* Fold an angle difference to [0,180] (vendor matcher). */
static int
fold180 (int d)
{
  d %= 360;
  if (d < 0)
    d += 360;
  if (d > 180)
    d = 360 - d;
  return d;
}

int
egis057e_m_score (const Egis057eMFeatureSet *probe,
                  const Egis057eMFeatureSet *gallery,
                  int *votes_out)
{
  int n1 = probe->n, n2 = gallery->n;
  const Pcfg *c = pcfg ();
  int *ci, *cj, *ch, *keep, *tbx, *tby;
  int ncand = 0, score = 0, votes = 0;
  int atol = (int) c->align_atol, ptol = (int) c->align_ptol;
  double pcx = 0.0, pcy = 0.0, dphi = 0.0, tx = 0.0, ty = 0.0;

  if (votes_out)
    *votes_out = 0;
  if (n1 < EGIS057E_M_MIN_MATCHED || n2 < EGIS057E_M_MIN_MATCHED)
    return 0;

  ci = malloc (sizeof (int) * n1);
  cj = malloc (sizeof (int) * n1);
  ch = malloc (sizeof (int) * n1);
  keep = malloc (sizeof (int) * n1);
  tbx = malloc (sizeof (int) * n1);
  tby = malloc (sizeof (int) * n1);
  if (!ci || !cj || !ch || !keep || !tbx || !tby)
    {
      free (ci);
      free (cj);
      free (ch);
      free (keep);
      free (tbx);
      free (tby);
      return 0;
    }

  for (int i = 0; i < n1; i++)
    {
      pcx += probe->f[i].x;
      pcy += probe->f[i].y;
    }
  pcx /= (double) n1;
  pcy /= (double) n1;

  /* Candidate generation: Hamming NN + ratio test (same-location twins are
   * exempted from the second-best) + Hamming budget. */
  for (int i = 0; i < n1; i++)
    {
      int best = 512, bj = -1, second = 512;
      int xbest, ybest;

      for (int j = 0; j < n2; j++)
        {
          int h;

          if (c->polarity_gate &&
              probe->f[i].polarity != gallery->f[j].polarity)
            continue;
          h = hamming512 (probe->f[i].desc, gallery->f[j].desc);
          if (h < best)
            {
              best = h;
              bj = j;
            }
        }
      if (bj < 0)
        continue;
      xbest = gallery->f[bj].x;
      ybest = gallery->f[bj].y;
      for (int j = 0; j < n2; j++)
        {
          int h, ddx, ddy;

          if (c->polarity_gate &&
              probe->f[i].polarity != gallery->f[j].polarity)
            continue;
          ddx = (int) gallery->f[j].x - xbest;
          ddy = (int) gallery->f[j].y - ybest;
          if (abs (ddx) <= 2 && abs (ddy) <= 2)
            continue;   /* vendor ±180 twin at the same location */
          h = hamming512 (probe->f[i].desc, gallery->f[j].desc);
          if (h < second)
            second = h;
        }

      if (best <= c->budget &&
          best * 1024 < second * c->ratio_q10)
        {
          ci[ncand] = i;
          cj[ncand] = bj;
          ch[ncand] = best;
          ncand++;
        }
    }

  if (ncand == 0)
    {
      free (ci);
      free (cj);
      free (ch);
      free (keep);
      free (tbx);
      free (tby);
      return 0;
    }

  for (int k = 0; k < ncand; k++)
    keep[k] = 1;

  /* Iterative rigid alignment (vendor translation_hist_align): alternate
   * folded-angle mode dphi and rotation-compensated translation mode (dx,dy),
   * re-filtering candidates each pass. */
  for (int iter = 0; iter < 3; iter++)
    {
      int hb[181], bestc = 0, bc = -1, bcnt = 0;
      double cs, sn;

      memset (hb, 0, sizeof (hb));
      for (int k = 0; k < ncand; k++)
        if (keep[k])
          {
            int d = fold180 ((int) probe->f[ci[k]].angle -
                             (int) gallery->f[cj[k]].angle);

            hb[d]++;
          }
      for (int b = 1; b <= 180; b++)
        if (hb[b] > hb[bestc])
          bestc = b;
      dphi = (double) bestc;

      cs = cos (dphi * M_PI / 180.0);
      sn = sin (dphi * M_PI / 180.0);

      for (int k = 0; k < ncand; k++)
        if (keep[k])
          {
            double rx = pcx + (probe->f[ci[k]].x - pcx) * cs +
                        (probe->f[ci[k]].y - pcy) * sn;
            double ry = pcy - (probe->f[ci[k]].x - pcx) * sn +
                        (probe->f[ci[k]].y - pcy) * cs;

            tbx[k] = (int) floor ((rx - gallery->f[cj[k]].x) / 8.0 + 0.5);
            tby[k] = (int) floor ((ry - gallery->f[cj[k]].y) / 8.0 + 0.5);
          }

      for (int k = 0; k < ncand; k++)
        {
          int cnt = 0;

          if (!keep[k])
            continue;
          for (int m = 0; m < ncand; m++)
            if (keep[m] && tbx[m] == tbx[k] && tby[m] == tby[k])
              cnt++;
          if (cnt > bcnt)
            {
              bcnt = cnt;
              bc = k;
            }
        }
      if (bc < 0)
        break;
      tx = tbx[bc] * 8.0;
      ty = tby[bc] * 8.0;

      for (int k = 0; k < ncand; k++)
        {
          double rx, ry, dx, dy;
          int ad;

          if (!keep[k])
            continue;
          rx = pcx + (probe->f[ci[k]].x - pcx) * cs +
               (probe->f[ci[k]].y - pcy) * sn;
          ry = pcy - (probe->f[ci[k]].x - pcx) * sn +
               (probe->f[ci[k]].y - pcy) * cs;
          dx = rx - gallery->f[cj[k]].x;
          dy = ry - gallery->f[cj[k]].y;
          ad = fold180 ((int) probe->f[ci[k]].angle -
                        (int) gallery->f[cj[k]].angle - (int) dphi);
          if (fabs (dx - tx) > ptol || fabs (dy - ty) > ptol || ad > atol)
            keep[k] = 0;
        }
    }

  /* refine the translation to the mean of the kept inliers so the
   * re-extraction pose is sub-pixel accurate (the histogram mode is 8px) */
  {
    double sdx = 0.0, sdy = 0.0, cs = cos (dphi * M_PI / 180.0);
    double sn = sin (dphi * M_PI / 180.0);
    int nk = 0;

    for (int k = 0; k < ncand; k++)
      if (keep[k])
        {
          double rx = pcx + (probe->f[ci[k]].x - pcx) * cs +
                      (probe->f[ci[k]].y - pcy) * sn;
          double ry = pcy - (probe->f[ci[k]].x - pcx) * sn +
                      (probe->f[ci[k]].y - pcy) * cs;

          sdx += rx - gallery->f[cj[k]].x;
          sdy += ry - gallery->f[cj[k]].y;
          nk++;
        }
    if (nk > 0)
      {
        tx = sdx / nk;
        ty = sdy / nk;
      }
  }

  for (int k = 0; k < ncand; k++)
    if (keep[k])
      {
        if (!reextract_ok (probe, ci[k], gallery, cj[k], ch[k], dphi, tx, ty,
                           pcx, pcy, c))
          continue;
        score += 128 - ch[k];
        votes++;
      }

  free (ci);
  free (cj);
  free (ch);
  free (keep);
  free (tbx);
  free (tby);
  if (votes_out)
    *votes_out = votes;
  return score;
}
