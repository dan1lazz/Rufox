/*
 * rufox — GUI engine wrapper
 *
 * RfxJob is blocking and must never run on the GTK main thread, otherwise the
 * progress bar freezes — the exact failure mode that makes a writer feel
 * broken. This is a small GObject that owns a GTask plus a GMainContext-owned
 * idle source, so progress arrives on the UI thread as signals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/rufox-gui.h"
#include "rufox-config.h"
#include "cliopts.h"
#include <stdio.h>
#include <string.h>
#include <stdio.h>

enum {
	SIG_PROGRESS,
	SIG_LOG,
	SIG_FINISHED,
	SIG_DEVICES_CHANGED,
	N_SIGNALS
};
static guint signals[N_SIGNALS];

struct _RufoxEngine {
	GObject parent;

	RfxJobHandle *job;
	RfxCancel    *cancel;
	GTask        *task;
	guint         idle_id;
	gboolean      running;

	/* Progress is produced on the worker thread and consumed on the UI
	 * thread; the queue keeps the two apart. */
	GAsyncQueue  *progress_q;
	GAsyncQueue  *log_q;

	RfxDevList   *devices;
};

G_DEFINE_TYPE(RufoxEngine, rufox_engine, G_TYPE_OBJECT)

/* ----------------------------------------------------------------------- */
typedef struct {
	RfxProgress p;
} ProgressItem;

typedef struct {
	RfxLogLevel lvl;
	char        line[1024];
} LogItem;

static void engine_on_progress(const RfxProgress *p, gpointer ud)
{
	RufoxEngine *self = RUFOX_ENGINE(ud);
	ProgressItem *it = g_new0(ProgressItem, 1);
	it->p = *p;
	g_async_queue_push(self->progress_q, it);
	g_idle_add(rufox_engine_drain_progress, self);
}

static void engine_on_log(RfxLogLevel lvl, const char *line, gpointer ud)
{
	RufoxEngine *self = RUFOX_ENGINE(ud);
	LogItem *it = g_new0(LogItem, 1);
	it->lvl = lvl;
	g_strlcpy(it->line, line, sizeof(it->line));
	g_async_queue_push(self->log_q, it);
	g_idle_add(rufox_engine_drain_log, self);
}

gboolean rufox_engine_drain_progress(gpointer ud)
{
	RufoxEngine *self = RUFOX_ENGINE(ud);
	ProgressItem *it;
	while ((it = g_async_queue_try_pop(self->progress_q)) != NULL) {
		g_signal_emit(self, signals[SIG_PROGRESS], 0, &it->p);
		g_free(it);
	}
	return G_SOURCE_REMOVE;
}

gboolean rufox_engine_drain_log(gpointer ud)
{
	RufoxEngine *self = RUFOX_ENGINE(ud);
	LogItem *it;
	while ((it = g_async_queue_try_pop(self->log_q)) != NULL) {
		g_signal_emit(self, signals[SIG_LOG], 0, (int)it->lvl, it->line);
		g_free(it);
	}
	return G_SOURCE_REMOVE;
}

/* ----------------------------------------------------------------------- */
/*
 * Unprivileged path: run the whole job inside rufox-helper under pkexec and
 * translate its line protocol back into signals. This is the polkit-based
 * equivalent of Rufus' single elevated process.
 */
