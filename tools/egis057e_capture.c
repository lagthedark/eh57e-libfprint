/*
 * egis057e_capture - standalone EH57E frame capture tool
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Opens the EgisTec EH57E (1c7a:057e) through the public libfprint/FpDevice
 * API built in this tree and captures one or more live frames, dumping each
 * 3990-byte native frame to a binary PGM (P5 70 57 255).
 *
 * It also saves the fresh per-activation 0x72 calibration frame (empty-sensor
 * background) as <prefix>-bg.pgm: the driver writes it when the
 * EGIS057E_BG_DUMP_PATH environment variable is set, which this tool sets.
 *
 * Usage:
 *   ./egis057e_capture [--subtract] [output-prefix] [frame-count]
 *
 *   --subtract            EH575-style background subtraction before dumping:
 *                         out = (v > bg + 2) ? (v - bg) : 0
 *                         (also enabled by EGIS057E_CAPTURE_SUBTRACT=1)
 *
 * The tool is intentionally independent of the private driver API; it only
 * links against the freshly built libfprint-2.so.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <libfprint/fprint.h>

#define EGIS057E_FRAME_WIDTH  70
#define EGIS057E_FRAME_HEIGHT 57
#define EGIS057E_FRAME_LEN    3990

static FpDevice *
select_egis057e (FpContext *ctx)
{
  GPtrArray *devices = fp_context_get_devices (ctx);
  FpDevice *fallback = NULL;

  if (!devices || devices->len == 0)
    return NULL;

  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *dev = g_ptr_array_index (devices, i);
      const gchar *driver = fp_device_get_driver (dev);

      if (g_strcmp0 (driver, "egis057e") == 0)
        return dev;

      if (!fallback && fp_device_has_feature (dev, FP_DEVICE_FEATURE_CAPTURE))
        fallback = dev;
    }

  return fallback;
}

static gboolean
write_pgm (const gchar  *path,
           const guchar *data,
           gsize         len,
           GError      **error)
{
  gchar header[64];
  gint header_len;
  FILE *f;

  if (len != EGIS057E_FRAME_LEN)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "unexpected frame length %zu (expected %d)",
                   len, EGIS057E_FRAME_LEN);
      return FALSE;
    }

  f = fopen (path, "wb");
  if (!f)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "failed to create %s: %s", path, g_strerror (errno));
      return FALSE;
    }

  header_len = g_snprintf (header, sizeof (header), "P5 %d %d 255\n",
                           EGIS057E_FRAME_WIDTH, EGIS057E_FRAME_HEIGHT);

  if (fwrite (header, 1, header_len, f) != (gsize) header_len ||
      fwrite (data, 1, len, f) != len)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "failed to write %s: %s", path, g_strerror (errno));
      fclose (f);
      return FALSE;
    }

  fclose (f);
  return TRUE;
}

/* Load a 70x57 binary PGM written by this tool (or the driver bg dump). */
static guchar *
load_pgm_70x57 (const gchar *path)
{
  FILE *f = fopen (path, "rb");
  gchar magic[3] = { 0 };
  gint w, h, mx, c;
  guchar *buf;

  if (!f)
    return NULL;
  if (fscanf (f, "%2s", magic) != 1 || strcmp (magic, "P5") != 0)
    {
      fclose (f);
      return NULL;
    }
  if (fscanf (f, "%d %d %d", &w, &h, &mx) != 3 ||
      w != EGIS057E_FRAME_WIDTH || h != EGIS057E_FRAME_HEIGHT || mx != 255)
    {
      fclose (f);
      return NULL;
    }
  c = fgetc (f);
  if (c == EOF)
    {
      fclose (f);
      return NULL;
    }

  buf = g_malloc (EGIS057E_FRAME_LEN);
  if (fread (buf, 1, EGIS057E_FRAME_LEN, f) != EGIS057E_FRAME_LEN)
    {
      g_free (buf);
      fclose (f);
      return NULL;
    }
  fclose (f);
  return buf;
}

