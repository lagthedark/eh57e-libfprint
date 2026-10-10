/*
 * egis057e-match-test - offline EH57E matcher harness
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Compiles the SAME egis057e-matcher.c as the libfprint driver, so feature
 * extraction and scoring are bit-identical to the runtime path.
 *
 * Modes:
 *   egis057e-match-test [--bg <bg.pgm>] features <image.pgm>
 *       Print the feature count and each feature as:
 *         <x> <y> <orient> <64-byte descriptor hex>
 *
 *   egis057e-match-test [--bg <bg.pgm>] score <gallery.pgm> <probe.pgm>
 *       Print: features=<gallery>/<probe> votes=<N> score=<S>
 *
 *   egis057e-match-test [--bg <bg.pgm>] diagnose <imageA.pgm> <imageB.pgm>
 *       Print the forensics metrics that locate orientation-readout
 *       instability: IP correspondence/repeatability, orientation margin,
 *       orientation agreement at correspondences, true-correspondence vs
 *       random/cross NN Hamming, and perturbation-stability of the verdict
 *       under +/-1 gray and +/-0.05 scale. The metric definitions mirror the
 *       numpy reference (analysis/eh57e_diag*.py), including the reference's
 *       best-translation selection and first-j correspondence dedupe.
 *
 * --bg applies EH575-style background subtraction before extraction:
 *       out = (v > bg + 2) ? (v - bg) : 0
 *
 * PGM input is binary P5, 8-bit (maxval 255), any geometry.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "egis057e-matcher.h"

typedef struct
{
  int      width;
  int      height;
  uint8_t *data;
} PgmImage;

static void
pgm_free (PgmImage *img)
{
  free (img->data);
  img->data = NULL;
  img->width = img->height = 0;
}

static int
pgm_load (const char *path, PgmImage *out)
{
  FILE *f = fopen (path, "rb");
  char magic[3] = { 0 };
  int w, h, maxval;
  int c;
  size_t n;

  if (!f)
    {
      fprintf (stderr, "error: cannot open %s\n", path);
      return -1;
    }

  if (fscanf (f, "%2s", magic) != 1 || strcmp (magic, "P5") != 0)
    {
      fprintf (stderr, "error: %s is not a binary P5 PGM\n", path);
      fclose (f);
      return -1;
    }

  {
    int vals[3] = { 0, 0, 0 };
    int got = 0;

    while (got < 3)
      {
        c = fgetc (f);
        if (c == EOF)
          break;
        if (c == '#')
          {
            while (c != '\n' && c != EOF)
              c = fgetc (f);
            continue;
          }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
          continue;
        ungetc (c, f);
        if (fscanf (f, "%d", &vals[got]) != 1)
          break;
        got++;
      }

    if (got < 3)
      {
        fprintf (stderr, "error: malformed PGM header in %s\n", path);
        fclose (f);
        return -1;
      }
    w = vals[0];
    h = vals[1];
    maxval = vals[2];
  }

  if (w <= 0 || h <= 0 || maxval != 255)
    {
      fprintf (stderr, "error: unsupported PGM %dx%d maxval=%d (need 8-bit)\n",
               w, h, maxval);
      fclose (f);
      return -1;
    }

  c = fgetc (f);
  if (c == EOF)
    {
      fprintf (stderr, "error: truncated PGM %s\n", path);
      fclose (f);
      return -1;
    }

  out->data = malloc ((size_t) w * h);
  if (!out->data)
    {
      fprintf (stderr, "error: out of memory\n");
      fclose (f);
      return -1;
    }

  n = fread (out->data, 1, (size_t) w * h, f);
  fclose (f);
  if (n != (size_t) w * h)
    {
      fprintf (stderr, "error: short raster in %s (%zu/%d)\n", path, n, w * h);
      pgm_free (out);
      return -1;
    }

  out->width = w;
  out->height = h;
  return 0;
}

/* EH575-style background subtraction, in place. */
static void
pgm_subtract_bg (PgmImage *img, const PgmImage *bg)
{
  size_t n = (size_t) img->width * img->height;

  for (size_t i = 0; i < n; i++)
    {
      int v = img->data[i];
      int b = bg->data[i];

      img->data[i] = v > b + 2 ? (uint8_t) (v - b) : 0;
    }
}

static double
round_half_even (double x)
{
  double f = floor (x);
  double diff = x - f;

  if (diff > 0.5)
    return f + 1.0;
  if (diff < 0.5)
    return f;
  /* exactly .5 -> round to even */
  return fmod (f, 2.0) == 0.0 ? f : f + 1.0;
}

/* Bilinear resize matching the numpy reference (analysis/eh57e_diag.py):
 * pixel-centre-aligned mapping, clamped borders, then (v + 0.5) truncation. */
