/*
 * egis057e_enroll - friendly enrollment / verification CLI for the EH57E
 *
 * SPDX-License-Identifier: 0BSD
 * Copyright (C) 2026 EH57E Linux driver contributors
 *
 * Uses the public libfprint FpDevice API plus its live signals/progress
 * callbacks to give the operator simple ASCII feedback, in Italian:
 *
 *   ./egis057e_enroll enroll  [finger-index]
 *   ./egis057e_enroll verify  [finger-index]
 *   ./egis057e_enroll monitor [finger-index]
 *
 * finger-index is 0..9 (left thumb..right little); default 1 (left index).
 *
 * "monitor" opens the device once and runs verify in a loop, printing
 * MATCH / NO MATCH for each press, until Ctrl+C. After a verdict the driver
 * waits for the finger to be lifted before completing, so the next attempt
 * starts from a lift (no phantom re-read of the same press). It avoids the
 * per-attempt open/close and the arm delay. Hard errors (driver watchdog /
 * wedge) stop the loop with a message; Ctrl+C releases the device via the
 * SIGINT/SIGTERM path. "verify" is one-shot and exits right after the verdict.
 *
 * Flow per attempt:
 *   NEEDED  -> "Appoggia il dito..."
 *   PRESENT -> "dito PRESENTE (tieni fermo)"
 *   verdict -> "MATCH" / "NO MATCH" then "Solleva il dito..."
 *   lift    -> "Appoggia il dito..." for the next attempt
 *
 * All messages are prefixed with a local HH:MM:SS.mmm timestamp.
 *
 * Enrollment progress:
 *   accepted stage X -> "[X/5] acquisito OK"
 *   retry            -> "[X/5] non valido: <reason> - riprova"
 *
 * libfprint debug is silenced by default (G_MESSAGES_DEBUG="") but the operator
 * can enable it: G_MESSAGES_DEBUG=all or =libfprint-egis057e.
 *
 * Recovery ladder (if the sensor ever stays "Resource busy" / wedged):
 *   1. Ctrl+C here: SIGINT/SIGTERM cancel the action and release the device;
 *      a 3 s safety timer force-closes if the action does not finalize.
 *   2. bash ~/eh57e-project/scripts/eh57e-cleanup.sh
 *        kills any leftover egis057e_enroll processes and resets the sensor.
 *   3. bash ~/eh57e-project/scripts/reset-sensor.sh
 *        issues USBDEVFS_RESET on the node if it is still stuck.
 *   (The driver never resets the USB device itself: that worsens convergence.)
 */

#define FP_COMPONENT "egis057e-cli"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>
#include <libfprint/fprint.h>

#include "storage.h"

typedef struct
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  guint         sigint_handler;
  guint         sigterm_handler;
  guint         force_quit_id;
  guint         exit_timeout_id;
  gboolean      interrupted;
  gboolean      awaiting_press;
  FpDevice     *dev;
  FpFinger      finger;
  gboolean      enroll;
  gboolean      monitor;
  int           ret_value;
} CliData;

#define STORAGE_FILE "test-storage.variant"

/* Create an empty storage file if absent so the examples' storage loader does
 * not warn on the first run. The file must live in the current directory for
 * both enroll and verify. */
static void
ensure_storage (void)
{
  if (!g_file_test (STORAGE_FILE, G_FILE_TEST_EXISTS))
    g_file_set_contents (STORAGE_FILE, "", 0, NULL);
}

static void
cli_log (const char *fmt, ...)
{
  va_list ap;
  g_autofree gchar *msg = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autofree gchar *stamp = NULL;

  /* HH:MM:SS.mmm (GLib's %f/%3f is not available in this build). */
  stamp = g_strdup_printf ("%02d:%02d:%02d.%03d",
                           g_date_time_get_hour (now),
                           g_date_time_get_minute (now),
                           g_date_time_get_second (now),
                           g_date_time_get_microsecond (now) / 1000);

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_print ("[%s] %s", stamp, msg);
}