static bool run_via_helper(RufoxEngine *self, RfxResult *out)
{
	const char *libexec = RUFOX_LIBEXECDIR;
	g_autofree char *helper = g_build_filename(libexec, "rufox-helper", NULL);
	if (!g_file_test(helper, G_FILE_TEST_IS_EXECUTABLE))
		return false;

	RfxJob *j = rfx_job_peek(self->job);
	RfxCliOpts o;
	memset(&o, 0, sizeof(o));
	o.device = j->DevicePath;
	o.image = j->ImagePath[0] != '\0' ? j->ImagePath : NULL;
	o.opt = j->Options;

	g_autoptr(GPtrArray) argv = rfx_cli_job_argv(helper, &o);
	/* Prepend pkexec when we are not root ourselves. */
	GPtrArray *full = g_ptr_array_new_with_free_func(g_free);
	if (!rfx_is_root())
		g_ptr_array_add(full, g_strdup("pkexec"));
	for (guint i = 0; i < argv->len; i++)
		g_ptr_array_add(full, g_strdup(g_ptr_array_index(argv, i)));
	g_ptr_array_add(full, NULL);

	GSubprocess *p = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE) != NULL
		? g_subprocess_launcher_spawnv(
			g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			                          G_SUBPROCESS_FLAGS_STDERR_MERGE),
			(const gchar *const *)full->pdata, NULL)
		: NULL;
	g_ptr_array_free(full, TRUE);
	if (p == NULL)
		return false;

	GInputStream *in = g_subprocess_get_stdout_pipe(p);
	g_autoptr(GDataInputStream) dis = g_data_input_stream_new(in);
	char *line;
	*out = RFX_RESULT_FAILED;
	double seconds = 0;
	gchar *errmsg = NULL;

	while ((line = g_data_input_stream_read_line_utf8(dis, NULL, NULL, NULL)) != NULL) {
		if (g_str_has_prefix(line, "PROGRESS ")) {
			RfxProgress pr;
			memset(&pr, 0, sizeof(pr));
			int op; double pct, bps, eta;
			guint64 bd, bt;
			char msg[256] = "";
			if (sscanf(line, "PROGRESS %d %lf %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
			             " %lf %lf %255[^\n]", &op, &pct, &bd, &bt, &bps, &eta, msg) >= 6) {
				pr.op = (RfxOp)op;
				pr.progress = (float)pct;
				pr.bytes_done = bd;
				pr.bytes_total = bt;
				pr.speed_bps = bps;
				pr.eta_seconds = eta;
				g_snprintf(pr.message, sizeof(pr.message), "%s", msg);
				g_async_queue_push(self->progress_q, g_memdup2(&pr, sizeof(pr)));
				g_idle_add(rufox_engine_drain_progress, self);
			}
		} else if (g_str_has_prefix(line, "LOG ")) {
			LogItem *it = g_new0(LogItem, 1);
			int lvl = 0;
			sscanf(line, "LOG %d %1023[^\n]", &lvl, it->line);
			it->lvl = (RfxLogLevel)lvl;
			g_async_queue_push(self->log_q, it);
			g_idle_add(rufox_engine_drain_log, self);
		} else if (g_str_has_prefix(line, "RESULT ")) {
			char verdict[16] = "";
			char msg[512] = "";
			sscanf(line, "RESULT %15s %lf %511[^\n]", verdict, &seconds, msg);
			*out = strcmp(verdict, "success") == 0 ? RFX_RESULT_SUCCESS :
			       strcmp(verdict, "cancelled") == 0 ? RFX_RESULT_CANCELLED :
			       RFX_RESULT_FAILED;
			g_free(errmsg);
			errmsg = g_strdup(msg);
		}
		g_free(line);
	}

	g_subprocess_wait(p, NULL, NULL);
	int st = g_subprocess_get_exit_status(p);
	g_object_unref(p);
	if (*out == RFX_RESULT_NONE)
		*out = (st == 0) ? RFX_RESULT_SUCCESS : RFX_RESULT_FAILED;
	g_free(errmsg);
	(void)seconds;
	return true;
}

static void worker_task(GTask *task, gpointer source, gpointer data, GCancellable *gc)
{
	RufoxEngine *self = RUFOX_ENGINE(source);
	(void)data; (void)gc;

	RfxResult r;
	if (!rfx_is_root() && run_via_helper(self, &r)) {
		g_task_return_int(task, (gssize)r);
		return;
	}
	r = rfx_job_run(self->job, self->cancel);
	g_task_return_int(task, (gssize)r);
}

static void worker_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	RufoxEngine *self = RUFOX_ENGINE(ud);
	GError *err = NULL;
	gssize r = g_task_propagate_int(G_TASK(res), &err);

	self->running = FALSE;
	rfx_log_set_callback(NULL, NULL);

	RfxJob *j = rfx_job_peek(self->job);
	g_signal_emit(self, signals[SIG_FINISHED], 0, (int)r,
	              j != NULL ? j->ErrorMessage : "",
	              j != NULL ? j->ElapsedSeconds : 0.0);
	g_clear_error(&err);
	g_clear_object(&self->task);
	g_object_unref(self);
}

/* ----------------------------------------------------------------------- */
static void rufox_engine_dispose(GObject *obj)
{
	RufoxEngine *self = RUFOX_ENGINE(obj);
	if (self->cancel != NULL)
		rfx_cancel_request(self->cancel);
	g_clear_object(&self->task);
	g_clear_pointer(&self->devices, rfx_dev_list_free);
	rfx_log_set_callback(NULL, NULL);
	G_OBJECT_CLASS(rufox_engine_parent_class)->dispose(obj);
}

static void rufox_engine_finalize(GObject *obj)
{
	RufoxEngine *self = RUFOX_ENGINE(obj);
	g_clear_pointer(&self->job, rfx_job_free);
	g_clear_pointer(&self->cancel, rfx_cancel_free);
	g_clear_pointer(&self->progress_q, g_async_queue_unref);
	g_clear_pointer(&self->log_q, g_async_queue_unref);
	G_OBJECT_CLASS(rufox_engine_parent_class)->finalize(obj);
}