static int
pgm_resize_bilinear (const PgmImage *src, double s, PgmImage *dst)
{
  int H = (int) round_half_even ((double) src->height * s);
  int W = (int) round_half_even ((double) src->width * s);
  int h = src->height, w = src->width;

  if (H < 1 || W < 1)
    return -1;

  dst->data = malloc ((size_t) W * H);
  if (!dst->data)
    return -1;
  dst->width = W;
  dst->height = H;

  for (int r = 0; r < H; r++)
    {
      double ys = ((double) r + 0.5) / s - 0.5;
      int yi;
      double fy;

      if (ys < 0)
        ys = 0;
      if (ys > h - 1)
        ys = h - 1;
      yi = (int) floor (ys);
      if (yi > h - 2)
        yi = h - 2;
      fy = ys - (double) yi;

      for (int c = 0; c < W; c++)
        {
          double xs = ((double) c + 0.5) / s - 0.5;
          int xi, x1, y1;
          double fx, v;

          if (xs < 0)
            xs = 0;
          if (xs > w - 1)
            xs = w - 1;
          xi = (int) floor (xs);
          if (xi > w - 2)
            xi = w - 2;
          fx = xs - (double) xi;
          x1 = xi + 1;
          y1 = yi + 1;

          v = src->data[yi * w + xi] * (1 - fy) * (1 - fx) +
              src->data[yi * w + x1] * (1 - fy) * fx +
              src->data[y1 * w + xi] * fy * (1 - fx) +
              src->data[y1 * w + x1] * fy * fx;
          v = floor (v + 0.5);
          if (v < 0)
            v = 0;
          if (v > 255)
            v = 255;
          dst->data[r * W + c] = (uint8_t) v;
        }
    }
  return 0;
}

