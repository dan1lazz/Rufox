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
#include "rufox-config.h"

#include <stdio.h>
#include <locale.h>

static char *opt_iso = NULL;
static char *opt_device = NULL;
static char *opt_fs = NULL;
static gboolean opt_verbose = FALSE;
static gboolean opt_dry_run = FALSE;

static const GOptionEntry options[] = {
	{ "iso",     'i', 0, G_OPTION_ARG_FILENAME, &opt_iso,
	  N_("Preselect the ISO image pointed by PATH"), "PATH" },
	{ "device",  'd', 0, G_OPTION_ARG_STRING,   &opt_device,
	  N_("Preselect the target device"),             "DEVICE" },
	{ "filesystem", 'f', 0, G_OPTION_ARG_STRING, &opt_fs,
	  N_("Preselect the file system to be preferred when formatting"), "FILESYSTEM" },
	{ "verbose", 'v', 0, G_OPTION_ARG_NONE,     &opt_verbose,
	  N_("Debug-level logging"), NULL },
	{ "dry-run", 0,   0, G_OPTION_ARG_NONE,     &opt_dry_run,
	  N_("Never touch a device: log what would happen"), NULL },
	{ NULL }
};

static void init_localization(void)
{
	/* LANGUAGE must be selected before setlocale(). The schema is optional in
	 * a developer build, but is always present in installed/AppImage builds. */
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema = source != NULL ?
		g_settings_schema_source_lookup(source, "io.github.rufox", TRUE) : NULL;
	if (schema != NULL) {
		g_autoptr(GSettings) settings = g_settings_new("io.github.rufox");
		g_autofree char *language = g_settings_get_string(settings, "language");
		if (g_strcmp0(language, "en") == 0 || g_strcmp0(language, "ru") == 0)
			g_setenv("LANGUAGE", language, TRUE);
		g_settings_schema_unref(schema);
	}
	setlocale(LC_ALL, "");

	const char *appdir = g_getenv("APPDIR");
	g_autofree char *localedir = appdir != NULL && appdir[0] != '\0' ?
		g_build_filename(appdir, "usr", "share", "locale", NULL) :
		g_strdup(RUFOX_LOCALEDIR);
	bindtextdomain("rufox", localedir);
	bind_textdomain_codeset("rufox", "UTF-8");
	textdomain("rufox");
}

int main(int argc, char **argv)
{
	init_localization();

	/* Rufus' own switches are -i/-f/-x/-g; we keep -i and -f compatible. */
	RufoxApp *app = rufox_app_new();
	GOptionContext *ctx = g_option_context_new(N_("— Rufus for Linux"));
	g_option_context_add_main_entries(ctx, options, "rufox");
	g_option_context_set_summary(ctx,
		_("A reliable USB formatting utility. Derived from Rufus by Pete Batard "
		  "(GPL-3.0-or-later)."));

	GError *err = NULL;
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		fprintf(stderr, "rufox: %s\n", err != NULL ? err->message : _("bad options"));
		g_clear_error(&err);
		g_option_context_free(ctx);
		g_object_unref(app);
		return 2;
	}
	g_option_context_free(ctx);

	if (opt_verbose)
		rfx_log_set_level(RFX_LOG_DEBUG);
	if (opt_fs != NULL) {
		bool valid = false;
		for (int fs = RFX_FS_UNKNOWN + 1; fs < RFX_FS_MAX; fs++) {
			if (g_ascii_strcasecmp(opt_fs, rfx_format_fs_name((RfxFs)fs)) == 0) {
				valid = true;
				break;
			}
		}
		if (!valid) {
			fprintf(stderr, _("rufox-gui: unknown file system '%s'\n"), opt_fs);
			g_object_unref(app);
			return 2;
		}
	}
	rufox_app_set_startup_options(app, opt_iso, opt_device, opt_fs, opt_dry_run);
	if (opt_dry_run)
		uprintf(_("Started in dry-run mode: no device will be modified"));

	/* The GTK init needs to happen before anything touches Gdk. */
	int status = g_application_run(G_APPLICATION(app), argc, argv);

	g_free(opt_iso);
	g_free(opt_device);
	g_free(opt_fs);
	g_object_unref(app);
	return status;
}