static void rufox_engine_class_init(RufoxEngineClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->dispose = rufox_engine_dispose;
	oc->finalize = rufox_engine_finalize;

	signals[SIG_PROGRESS] = g_signal_new("progress", G_TYPE_FROM_CLASS(klass),
	                                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
	                                     G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[SIG_LOG] = g_signal_new("log", G_TYPE_FROM_CLASS(klass),
	                                G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
	                                G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_STRING);
	signals[SIG_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass),
	                                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
	                                     G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_STRING,
	                                     G_TYPE_DOUBLE);
	signals[SIG_DEVICES_CHANGED] = g_signal_new("devices-changed", G_TYPE_FROM_CLASS(klass),
	                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
	                                            G_TYPE_NONE, 0);
}

static void rufox_engine_init(RufoxEngine *self)
{
	self->job = rfx_job_new();
	self->cancel = rfx_cancel_new();
	self->progress_q = g_async_queue_new();
	self->log_q = g_async_queue_new();
	self->devices = rfx_dev_list_new();
	rfx_job_set_progress_cb(self->job, engine_on_progress, self);
}

RufoxEngine* rufox_engine_new(void)
{
	return g_object_new(RUFOX_TYPE_ENGINE, NULL);
}

gboolean rufox_engine_is_running(RufoxEngine *self)
{
	return self->running;
}

RfxJob* rufox_engine_peek_job(RufoxEngine *self)
{
	return rfx_job_peek(self->job);
}

void rufox_engine_set_device(RufoxEngine *self, const char *devpath)
{
	rfx_job_set_device(self->job, devpath);
}

void rufox_engine_set_image(RufoxEngine *self, const char *image)
{
	rfx_job_set_image(self->job, image);
}

void rufox_engine_set_options(RufoxEngine *self, const RfxOptions *opt)
{
	rfx_job_set_options(self->job, opt);
}

void rufox_engine_start(RufoxEngine *self)
{
	if (self->running) return;
	self->running = TRUE;

	/* Route the engine's log into the GUI's log pane as well as stderr. */
	rfx_log_set_callback(engine_on_log, self);

	g_object_ref(self);
	self->task = g_task_new(self, NULL, worker_done, NULL);
	g_task_set_task_data(self->task, NULL, NULL);
	g_task_run_in_thread(self->task, worker_task);
}

void rufox_engine_cancel(RufoxEngine *self)
{
	if (self->running)
		rfx_cancel_request(self->cancel);
}

/* ----------------------------------------------------------------------- */
/* Device list                                                             */
/* ----------------------------------------------------------------------- */
int rufox_engine_refresh_devices(RufoxEngine *self)
{
	if (!rfx_dev_list_refresh(self->devices))
		return 0;
	g_signal_emit(self, signals[SIG_DEVICES_CHANGED], 0);
	return rfx_dev_list_count(self->devices);
}

const RfxDrive* rufox_engine_get_device(RufoxEngine *self, int i)
{
	return rfx_dev_list_get(self->devices, i);
}

int rufox_engine_device_count(RufoxEngine *self)
{
	return rfx_dev_list_count(self->devices);
}

/* ----------------------------------------------------------------------- */
/* Image scanning, off the UI thread                                       */
/* ----------------------------------------------------------------------- */
typedef struct {
	char *path;
	RfxImgReport rep;
} ScanData;

static void scan_data_free(gpointer p)
{
	ScanData *sd = p;
	if (sd == NULL) return;
	g_free(sd->path);
	g_free(sd);
}

static void scan_worker(GTask *task, gpointer source, gpointer data, GCancellable *gc)
{
	ScanData *sd = data;
	(void)source; (void)gc;
	bool ok = rfx_iso_scan(sd->path, &sd->rep, NULL);
	g_task_return_boolean(task, ok);
}

void rufox_engine_scan_image(RufoxEngine *self, const char *path,
                             GAsyncReadyCallback cb, gpointer ud)
{
	ScanData *sd = g_new0(ScanData, 1);
	sd->path = g_strdup(path);
	GTask *t = g_task_new(self, NULL, cb, ud);
	g_task_set_task_data(t, sd, scan_data_free);
	g_task_run_in_thread(t, scan_worker);
	g_object_unref(t);
}

gboolean rufox_engine_scan_finish(RufoxEngine *self, GAsyncResult *res,
                                  RfxImgReport *out, GError **error)
{
	(void)self;
	GTask *t = G_TASK(res);
	gboolean ok = g_task_propagate_boolean(t, error);
	ScanData *sd = g_task_get_task_data(t);
	if (ok && sd != NULL && out != NULL)
		*out = sd->rep;
	return ok;
}