/* EH575-style background subtraction. */
static void
subtract_bg (guchar       *dst,
             const guchar *src,
             const guchar *bg,
             gsize         len)
{
  for (gsize i = 0; i < len; i++)
    {
      gint v = (gint) src[i];
      gint b = (gint) bg[i];

      dst[i] = v > b + 2 ? (guchar) (v - b) : 0;
    }
}

int
main (int argc, char *argv[])
{
  const gchar *prefix = "frame";
  gint count = 1;
  gboolean subtract = FALSE;
  gint positional = 0;
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *bg_path = NULL;
  guchar *bg = NULL;
  gboolean bg_loaded = FALSE;
  FpDevice *dev;

  if (g_strcmp0 (g_getenv ("EGIS057E_CAPTURE_SUBTRACT"), "1") == 0)
    subtract = TRUE;

  for (int i = 1; i < argc; i++)
    {
      if (g_strcmp0 (argv[i], "--subtract") == 0)
        {
          subtract = TRUE;
          continue;
        }
      if (g_str_has_prefix (argv[i], "--"))
        {
          fprintf (stderr, "Unknown option: %s\n", argv[i]);
          return EXIT_FAILURE;
        }
      if (positional == 0)
        prefix = argv[i];
      else if (positional == 1)
        count = atoi (argv[i]);
      positional++;
    }

  if (count < 1)
    count = 1;

  ctx = fp_context_new ();
  dev = select_egis057e (ctx);
  if (!dev)
    {
      fprintf (stderr, "No EH57E (driver egis057e) capture device found.\n");
      return EXIT_FAILURE;
    }

  g_print ("Selected %s (%s), driver %s\n",
           fp_device_get_device_id (dev),
           fp_device_get_name (dev),
           fp_device_get_driver (dev));

  /* Ask the driver to dump the fresh per-activation 0x72 calibration frame. */
  bg_path = g_strdup_printf ("%s-bg.pgm", prefix);
  g_setenv ("EGIS057E_BG_DUMP_PATH", bg_path, TRUE);

  if (!fp_device_open_sync (dev, NULL, &error))
    {
      fprintf (stderr, "Failed to open device: %s\n", error->message);
      return EXIT_FAILURE;
    }

  g_print ("Opened device; capturing %d frame(s)%s. Press the sensor when prompted.\n",
           count, subtract ? " (background-subtracted)" : "");

  for (gint i = 0; i < count; i++)
    {
      g_autoptr(FpImage) image = NULL;
      g_autofree gchar *path = NULL;
      const guchar *data;
      gsize len = 0;

      g_clear_error (&error);
      image = fp_device_capture_sync (dev, TRUE, NULL, &error);
      if (!image)
        {
          fprintf (stderr, "Capture %d failed: %s\n", i, error->message);
          break;
        }

      data = fp_image_get_data (image, &len);
      path = g_strdup_printf ("%s-%03d.pgm", prefix, i);

      /* The driver writes the background during the first activation's init. */
      if (!bg_loaded)
        {
          bg = load_pgm_70x57 (bg_path);
          bg_loaded = TRUE;
          if (bg)
            g_print ("Background saved: %s\n", bg_path);
          else if (subtract)
            fprintf (stderr, "Warning: %s not readable; dumping raw frames\n",
                     bg_path);
        }

      g_clear_error (&error);
      if (subtract && bg)
        {
          g_autofree guchar *sub = g_malloc (len);

          subtract_bg (sub, data, bg, len);
          if (!write_pgm (path, sub, len, &error))
            fprintf (stderr, "Frame %d: %s\n", i, error->message);
          else
            g_print ("Frame %d: wrote %zu bytes to %s (bg-subtracted)\n",
                     i, len, path);
        }
      else if (!write_pgm (path, data, len, &error))
        {
          fprintf (stderr, "Frame %d: %s\n", i, error->message);
        }
      else
        {
          g_print ("Frame %d: wrote %zu bytes to %s (%ux%u)\n",
                   i, len, path,
                   fp_image_get_width (image),
                   fp_image_get_height (image));
        }
    }

  g_free (bg);
  fp_device_close_sync (dev, NULL, NULL);

  return EXIT_SUCCESS;
}
