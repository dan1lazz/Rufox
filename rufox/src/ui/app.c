/*
 * rufox — application object
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/rufox-gui.h"
#include <stdio.h>

struct _RufoxApp {
	AdwApplication parent;
	GtkWidget *window;
	char *startup_image;
	char *startup_device;
	char *startup_fs;
	gboolean startup_dry_run;
};

G_DEFINE_TYPE(RufoxApp, rufox_app, ADW_TYPE_APPLICATION)

static void on_about(GSimpleAction *action, GVariant *param, gpointer ud)
{
	RufoxApp *self = RUFOX_APP(ud);
	(void)action; (void)param;

	/* GtkAboutDialog keeps the release compatible with libadwaita 1.0/1.1,
	 * which are still shipped by Ubuntu 22.04 and lack AdwAboutWindow. */
	GtkWidget *about = gtk_about_dialog_new();
	gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "Rufox");
	gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "io.github.rufox");
	gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), RUFOX_VERSION_STR);
	gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0_ONLY);
	gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about), "https://github.com/dan1lazz/Rufox");
	gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about),
		"A Linux port of Rufus, The Reliable USB Formatting Utility.\n\n"
		"Derived from Rufus by Pete Batard <pete@akeo.ie>, licensed under the\n"
		"GNU General Public License version 3 or later. The partition geometry,\n"
		"the image analysis rules and the bootloader config patching follow\n"
		"Rufus' own implementation, so that the resulting media behaves the same.\n\n"
		"Report issues at https://github.com/dan1lazz/Rufox/issues");
	gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(about),
		"\xc2\xa9 2011-2026 Pete Batard (Rufus)\n"
		"\xc2\xa9 2026 rufox contributors (this port)");

	if (self->window != NULL)
		gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(self->window));
	gtk_window_present(GTK_WINDOW(about));
}

static void on_upstream(GSimpleAction *action, GVariant *param, gpointer ud)
{
	RufoxApp *self = RUFOX_APP(ud);
	(void)action; (void)param;
	/* gtk_uri_launch() is GTK 4.10+; 4.8 has gtk_show_uri(). */
	gtk_show_uri(GTK_WINDOW(self->window), "https://rufus.ie", GDK_CURRENT_TIME);
}

static void on_quit(GSimpleAction *action, GVariant *param, gpointer ud)
{
	RufoxApp *self = RUFOX_APP(ud);
	(void)action; (void)param;
	if (self->window != NULL)
		gtk_window_destroy(GTK_WINDOW(self->window));
}

static void app_activate(GApplication *app)
{
	RufoxApp *self = RUFOX_APP(app);
	if (self->window == NULL)
		self->window = rufox_window_new(self, self->startup_image,
		                                self->startup_device, self->startup_fs,
		                                self->startup_dry_run);
	gtk_window_present(GTK_WINDOW(self->window));
}

static void rufox_app_startup(GApplication *app)
{
	G_APPLICATION_CLASS(rufox_app_parent_class)->startup(app);

	static const GActionEntry entries[] = {
		{ "about",    on_about,    NULL, NULL, NULL, {} },
		{ "upstream", on_upstream, NULL, NULL, NULL, {} },
		{ "quit",     on_quit,     NULL, NULL, NULL, {} },
	};
	g_action_map_add_action_entries(G_ACTION_MAP(app), entries,
	                                G_N_ELEMENTS(entries), app);

	GtkCssProvider *prov = gtk_css_provider_new();
	gtk_css_provider_load_from_data(prov,
	                                ".rufox-device { font-weight: bold; }\n", -1);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	                                           GTK_STYLE_PROVIDER(prov),
	                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(prov);
}

static void rufox_app_finalize(GObject *obj)
{
	RufoxApp *self = RUFOX_APP(obj);
	g_clear_pointer(&self->startup_image, g_free);
	g_clear_pointer(&self->startup_device, g_free);
	g_clear_pointer(&self->startup_fs, g_free);
	G_OBJECT_CLASS(rufox_app_parent_class)->finalize(obj);
}

static void rufox_app_class_init(RufoxAppClass *klass)
{
	G_APPLICATION_CLASS(klass)->activate = app_activate;
	G_APPLICATION_CLASS(klass)->startup = rufox_app_startup;
	G_OBJECT_CLASS(klass)->finalize = rufox_app_finalize;
}

static void rufox_app_init(RufoxApp *self)
{
	self->window = NULL;
}

RufoxApp* rufox_app_new(void)
{
	return g_object_new(RUFOX_TYPE_APP,
	                    "application-id", "io.github.rufox",
	                    "flags", G_APPLICATION_FLAGS_NONE,
	                    NULL);
}

void rufox_app_set_startup_options(RufoxApp *app, const char *image,
                                   const char *device, const char *filesystem,
                                   gboolean dry_run)
{
	g_return_if_fail(RUFOX_IS_APP(app));
	app->startup_image = g_strdup(image);
	app->startup_device = g_strdup(device);
	app->startup_fs = g_strdup(filesystem);
	app->startup_dry_run = dry_run;
}
