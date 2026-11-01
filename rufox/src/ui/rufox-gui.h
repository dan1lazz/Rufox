/*
 * rufox — GTK4/libadwaita front-end, shared header
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFOX_GUI_H
#define RUFOX_GUI_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "../rufox.h"

G_BEGIN_DECLS

/* ----------------------------------------------------------------------- */
/* Engine                                                                  */
/* ----------------------------------------------------------------------- */
#define RUFOX_TYPE_ENGINE (rufox_engine_get_type())
G_DECLARE_FINAL_TYPE(RufoxEngine, rufox_engine, RUFOX, ENGINE, GObject)

RufoxEngine* rufox_engine_new(void);
gboolean    rufox_engine_is_running(RufoxEngine *self);
RfxJob*     rufox_engine_peek_job(RufoxEngine *self);
void        rufox_engine_set_device(RufoxEngine *self, const char *devpath);
void        rufox_engine_set_image(RufoxEngine *self, const char *image);
void        rufox_engine_set_options(RufoxEngine *self, const RfxOptions *opt);
void        rufox_engine_start(RufoxEngine *self);
void        rufox_engine_cancel(RufoxEngine *self);

int               rufox_engine_refresh_devices(RufoxEngine *self);
const RfxDrive*   rufox_engine_get_device(RufoxEngine *self, int i);
int               rufox_engine_device_count(RufoxEngine *self);

void     rufox_engine_scan_image(RufoxEngine *self, const char *path,
                                 GAsyncReadyCallback cb, gpointer ud);
gboolean rufox_engine_scan_finish(RufoxEngine *self, GAsyncResult *res,
                                  RfxImgReport *out, GError **error);

gboolean rufox_engine_drain_progress(gpointer ud);
gboolean rufox_engine_drain_log(gpointer ud);

/* ----------------------------------------------------------------------- */
/* Application / window                                                    */
/* ----------------------------------------------------------------------- */
#define RUFOX_TYPE_APP (rufox_app_get_type())
G_DECLARE_FINAL_TYPE(RufoxApp, rufox_app, RUFOX, APP, AdwApplication)

#define RUFOX_TYPE_WINDOW (rufox_window_get_type())
G_DECLARE_FINAL_TYPE(RufoxWindow, rufox_window, RUFOX, WINDOW, AdwApplicationWindow)

GtkWidget* rufox_window_new(RufoxApp *app);
RufoxApp*  rufox_app_new(void);

G_END_DECLS

#endif /* RUFOX_GUI_H */
