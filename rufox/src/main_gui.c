/*
 * rufox — GUI entry point
 *
 * Rufus is a single-threaded Win32 dialog; its long operations run on worker
 * threads created with CreateThread and report back through PostMessage. The
 * GTK equivalent is a GTask plus signals, which engine.c wraps around the
 * blocking RfxJob so that the main loop stays responsive while writing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/rufox-gui.h"

#include <stdio.h>
#include <locale.h>

static char *opt_iso = NULL;
static char *opt_device = NULL;
static char *opt_fs = NULL;
static gboolean opt_verbose = FALSE;
static gboolean opt_dry_run = FALSE;

static const GOptionEntry options[] = {
	{ "iso",     'i', 0, G_OPTION_ARG_FILENAME, &opt_iso,
	  "Preselect the ISO image pointed by PATH", "PATH" },
	{ "device",  'd', 0, G_OPTION_ARG_STRING,   &opt_device,
	  "Preselect the target device",             "DEVICE" },
	{ "filesystem", 'f', 0, G_OPTION_ARG_STRING, &opt_fs,
	  "Preselect the file system to be preferred when formatting", "FILESYSTEM" },
	{ "verbose", 'v', 0, G_OPTION_ARG_NONE,     &opt_verbose,
	  "Debug-level logging", NULL },
	{ "dry-run", 0,   0, G_OPTION_ARG_NONE,     &opt_dry_run,
	  "Never touch a device: log what would happen", NULL },
	{ NULL }
};

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");

	/* Rufus' own switches are -i/-f/-x/-g; we keep -i and -f compatible. */
	RufoxApp *app = rufox_app_new();
	GOptionContext *ctx = g_option_context_new("— Rufus for Linux");
	g_option_context_add_main_entries(ctx, options, NULL);
	g_option_context_set_summary(ctx,
		"A reliable USB formatting utility. Derived from Rufus by Pete Batard "
		"(GPL-3.0-or-later).");

	GError *err = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		fprintf(stderr, "rufox: %s\n", err != NULL ? err->message : "bad options");
		g_clear_error(&err);
		g_option_context_free(ctx);
		g_object_unref(app);
		return 2;
	}
	g_option_context_free(ctx);

	if (opt_verbose)
		rfx_log_set_level(RFX_LOG_DEBUG);
	if (opt_dry_run)
		uprintf("Started in dry-run mode: no device will be modified");

	/* The GTK init needs to happen before anything touches Gdk. */
	int status = g_application_run(G_APPLICATION(app), argc, argv);

	g_free(opt_iso);
	g_free(opt_device);
	g_free(opt_fs);
	g_object_unref(app);
	return status;
}