static int
pgm_dither (const PgmImage *src, int delta, PgmImage *dst)
{
  size_t n = (size_t) src->width * src->height;

  dst->data = malloc (n);
  if (!dst->data)
    return -1;
  dst->width = src->width;
  dst->height = src->height;

  for (size_t i = 0; i < n; i++)
    {
      int v = (int) src->data[i] + delta;

      dst->data[i] = (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
    }
  return 0;
}

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

/* ---- diagnostics ---- */

typedef struct
{
  int    n1, n2;
  int    n_corr;
  double rep;
  int    ex;
  int    pm1;
  int    mode_od;       /* modal orientation-bin difference at correspondences */
  int    ex_comp;       /* exact-bin agreement after removing mode_od */
  int    pm1_comp;      /* +/-1-bin agreement after removing mode_od */
  double hmean;
  int    hmin;
  double tx, ty;
} CorrStats;

typedef struct
{
  int kx, ky, idx;
} KeyIdx;

static int
cmp_key (const void *a, const void *b)
{
  const KeyIdx *ka = a, *kb = b;

  if (ka->kx != kb->kx)
    return ka->kx < kb->kx ? -1 : 1;
  if (ka->ky != kb->ky)
    return ka->ky < kb->ky ? -1 : 1;
  return 0;
}

/*
 * Correspondence statistics. The best translation is the representative offset
 * of the largest 8px bucket (the mode). (The Phase-2.5 harness inherited the
 * numpy reference's `t = offs[argmax(unique-counts)]`, which indexes the
 * flattened offset array by a group index and yields meaningless translations
 * for many pairs; the mode representative is the correct best translation.)
 */
static void
corr_stats (const Egis057eMFeatureSet *fa, const Egis057eMFeatureSet *fb,
            CorrStats *st)
{
  int n1 = fa->n, n2 = fb->n;
  int total = n1 * n2;
  int *ox, *oy, *diff = NULL;
  KeyIdx *keys;
  int best_cnt = 0;
  int bins = egis057e_m_orient_bins ();

  memset (st, 0, sizeof (*st));
  st->n1 = n1;
  st->n2 = n2;
  st->hmin = 512;

  if (n1 < 5 || n2 < 5)
    return;

  ox = malloc (sizeof (int) * total);
  oy = malloc (sizeof (int) * total);
  keys = malloc (sizeof (KeyIdx) * total);
  diff = malloc (sizeof (int) * n1);
  if (!ox || !oy || !keys)
    {
      free (ox);
      free (oy);
      free (keys);
      free (diff);
      return;
    }

  for (int i = 0; i < n1; i++)
    for (int j = 0; j < n2; j++)
      {
        int k = i * n2 + j;

        ox[k] = (int) fa->f[i].x - (int) fb->f[j].x;
        oy[k] = (int) fa->f[i].y - (int) fb->f[j].y;
        keys[k].kx = (int) round_half_even ((double) ox[k] / 4.0);
        keys[k].ky = (int) round_half_even ((double) oy[k] / 4.0);
        keys[k].idx = k;
      }

  qsort (keys, (size_t) total, sizeof (KeyIdx), cmp_key);

  /* mode translation = representative offset of the largest bucket */
  {
    int group_start = 0, best_idx = 0;

    for (int i = 1; i <= total; i++)
      {
        if (i == total ||
            keys[i].kx != keys[i - 1].kx || keys[i].ky != keys[i - 1].ky)
          {
            int cnt = i - group_start;

            if (cnt > best_cnt)
              {
                best_cnt = cnt;
                best_idx = keys[group_start].idx;
              }
            group_start = i;
          }
      }
    st->tx = (double) ox[best_idx];
    st->ty = (double) oy[best_idx];
  }

  for (int i = 0; i < n1; i++)
    {
      for (int j = 0; j < n2; j++)
        {
          double dx = ((double) fa->f[i].x - (double) fb->f[j].x) - st->tx;
          double dy = ((double) fa->f[i].y - (double) fb->f[j].y) - st->ty;

          if (fabs (dx) <= 3.0 && fabs (dy) <= 3.0)
            {
              int h = hamming512 (fa->f[i].desc, fb->f[j].desc);

              st->n_corr++;
              if (fa->f[i].orient == fb->f[j].orient)
                st->ex++;
              if (abs ((int) fa->f[i].orient - (int) fb->f[j].orient) <= 1)
                st->pm1++;
              if (diff)
                {
                  int d = (int) fa->f[i].orient - (int) fb->f[j].orient;

                  d %= bins;
                  if (d < 0)
                    d += bins;
                  diff[st->n_corr - 1] = d;
                }
              st->hmean += h;
              if (h < st->hmin)
                st->hmin = h;
              break;   /* first j per i (numpy firstj dedupe) */
            }
        }
    }

  if (st->n_corr > 0)
    st->hmean /= st->n_corr;
  st->rep = (double) st->n_corr / (double) (n1 > n2 ? n1 : n2);

  /* modal orientation-bin difference (global rotation) and compensated
   * agreement at correspondences */
  if (diff && st->n_corr > 0 && bins > 0 && bins <= 64)
    {
      int hist[64] = { 0 };

      for (int k = 0; k < st->n_corr; k++)
        hist[diff[k]]++;
      for (int b = 1; b < bins; b++)
        if (hist[b] > hist[st->mode_od])
          st->mode_od = b;
      st->ex_comp = hist[st->mode_od];
      for (int b = 0; b < bins; b++)
        {
          int d = b - st->mode_od;

          if (d < 0)
            d = -d;
          if (d > bins / 2)
            d = bins - d;
          if (d <= 1)
            st->pm1_comp += hist[b];
        }
    }

  free (diff);
  free (ox);
  free (oy);
  free (keys);
}

/* cross-NN (each A feature to nearest B) and all-pairs Hamming means */
static void
hamming_means (const Egis057eMFeatureSet *fa, const Egis057eMFeatureSet *fb,
               double *cross_nn, double *all_pairs)
{
  double nnsum = 0.0, sumall = 0.0;
  int npairs = 0;

  *cross_nn = 0.0;
  *all_pairs = 0.0;
  if (fa->n == 0 || fb->n == 0)
    return;

  for (int i = 0; i < fa->n; i++)
    {
      int best = 512;

      for (int j = 0; j < fb->n; j++)
        {
          int h = hamming512 (fa->f[i].desc, fb->f[j].desc);

          if (h < best)
            best = h;
          sumall += h;
          npairs++;
        }
      nnsum += best;
    }
  *cross_nn = nnsum / fa->n;
  *all_pairs = npairs > 0 ? sumall / npairs : 0.0;
}

/* ---- modes ---- */

static int
extract_one (const char *path, const PgmImage *bg, Egis057eMFeatureSet *out,
             double *margin)
{
  PgmImage img = { 0 };

  if (pgm_load (path, &img) != 0)
    return -1;
  if (bg)
    {
      if (bg->width != img.width || bg->height != img.height)
        {
          fprintf (stderr, "error: bg is %dx%d but %s is %dx%d\n",
                   bg->width, bg->height, path, img.width, img.height);
          pgm_free (&img);
          return -1;
        }
      pgm_subtract_bg (&img, bg);
    }

  egis057e_m_extract_ex (img.data, img.width, img.height, out, margin);
  pgm_free (&img);
  return 0;
}

static int
cmd_features (const char *path, const PgmImage *bg)
{
  Egis057eMFeatureSet set = { 0 };

  if (extract_one (path, bg, &set, NULL) != 0)
    return EXIT_FAILURE;

  printf ("features=%d\n", set.n);
  for (int i = 0; i < set.n; i++)
    {
      printf ("%u %u %u ", set.f[i].x, set.f[i].y, set.f[i].orient);
      for (int b = 0; b < EGIS057E_M_DESC_BYTES; b++)
        printf ("%02x", set.f[i].desc[b]);
      printf ("\n");
    }
  egis057e_m_feature_set_clear (&set);
  return EXIT_SUCCESS;
}

static int
cmd_score (const char *gallery_path, const char *probe_path, const PgmImage *bg)
{
  Egis057eMFeatureSet gallery = { 0 }, probe = { 0 };
  int votes = 0;
  int score;

  if (extract_one (gallery_path, bg, &gallery, NULL) != 0)
    return EXIT_FAILURE;
  if (extract_one (probe_path, bg, &probe, NULL) != 0)
    return EXIT_FAILURE;

  score = egis057e_m_score (&probe, &gallery, &votes);
  printf ("features=%d/%d votes=%d score=%d\n",
          gallery.n, probe.n, votes, score);
  egis057e_m_feature_set_clear (&gallery);
  egis057e_m_feature_set_clear (&probe);
  return EXIT_SUCCESS;
}

static int
cmd_diagnose (const char *path_a, const char *path_b, const PgmImage *bg)
{
  Egis057eMFeatureSet fa = { 0 }, fb = { 0 };
  PgmImage A = { 0 }, B = { 0 };
  CorrStats cs;
  double margin_a, margin_b, cross_nn, all_pairs;
  int b_votes = 0, b_score;
  int v_plus1 = 0, v_minus1 = 0, v_up = 0, v_dn = 0;
  int s_plus1, s_minus1, s_up, s_dn;
  int stable;

  if (pgm_load (path_a, &A) != 0)
    return EXIT_FAILURE;
  if (pgm_load (path_b, &B) != 0)
    {
      pgm_free (&A);
      return EXIT_FAILURE;
    }

  if (bg)
    {
      if (bg->width != A.width || bg->height != A.height ||
          bg->width != B.width || bg->height != B.height)
        {
          fprintf (stderr, "error: bg dimensions do not match the pair\n");
          pgm_free (&A);
          pgm_free (&B);
          return EXIT_FAILURE;
        }
      pgm_subtract_bg (&A, bg);
      pgm_subtract_bg (&B, bg);
    }

  egis057e_m_extract_ex (A.data, A.width, A.height, &fa, &margin_a);
  egis057e_m_extract_ex (B.data, B.width, B.height, &fb, &margin_b);

  corr_stats (&fa, &fb, &cs);
  hamming_means (&fa, &fb, &cross_nn, &all_pairs);
  b_score = egis057e_m_score (&fb, &fa, &b_votes);

  printf ("Pair: %s x %s\n", path_a, path_b);
  printf ("  bg: %s\n", bg ? "subtracted" : "none");
  printf ("  features: A=%d B=%d\n", fa.n, fb.n);
  printf ("  IP correspondences (<=3px, t=(%.0f,%.0f)): %d  repeatability=%.2f\n",
          cs.tx, cs.ty, cs.n_corr, cs.rep);
  printf ("  orientation margin (mean 2nd/best histogram bin): A=%.3f B=%.3f\n",
          margin_a, margin_b);
  printf ("  orientation agreement at correspondences: exact=%d/%d pm1=%d/%d | mode_rot=%d comp_exact=%d/%d comp_pm1=%d/%d\n",
          cs.ex, cs.n_corr, cs.pm1, cs.n_corr, cs.mode_od,
          cs.ex_comp, cs.n_corr, cs.pm1_comp, cs.n_corr);
  printf ("  descriptor hamming: true-corr mean=%.1f min=%d | cross-NN mean=%.1f | all-pairs mean=%.1f\n",
          cs.hmean, cs.n_corr ? cs.hmin : 512, cross_nn, all_pairs);
  {
    int geom = 0;

    for (int i = 0; i < fa.n; i++)
      {
        int best = 512, bj = -1;

        for (int j = 0; j < fb.n; j++)
          {
            int h = hamming512 (fa.f[i].desc, fb.f[j].desc);

            if (h < best)
              {
                best = h;
                bj = j;
              }
          }
        if (bj >= 0)
          {
            double dx = (double) fa.f[i].x - (double) fb.f[bj].x - cs.tx;
            double dy = (double) fa.f[i].y - (double) fb.f[bj].y - cs.ty;

            if (fabs (dx) <= 6.0 && fabs (dy) <= 6.0)
              geom++;
          }
      }
    printf ("  descriptor-NN geometric consistency (<=6px): %d/%d\n", geom, fa.n);
  }
  printf ("  verdict: votes=%d score=%d\n", b_votes, b_score);

  {
    PgmImage a1 = { 0 }, b1 = { 0 }, am = { 0 }, bm = { 0 };
    PgmImage au = { 0 }, bu = { 0 }, ad = { 0 }, bd = { 0 };
    Egis057eMFeatureSet f = { 0 };

    pgm_dither (&A, 1, &a1);
    pgm_dither (&B, 1, &b1);
    egis057e_m_extract (a1.data, a1.width, a1.height, &f);
    egis057e_m_extract (b1.data, b1.width, b1.height, &fb);
    s_plus1 = egis057e_m_score (&fb, &f, &v_plus1);

    pgm_dither (&A, -1, &am);
    pgm_dither (&B, -1, &bm);
    egis057e_m_extract (am.data, am.width, am.height, &f);
    egis057e_m_extract (bm.data, bm.width, bm.height, &fb);
    s_minus1 = egis057e_m_score (&fb, &f, &v_minus1);

    pgm_resize_bilinear (&A, 1.05, &au);
    pgm_resize_bilinear (&B, 1.05, &bu);
    egis057e_m_extract (au.data, au.width, au.height, &f);
    egis057e_m_extract (bu.data, bu.width, bu.height, &fb);
    s_up = egis057e_m_score (&fb, &f, &v_up);

    pgm_resize_bilinear (&A, 0.95, &ad);
    pgm_resize_bilinear (&B, 0.95, &bd);
    egis057e_m_extract (ad.data, ad.width, ad.height, &f);
    egis057e_m_extract (bd.data, bd.width, bd.height, &fb);
    s_dn = egis057e_m_score (&fb, &f, &v_dn);

    pgm_free (&a1);
    pgm_free (&b1);
    pgm_free (&am);
    pgm_free (&bm);
    pgm_free (&au);
    pgm_free (&bu);
    pgm_free (&ad);
    pgm_free (&bd);
    egis057e_m_feature_set_clear (&f);
    egis057e_m_feature_set_clear (&fb);
  }

  stable = (v_plus1 == b_votes && s_plus1 == b_score &&
            v_minus1 == b_votes && s_minus1 == b_score &&
            v_up == b_votes && s_up == b_score &&
            v_dn == b_votes && s_dn == b_score);

  printf ("  perturbation stability:\n");
  printf ("    +1 gray   : votes=%d score=%d\n", v_plus1, s_plus1);
  printf ("    -1 gray   : votes=%d score=%d\n", v_minus1, s_minus1);
  printf ("    scale 1.05: votes=%d score=%d\n", v_up, s_up);
  printf ("    scale 0.95: votes=%d score=%d\n", v_dn, s_dn);
  printf ("    stable: %s\n", stable ? "yes" : "no");

  pgm_free (&A);
  pgm_free (&B);
  egis057e_m_feature_set_clear (&fa);
  egis057e_m_feature_set_clear (&fb);
  return EXIT_SUCCESS;
}

/* ---- end-to-end evaluation ---- */

typedef struct
{
  char path[1024];
  char label[64];
  char group[64];
} EvalFile;

/* Resolve a file label to a finger group via EGIS057E_EVAL_MAP ("a=A,b=A,c=B"). */
static void
map_group (const char *map, const char *label, char *out, size_t outsz)
{
  const char *p = map;
  size_t ll0 = strlen (label);

  if (ll0 >= outsz)
    ll0 = outsz - 1;
  memcpy (out, label, ll0);
  out[ll0] = 0;
  if (!map)
    return;
  while (*p)
    {
      const char *eq = strchr (p, '=');
      const char *end;
      size_t ll, gl;

      if (!eq)
        break;
      end = strchr (p, ',');
      ll = (size_t) (eq - p);
      if (strlen (label) == ll && strncmp (label, p, ll) == 0)
        {
          const char *g = eq + 1;

          gl = end ? (size_t) (end - g) : strlen (g);
          if (gl >= outsz)
            gl = outsz - 1;
          memcpy (out, g, gl);
          out[gl] = 0;
          return;
        }
      if (!end)
        break;
      p = end + 1;
    }
}

static int
cmp_score (const void *a, const void *b)
{
  int x = *(const int *) a, y = *(const int *) b;

  return (x > y) - (x < y);
}

static double
median_sorted (int *a, int n)
{
  if (n <= 0)
    return 0.0;
  return (n & 1) ? (double) a[n / 2]
                 : 0.5 * ((double) a[n / 2 - 1] + (double) a[n / 2]);
}

/* EER and TAR at the FAR=0 operating point; gen/imp must be sorted. */
static void
eer_stats (int *gen, int ng, int *imp, int ni, double *eer, double *tar0)
{
  *eer = 0.5;
  *tar0 = 0.0;
  if (ng <= 0 || ni <= 0)
    return;
  {
    int tmin = gen[0] < imp[0] ? gen[0] : imp[0];
    int tmax = gen[ng - 1] > imp[ni - 1] ? gen[ng - 1] : imp[ni - 1];
    double gap = 1e9, e = 0.5;
    int T0 = imp[ni - 1] + 1, c = 0;

    for (int T = tmin; T <= tmax + 1; T++)
      {
        int fi = 0, fk = 0;
        double far, frr;

        while (fi < ni && imp[fi] < T)
          fi++;
        far = (double) (ni - fi) / (double) ni;
        while (fk < ng && gen[fk] < T)
          fk++;
        frr = (double) fk / (double) ng;
        if (fabs (far - frr) < gap)
          {
            gap = fabs (far - frr);
            e = 0.5 * (far + frr);
          }
      }
    *eer = e;
    for (int k = 0; k < ng; k++)
      if (gen[k] >= T0)
        c++;
    *tar0 = (double) c / ng;
  }
}

static int
cmp_evalfile (const void *a, const void *b)
{
  return strcmp (((const EvalFile *) a)->path, ((const EvalFile *) b)->path);
}

static int
cmd_eval (const char *dir, const PgmImage *bg)
{
  DIR *d = opendir (dir);
  EvalFile files[512];
  int n = 0, npairs, ng = 0, ni = 0;
  struct dirent *ent;
  Egis057eMFeatureSet *feats;
  int *gen, *imp;
  const char *skip = getenv ("EGIS057E_EVAL_SKIP");
  const char *mode = getenv ("EGIS057E_P_ORIENT_MODE");
  const char *map = getenv ("EGIS057E_EVAL_MAP");

  if (!d)
    {
      fprintf (stderr, "error: cannot open dir %s\n", dir);
      return EXIT_FAILURE;
    }
  if (!skip || !*skip)
    skip = "first-000,fingerC-bg";

  while ((ent = readdir (d)) != NULL && n < 512)
    {
      const char *nm = ent->d_name;
      size_t L = strlen (nm);
      char base[512];
      int k = 0;

      if (L < 5 || strcmp (nm + L - 4, ".pgm") != 0)
        continue;
      snprintf (base, sizeof (base), "%s", nm);
      base[strlen (base) - 4] = 0;
      if (strcmp (base, skip) == 0)
        continue;
      {
        int skipped = 0;
        const char *sp = skip;

        while (*sp)
          {
            const char *se = strchr (sp, ',');
            size_t sl = se ? (size_t) (se - sp) : strlen (sp);

            if (strlen (base) == sl && strncmp (base, sp, sl) == 0)
              {
                skipped = 1;
                break;
              }
            if (!se)
              break;
            sp = se + 1;
          }
        if (skipped)
          continue;
      }
      while (nm[k] && nm[k] != '-' && k < 63)
        {
          files[n].label[k] = nm[k];
          k++;
        }
      files[n].label[k] = 0;
      map_group (map, files[n].label, files[n].group, sizeof (files[n].group));
      snprintf (files[n].path, sizeof (files[n].path), "%s/%s", dir, nm);
      n++;
    }
  closedir (d);
  if (n < 2)
    {
      fprintf (stderr, "error: need >=2 pgms in %s\n", dir);
      return EXIT_FAILURE;
    }
  qsort (files, (size_t) n, sizeof (EvalFile), cmp_evalfile);

  feats = calloc ((size_t) n, sizeof (Egis057eMFeatureSet));
  if (!feats)
    return EXIT_FAILURE;
  for (int i = 0; i < n; i++)
    {
      PgmImage im = { 0 };

      if (pgm_load (files[i].path, &im) != 0)
        {
          free (feats);
          return EXIT_FAILURE;
        }
      if (bg)
        pgm_subtract_bg (&im, bg);
      egis057e_m_extract (im.data, im.width, im.height, &feats[i]);
      pgm_free (&im);
    }

  npairs = n * (n - 1) / 2;
  gen = malloc (sizeof (int) * (size_t) (npairs > 0 ? npairs : 1));
  imp = malloc (sizeof (int) * (size_t) (npairs > 0 ? npairs : 1));
  if (!gen || !imp)
    {
      free (gen);
      free (imp);
      free (feats);
      return EXIT_FAILURE;
    }

  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      {
        int votes = 0;
        int s = egis057e_m_score (&feats[j], &feats[i], &votes);

        if (strcmp (files[i].group, files[j].group) == 0)
          gen[ng++] = s;
        else
          imp[ni++] = s;
      }

  printf ("Eval dir=%s orient_mode=%s files=%d  genuine_pairs=%d impostor_pairs=%d\n",
          dir, mode ? mode : "1", n, ng, ni);
  {
    double gmin = 0, gmed, gmax = 0, imin = 0, imed, imax = 0;

    qsort (gen, (size_t) ng, sizeof (int), cmp_score);
    qsort (imp, (size_t) ni, sizeof (int), cmp_score);
    gmed = median_sorted (gen, ng);
    imed = median_sorted (imp, ni);
    if (ng)
      {
        gmin = gen[0];
        gmax = gen[ng - 1];
      }
    if (ni)
      {
        imin = imp[0];
        imax = imp[ni - 1];
      }
    printf ("  genuine  n=%d min=%.0f median=%.0f max=%.0f\n", ng, gmin, gmed, gmax);
    printf ("  impostor n=%d min=%.0f median=%.0f max=%.0f\n", ni, imin, imed, imax);
    if (ng && ni)
      printf ("  impostor_max - genuine_min = %.0f (negative => clean separation)\n",
              imax - gmin);

    if (ng && ni)
      {
        double eer, tar0;

        eer_stats (gen, ng, imp, ni, &eer, &tar0);
        printf ("  EER=%.3f ; FAR=0 TAR=%.3f\n", eer, tar0);
      }
  }

  /* per-finger EER (genuine within each finger group vs the global impostors) */
  {
    char groups[64][64];
    int ngrp = 0;

    for (int i = 0; i < n; i++)
      {
        int found = 0;

        for (int g = 0; g < ngrp; g++)
          if (strcmp (groups[g], files[i].group) == 0)
            {
              found = 1;
              break;
            }
        if (!found && ngrp < 64)
          {
            snprintf (groups[ngrp], 64, "%s", files[i].group);
            ngrp++;
          }
      }
    for (int g = 0; g < ngrp; g++)
      {
        int *gs = malloc (sizeof (int) * (size_t) (npairs > 0 ? npairs : 1));
        int ngs = 0;
        double eer, tar0, mn = 0, md = 0, mx = 0;

        if (!gs)
          continue;
        for (int i = 0; i < n; i++)
          for (int j = i + 1; j < n; j++)
            if (strcmp (files[i].group, groups[g]) == 0 &&
                strcmp (files[j].group, groups[g]) == 0)
              {
                int v = 0;

                gs[ngs++] = egis057e_m_score (&feats[j], &feats[i], &v);
              }
        qsort (gs, (size_t) ngs, sizeof (int), cmp_score);
        if (ngs)
          {
            mn = gs[0];
            mx = gs[ngs - 1];
            md = median_sorted (gs, ngs);
          }
        eer_stats (gs, ngs, imp, ni, &eer, &tar0);
        printf ("  finger %-8s genuine n=%d min=%.0f median=%.0f max=%.0f  EER=%.3f TAR@FAR0=%.3f\n",
                groups[g], ngs, mn, md, mx, eer, tar0);
        free (gs);
      }
  }

  /* ---- template-based eval (mirrors the driver: enroll first K frames) ---- */
  {
    char groups[64][64];
    int ngrp = 0;
    const char *ke = getenv ("EGIS057E_EVAL_K");
    int K = (ke && *ke) ? atoi (ke) : 6;

    if (K < 1)
      K = 1;
    for (int i = 0; i < n; i++)
      {
        int found = 0;

        for (int g = 0; g < ngrp; g++)
          if (strcmp (groups[g], files[i].group) == 0)
            {
              found = 1;
              break;
            }
        if (!found && ngrp < 64)
          {
            snprintf (groups[ngrp], 64, "%s", files[i].group);
            ngrp++;
          }
      }

    printf ("Template eval (enroll first K=%d; rule best>=%d && >=%d frames>=%d):\n",
            K, EGIS057E_M_MATCH_THRESHOLD, EGIS057E_M_AGREE_FRAMES,
            EGIS057E_M_AGREE_SCORE);

    for (int g = 0; g < ngrp; g++)
      {
        int *tmpl = malloc (sizeof (int) * n);
        int *gs = malloc (sizeof (int) * n);
        int *is = malloc (sizeof (int) * n);
        int *tsc = malloc (sizeof (int) * (size_t) n * (size_t) K);
        int nt = 0, ngx = 0, nix = 0;

        if (!tmpl || !gs || !is || !tsc)
          {
            free (tmpl);
            free (gs);
            free (is);
            free (tsc);
            continue;
          }
        for (int i = 0; i < n; i++)
          if (strcmp (files[i].group, groups[g]) == 0 && nt < K)
            tmpl[nt++] = i;

        for (int p = 0; p < n; p++)
          {
            int is_tmpl = 0, best = 0;

            for (int t = 0; t < nt; t++)
              if (tmpl[t] == p)
                is_tmpl = 1;
            if (is_tmpl)
              continue;
            for (int t = 0; t < nt; t++)
              {
                int v = 0;
                int s = egis057e_m_score (&feats[p], &feats[tmpl[t]], &v);

                tsc[(size_t) p * K + t] = s;
                if (s > best)
                  best = s;
              }
            if (strcmp (files[p].group, groups[g]) == 0)
              gs[ngx++] = best;
            else
              is[nix++] = best;
          }
        qsort (gs, (size_t) ngx, sizeof (int), cmp_score);
        qsort (is, (size_t) nix, sizeof (int), cmp_score);
        {
          double eer, tar0, gmin = 0, gmed = 0, gmax = 0;
          double imin = 0, imed = 0, imax = 0;

          if (ngx)
            {
              gmin = gs[0];
              gmax = gs[ngx - 1];
              gmed = median_sorted (gs, ngx);
            }
          if (nix)
            {
              imin = is[0];
              imax = is[nix - 1];
              imed = median_sorted (is, nix);
            }
          eer_stats (gs, ngx, is, nix, &eer, &tar0);
          printf ("  enroll %-8s best genuine n=%d %.0f/%.0f/%.0f | impostor n=%d %.0f/%.0f/%.0f | EER=%.3f\n",
                  groups[g], ngx, gmin, gmed, gmax, nix, imin, imed, imax, eer);
          printf ("    genuine:");
          for (int k = 0; k < ngx; k++)
            printf (" %d", gs[k]);
          printf ("\n    impostor:");
          for (int k = 0; k < nix; k++)
            printf (" %d", is[k]);
          printf ("\n");

          if (nix && ngx)
            {
              int T0 = (int) (imax * 1.15);
              int cg = 0, ci = 0;

              for (int k = 0; k < ngx; k++)
                if (gs[k] >= T0)
                  cg++;
              for (int k = 0; k < nix; k++)
                if (is[k] >= T0)
                  ci++;
              printf ("    FAR=0 point T=%d (impostor_max=%.0f +15%%): TAR=%.3f FRR=%.3f FAR=%.3f\n",
                      T0, imax, (double) cg / ngx, 1.0 - (double) cg / ngx,
                      (double) ci / nix);

              for (int ai = 0; ai < 3; ai++)
                {
                  int A0 = ai == 0 ? T0 : (ai == 1 ? T0 / 2 : T0 * 3 / 4);
                  int gpass = 0, ipass = 0;

                  if (A0 < 1)
                    A0 = 1;
                  for (int p = 0; p < n; p++)
                    {
                      int is_tmpl = 0, best = 0, agree = 0, verdict;

                      for (int t = 0; t < nt; t++)
                        if (tmpl[t] == p)
                          is_tmpl = 1;
                      if (is_tmpl)
                        continue;
                      for (int t = 0; t < nt; t++)
                        {
                          if (tsc[(size_t) p * K + t] > best)
                            best = tsc[(size_t) p * K + t];
                          if (tsc[(size_t) p * K + t] >= A0)
                            agree++;
                        }
                      verdict = best >= T0 && agree >= EGIS057E_M_AGREE_FRAMES;
                      if (strcmp (files[p].group, groups[g]) == 0)
                        {
                          if (verdict)
                            gpass++;
                        }
                      else if (verdict)
                        ipass++;
                    }
                  printf ("    rule MATCH=%d AGREE=%d: TAR=%.3f FAR=%.3f\n",
                          T0, A0, ngx ? (double) gpass / ngx : 0.0,
                          nix ? (double) ipass / nix : 0.0);
                }
              {
                int gpass = 0, ipass = 0;

                for (int p = 0; p < n; p++)
                  {
                    int is_tmpl = 0, best = 0, agree = 0, verdict;

                    for (int t = 0; t < nt; t++)
                      if (tmpl[t] == p)
                        is_tmpl = 1;
                    if (is_tmpl)
                      continue;
                    for (int t = 0; t < nt; t++)
                      {
                        if (tsc[(size_t) p * K + t] > best)
                          best = tsc[(size_t) p * K + t];
                        if (tsc[(size_t) p * K + t] >= EGIS057E_M_AGREE_SCORE)
                          agree++;
                      }
                    verdict = best >= EGIS057E_M_MATCH_THRESHOLD &&
                              agree >= EGIS057E_M_AGREE_FRAMES;
                    if (strcmp (files[p].group, groups[g]) == 0)
                      {
                        if (verdict)
                          gpass++;
                      }
                    else if (verdict)
                      ipass++;
                  }
                printf ("    rule HEADER MATCH=%d AGREE=%d: TAR=%.3f FAR=%.3f\n",
                        EGIS057E_M_MATCH_THRESHOLD, EGIS057E_M_AGREE_SCORE,
                        ngx ? (double) gpass / ngx : 0.0,
                        nix ? (double) ipass / nix : 0.0);
              }

              printf ("    ROC (T: FAR FRR):\n");
              {
                int step = gmax > 0 ? (int) (gmax / 10) : 1;

                if (step < 1)
                  step = 1;
                for (int T = 0; T <= (int) gmax + step; T += step)
                  {
                    int cg = 0, ci = 0;

                    for (int k = 0; k < ngx; k++)
                      if (gs[k] >= T)
                        cg++;
                    for (int k = 0; k < nix; k++)
                      if (is[k] >= T)
                        ci++;
                    printf ("      %5d  FAR=%.3f FRR=%.3f\n", T,
                            (double) ci / nix, 1.0 - (double) cg / ngx);
                  }
              }
            }
        }
        free (tmpl);
        free (gs);
        free (is);
        free (tsc);
      }
  }

  for (int i = 0; i < n; i++)
    egis057e_m_feature_set_clear (&feats[i]);
  free (gen);
  free (imp);
  free (feats);
  return EXIT_SUCCESS;
}

static void
usage (const char *prog)
{
  fprintf (stderr,
           "usage: %s [--bg <bg.pgm>] features <image.pgm>\n"
           "       %s [--bg <bg.pgm>] score <gallery.pgm> <probe.pgm>\n"
           "       %s [--bg <bg.pgm>] diagnose <imageA.pgm> <imageB.pgm>\n"
           "       %s eval <dir>\n",
           prog, prog, prog, prog);
}

int
main (int argc, char *argv[])
{
  const char *bg_path = NULL;
  const char *pos[4];
  int npos = 0;
  PgmImage bg = { 0 };
  int ret;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--bg") == 0)
        {
          if (i + 1 >= argc)
            {
              fprintf (stderr, "error: --bg needs a path\n");
              return EXIT_FAILURE;
            }
          bg_path = argv[++i];
          continue;
        }
      if (strncmp (argv[i], "--", 2) == 0)
        {
          fprintf (stderr, "error: unknown option %s\n", argv[i]);
          return EXIT_FAILURE;
        }
      if (npos < 4)
        pos[npos++] = argv[i];
    }

  if (bg_path && pgm_load (bg_path, &bg) != 0)
    return EXIT_FAILURE;

  if (npos >= 2 && strcmp (pos[0], "features") == 0)
    ret = cmd_features (pos[1], bg_path ? &bg : NULL);
  else if (npos >= 3 && strcmp (pos[0], "score") == 0)
    ret = cmd_score (pos[1], pos[2], bg_path ? &bg : NULL);
  else if (npos >= 3 && strcmp (pos[0], "diagnose") == 0)
    ret = cmd_diagnose (pos[1], pos[2], bg_path ? &bg : NULL);
  else if (npos >= 2 && strcmp (pos[0], "eval") == 0)
    ret = cmd_eval (pos[1], bg_path ? &bg : NULL);
  else
    {
      usage (argv[0]);
      ret = EXIT_FAILURE;
    }

  pgm_free (&bg);
  return ret;
}