/* Intercept the driver's per-frame settle-progress log lines (domain
 * "libfprint-egis057e") and render them as a live ASCII bar. All other driver
 * messages are forwarded to the default handler (so G_MESSAGES_DEBUG still
 * controls the normal debug output). A custom g_log handler receives the
 * messages even when G_MESSAGES_DEBUG is unset. */
static void
driver_log (const gchar *domain, GLogLevelFlags level, const gchar *message,
            gpointer user_data)
{
  (void) user_data;

  if (g_str_has_prefix (message, "EGIS057E_SETTLE_START"))
    {
      cli_log ("rilevato, tieniti fermo\n");
      return;
    }
  if (g_str_has_prefix (message, "EGIS057E_SETTLE_DONE"))
    {
      cli_log ("frame acquisito\n");
      return;
    }
  if (g_str_has_prefix (message, "EGIS057E_SETTLE "))
    {
      guint n = 0, m = 0;

      if (sscanf (message, "EGIS057E_SETTLE %u %u", &n, &m) == 2 && m > 0)
        {
          g_autofree gchar *bar = g_malloc (m + 1);

          for (guint i = 0; i < m; i++)
            bar[i] = i < n ? '#' : '-';
          bar[m] = 0;
          cli_log ("[%s] %u/%u\n", bar, n, m);
        }
      return;
    }

  g_log_default_handler (domain, level, message, user_data);
}

static const char *
driver_name (void)
{
  return "egis057e";
}

static FpDevice *
select_device (FpContext *ctx)
{
  GPtrArray *devices = fp_context_get_devices (ctx);
  FpDevice *fallback = NULL;

  if (!devices || devices->len == 0)
    return NULL;
  for (guint i = 0; i < devices->len; i++)
    {
      FpDevice *dev = g_ptr_array_index (devices, i);

      if (g_strcmp0 (fp_device_get_driver (dev), driver_name ()) == 0)
        return dev;
      if (!fallback)
        fallback = dev;
    }
  return fallback;
}

static FpFinger
finger_from_index (int idx)
{
  /* Operator-facing index is 0-based over the 9 fingers:
   * 0=left thumb, 1=left index, ..., 6=right index, 8=right little. */
  if (idx < 0 || idx > 8)
    idx = 1;
  return (FpFinger) (FP_FINGER_FIRST + idx);
}

static int
finger_index (FpFinger f)
{
  return (int) f - (int) FP_FINGER_FIRST;
}

/* ---- live finger status ---- */

static void
on_finger_status (FpDevice *dev, GParamSpec *pspec, gpointer user_data)
{
  CliData *cli = user_data;
  FpFingerStatusFlags st = fp_device_get_finger_status (dev);

  (void) pspec;

  if (st & FP_FINGER_STATUS_PRESENT)
    {
      if (cli->awaiting_press)
        cli_log ("dito PRESENTE (tieni fermo)\n");
    }
  else if (st & FP_FINGER_STATUS_NEEDED)
    {
      /* Only prompt for a press when we are actually waiting for one; after a
       * verdict the operator is being asked to lift, not press. */
      if (!cli->awaiting_press)
        {
          cli->awaiting_press = TRUE;
          cli_log ("Appoggia il dito...\n");
        }
    }
  else
    {
      /* NONE: the driver reports this only when the frame shows no finger. Do
       * not print "ASSENTE" spuriously; after a verdict the lift is expected. */
      cli->awaiting_press = FALSE;
    }
}

/* ---- enrollment ---- */

static const char *
retry_reason (GError *error, gboolean *is_retry)
{
  *is_retry = (error && error->domain == FP_DEVICE_RETRY);
  if (!*is_retry)
    return error ? error->message : NULL;

  switch (error->code)
    {
    case FP_DEVICE_RETRY_CENTER_FINGER:
      return "immagine poco nitida, centra meglio il dito";
    case FP_DEVICE_RETRY_REMOVE_FINGER:
      return "solleva il dito";
    case FP_DEVICE_RETRY_TOO_SHORT:
      return "pressione troppo breve";
    case FP_DEVICE_RETRY_TOO_FAST:
      return "movimento troppo rapido";
    case FP_DEVICE_RETRY_GENERAL:
    default:
      return "stessa posizione o appoggio non valido, sposta il dito";
    }
}

