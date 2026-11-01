/*
 * rufox — external helper execution (Rufus: process.c / RunCommandU)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <string.h>
#include <errno.h>

void rfx_proc_result_clear(RfxProcResult *res)
{
	if (res == NULL) return;
	g_clear_pointer(&res->std_out, g_free);
	g_clear_pointer(&res->std_err, g_free);
	res->exit_status = 0;
}

bool rfx_run(GStrv argv, const char *cwd, RfxProcResult *res, GError **error)
{
	g_return_val_if_fail(argv != NULL && argv[0] != NULL, false);
	if (res != NULL)
		rfx_proc_result_clear(res);

	GSubprocessLauncher *launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		                            G_SUBPROCESS_FLAGS_STDERR_PIPE);
	if (cwd != NULL)
		g_subprocess_launcher_set_cwd(launcher, cwd);

	/* Children must never inherit a half-open block device of ours. */
	g_subprocess_launcher_unsetenv(launcher, "LD_PRELOAD");

	char *joined = g_strjoinv(" ", (gchar**)argv);
	uprintfd("$ %s", joined);
	g_free(joined);

	GSubprocess *p = g_subprocess_launcher_spawnv(launcher,
	                                              (const gchar *const *)argv, error);
	g_object_unref(launcher);
	if (p == NULL) {
		rfx_set_error(ERR_MISSING_TOOL, 0, "Failed to execute '%s': %s",
		              argv[0], error && *error ? (*error)->message : "?");
		return false;
	}

	char *so = NULL, *se = NULL;
	if (!g_subprocess_communicate_utf8(p, NULL, NULL, &so, &se, error)) {
		/* communicate_utf8 fails on non-UTF8 output from e.g. mkfs */
		g_clear_error(error);
		g_subprocess_wait(p, NULL, NULL);
	}
	int status = g_subprocess_get_exit_status(p);
	g_object_unref(p);

	if (res != NULL) {
		res->exit_status = status;
		res->std_out = so;
		res->std_err = se;
		so = se = NULL;
	} else {
		if (so != NULL && *so != '\0') uprintfd("| %s", so);
		if (se != NULL && *se != '\0') uprintfd("| %s", se);
	}
	g_free(so);
	g_free(se);

	if (status != 0)
		uprintfd("! '%s' exited with status %d", argv[0], status);

	return status == 0;
}

bool rfx_run_ok(GStrv argv, const char *cwd, GError **error)
{
	RfxProcResult res = { 0 };
	bool r = rfx_run(argv, cwd, &res, error);
	if (!r && error != NULL && (*error == NULL)) {
		char *msg = g_strdup_printf("'%s' failed (exit %d)%s%s",
		                            argv[0], res.exit_status,
		                            res.std_err && *res.std_err ? ": " : "",
		                            res.std_err ? res.std_err : "");
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
		g_free(msg);
	}
	rfx_proc_result_clear(&res);
	return r;
}