static void
on_enroll_progress (FpDevice *device, gint completed_stages, FpPrint *print,
                    gpointer user_data, GError *error)
{
  CliData *cli = user_data;
  gint total = fp_device_get_nr_enroll_stages (device);

  (void) print;
  if (error)
    {
      gboolean is_retry;
      const char *reason = retry_reason (error, &is_retry);

      cli_log ("[%d/%d] non valido: %s - riprova\n",
               completed_stages, total, reason);
      return;
    }

  cli_log ("[%d/%d] acquisito OK\n", completed_stages, total);
  (void) cli;
}

static gboolean
exit_force_cb (gpointer user_data)
{
  CliData *cli = user_data;

  cli->exit_timeout_id = 0;
  /* The close did not complete in time; quit anyway so the tool exits right
   * after the verdict (the process teardown releases the USB interface). */
  g_main_loop_quit (cli->loop);
  return G_SOURCE_REMOVE;
}

static void
on_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  CliData *cli = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_close_finish (dev, res, &error))
    cli_log ("Nota: chiusura dispositivo: %s\n", error->message);
  g_main_loop_quit (cli->loop);
}

static void
finish_and_close (FpDevice *dev, CliData *cli)
{
  if (fp_device_is_open (dev))
    {
      fp_device_close (dev, NULL, (GAsyncReadyCallback) on_closed, cli);
      /* Safety net: never hang after a verdict waiting on the close callback. */
      if (cli->exit_timeout_id == 0)
        cli->exit_timeout_id = g_timeout_add (1500, exit_force_cb, cli);
    }
  else
    g_main_loop_quit (cli->loop);
}

static void
on_enroll_complete (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  CliData *cli = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;

  print = fp_device_enroll_finish (dev, res, &error);
  if (error)
    {
      cli_log ("Enrollment fallito: %s\n", error->message);
      cli->ret_value = EXIT_FAILURE;
    }
  else
    {
      cli_log ("Enrollment completato OK\n");
      if (print_data_save (print, cli->finger, FALSE) < 0)
        {
          cli_log ("Nota: non sono riuscito a salvare il template su disco\n");
          cli->ret_value = EXIT_FAILURE;
        }
      else
        {
          cli_log ("Template salvato (finger %d).\n", finger_index (cli->finger));
          cli->ret_value = EXIT_SUCCESS;
        }
    }

  finish_and_close (dev, cli);
}

static void
run_enroll (FpDevice *dev, CliData *cli)
{
  FpPrint *tmpl;

  cli_log ("Modalita' ENROLL - finger %d.\n", finger_index (cli->finger));
  cli_log ("Servono %d appoggi. Solleva tra un appoggio e l'altro.\n\n",
           fp_device_get_nr_enroll_stages (dev));

  tmpl = print_create_template (dev, cli->finger, FALSE);
  fp_device_enroll (dev, tmpl, cli->cancellable,
                    on_enroll_progress, cli, NULL,
                    (GAsyncReadyCallback) on_enroll_complete, cli);
}

/* ---- verification ---- */

static void run_verify_iteration (CliData *cli);

static void
on_match (FpDevice *dev, FpPrint *match, FpPrint *print,
          gpointer user_data, GError *error)
{
  CliData *cli = user_data;

  (void) dev;
  (void) print;
  if (error)
    {
      gboolean is_retry;
      const char *reason = retry_reason (error, &is_retry);

      cli_log ("Scansione non valida: %s - riprova\n", reason);
      return;
    }
  cli_log (match ? "MATCH\n" : "NO MATCH\n");
  /* The driver now waits for the finger to lift before completing, so the
   * next attempt starts from a lift (no phantom re-read of the same press). */
  cli->awaiting_press = FALSE;
  cli_log ("Solleva il dito...\n");
}

static void
on_verify_complete (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  CliData *cli = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;
  gboolean match = FALSE;

  if (!fp_device_verify_finish (dev, res, &match, &print, &error))
    {
      gboolean cancelled = g_cancellable_is_cancelled (cli->cancellable);

      if (cli->monitor)
        {
          if (!cancelled)
            {
              if (g_str_has_prefix (error->message, "EGIS057E_NO_PRESS"))
                {
                  /* No press within the window: keep monitoring, do not exit. */
                  cli_log ("Nessun appoggio entro il timeout; resto in attesa (Ctrl+C per uscire).\n");
                  run_verify_iteration (cli);
                  return;
                }
              cli_log ("Verifica non riuscita: %s\n", error->message);
              cli_log ("Esco dal monitor. Se il sensore resta bloccato: bash ~/eh57e-project/scripts/eh57e-cleanup.sh\n");
            }
          cli->ret_value = EXIT_FAILURE;
          finish_and_close (dev, cli);
          return;
        }

      if (g_str_has_prefix (error->message, "EGIS057E_NO_PRESS"))
        cli_log ("Nessun appoggio entro il timeout.\n");
      else
        cli_log ("Verify interrotto: %s\n", error->message);
      cli->ret_value = EXIT_FAILURE;
      finish_and_close (dev, cli);
      return;
    }

  if (cli->monitor)
    {
      cli->ret_value = EXIT_SUCCESS;
      /* Stop on Ctrl+C, otherwise re-arm and wait for the next press. The
       * device stays open, so there is no per-attempt open/close or arm delay. */
      if (g_cancellable_is_cancelled (cli->cancellable))
        {
          finish_and_close (dev, cli);
          return;
        }
      run_verify_iteration (cli);
      return;
    }

  cli->ret_value = match ? EXIT_SUCCESS : EXIT_FAILURE;
  finish_and_close (dev, cli);
}

static void
run_verify_iteration (CliData *cli)
{
  g_autoptr(FpPrint) verify_print = NULL;

  verify_print = print_data_load (cli->dev, cli->finger);
  if (!verify_print)
    {
      cli_log ("Nessun template salvato per questo dito: esegui prima `enroll`.\n");
      cli->ret_value = EXIT_FAILURE;
      finish_and_close (cli->dev, cli);
      return;
    }

  /* Own the press prompt deterministically so it is never lost (the driver's
   * NEEDED report then finds awaiting_press==TRUE and prints nothing). */
  cli->awaiting_press = TRUE;
  cli_log ("Appoggia il dito...\n");

  fp_device_verify (cli->dev, verify_print, cli->cancellable,
                    on_match, cli, NULL,
                    (GAsyncReadyCallback) on_verify_complete, cli);
}

static void
run_verify (FpDevice *dev, CliData *cli)
{
  (void) dev;
  cli_log ("Modalita' %s - finger %d.\n",
           cli->monitor ? "MONITOR (loop)" : "VERIFY",
           finger_index (cli->finger));
  if (cli->monitor)
    cli_log ("Premi ripetutamente; fra un tentativo e l'altro solleva il dito. Ctrl+C per uscire.\n");
  run_verify_iteration (cli);
}

/* ---- lifecycle ---- */

static void
on_open (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  CliData *cli = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      cli_log ("Impossibile aprire il dispositivo: %s\n", error->message);
      cli->ret_value = EXIT_FAILURE;
      g_main_loop_quit (cli->loop);
      return;
    }

  cli_log ("Dispositivo: %s (%s) [driver %s]\n",
           fp_device_get_device_id (dev), fp_device_get_name (dev),
           fp_device_get_driver (dev));

  if (cli->enroll)
    run_enroll (dev, cli);
  else
    run_verify (dev, cli);
}

static gboolean
force_quit_cb (gpointer user_data)
{
  CliData *cli = user_data;

  cli->force_quit_id = 0;
  cli_log ("chiusura forzata: rilascio il dispositivo.\n");

  /* Best-effort close. If it does not complete, quitting the loop exits the
   * process and libusb releases the interface anyway, so the sensor is not
   * left "Resource busy". */
  if (cli->dev && fp_device_is_open (cli->dev))
    fp_device_close (cli->dev, NULL, NULL, NULL);
  g_main_loop_quit (cli->loop);
  return G_SOURCE_REMOVE;
}

static gboolean
on_signal (gpointer user_data)
{
  CliData *cli = user_data;

  if (cli->interrupted)
    {
      /* A second signal: stop waiting and exit immediately. */
      cli_log ("\nInterrotto di nuovo: esco subito.\n");
      g_main_loop_quit (cli->loop);
      return G_SOURCE_CONTINUE;
    }

  cli->interrupted = TRUE;
  cli_log ("\nInterrotto: annullo l'azione e rilascio il dispositivo...\n");
  g_cancellable_cancel (cli->cancellable);
  /* Safety net: if the action does not finalize/close promptly, force it. */
  if (cli->force_quit_id == 0)
    cli->force_quit_id = g_timeout_add (3000, force_quit_cb, cli);
  return G_SOURCE_CONTINUE;
}

static void
usage (const char *prog)
{
  fprintf (stderr,
           "usage: %s enroll|verify|monitor [finger-index 0..8, default 1]\n"
           "  finger-index: 0=left thumb, 1=left index, 6=right index, 8=right little\n"
           "  enroll  : acquire a template (N presses)\n"
           "  verify  : one-shot verification (exits after the verdict)\n"
           "  monitor : keep the device open and verify repeatedly until Ctrl+C\n",
           prog);
}

int
main (int argc, char *argv[])
{
  g_autoptr(FpContext) ctx = NULL;
  CliData *cli;
  FpDevice *dev;
  int ret;

  /* Only default to silence if the operator did not set G_MESSAGES_DEBUG;
   * do not overwrite, so G_MESSAGES_DEBUG=all / =libfprint-egis057e works. */
  setenv ("G_MESSAGES_DEBUG", "", 0);

  if (argc < 2 ||
      (strcmp (argv[1], "enroll") != 0 &&
       strcmp (argv[1], "verify") != 0 &&
       strcmp (argv[1], "monitor") != 0))
    {
      usage (argv[0]);
      return EXIT_FAILURE;
    }

  cli = g_new0 (CliData, 1);
  cli->enroll = strcmp (argv[1], "enroll") == 0;
  cli->monitor = strcmp (argv[1], "monitor") == 0;
  cli->finger = finger_from_index (argc > 2 ? atoi (argv[2]) : 1);
  cli->loop = g_main_loop_new (NULL, FALSE);
  cli->cancellable = g_cancellable_new ();
  cli->sigint_handler = g_unix_signal_add_full (G_PRIORITY_HIGH, SIGINT,
                                                on_signal, cli, NULL);
  cli->sigterm_handler = g_unix_signal_add_full (G_PRIORITY_HIGH, SIGTERM,
                                                 on_signal, cli, NULL);
  cli->ret_value = EXIT_FAILURE;

  ctx = fp_context_new ();
  dev = select_device (ctx);
  if (!dev)
    {
      cli_log ("Nessun dispositivo EH57E (driver egis057e) trovato.\n");
      g_main_loop_unref (cli->loop);
      g_object_unref (cli->cancellable);
      g_free (cli);
      return EXIT_FAILURE;
    }
  cli->dev = dev;

  /* Live finger-status feedback. */
  g_signal_connect (dev, "notify::finger-status",
                    G_CALLBACK (on_finger_status), cli);

  /* Live settle-progress bar from the driver's log channel. */
  g_log_set_handler ("libfprint-egis057e", G_LOG_LEVEL_MASK,
                     driver_log, NULL);
  {
    g_autofree gchar *sp = g_canonicalize_filename (STORAGE_FILE, NULL);

    cli_log ("Storage template: %s\n", sp);
    ensure_storage ();
  }

  fp_device_open (dev, cli->cancellable, (GAsyncReadyCallback) on_open, cli);
  g_main_loop_run (cli->loop);

  ret = cli->ret_value;
  if (cli->sigint_handler)
    g_source_remove (cli->sigint_handler);
  if (cli->sigterm_handler)
    g_source_remove (cli->sigterm_handler);
  if (cli->force_quit_id)
    g_source_remove (cli->force_quit_id);
  if (cli->exit_timeout_id)
    g_source_remove (cli->exit_timeout_id);
  g_main_loop_unref (cli->loop);
  g_object_unref (cli->cancellable);
  g_free (cli);
  return ret;
}
