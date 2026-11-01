/*
 * rufox-helper — the privileged half of rufox
 *
 * On a desktop without udisks2 (or with it locked down), the unprivileged GUI
 * cannot open /dev/sdX for writing. Rather than making the GUI setuid, the
 * whole job runs here, once, under polkit: the GUI shells out to
 *
 *     pkexec /usr/libexec/rufox-helper write -d /dev/sdb ...
 *
 * and reads a line protocol off stdout:
 *
 *     PROGRESS <op> <pct> <bytes_done> <bytes_total> <bps> <eta> <message>
 *     LOG <level> <line>
 *     RESULT <success|failed|cancelled> <seconds> <error message>
 *
 * Because the engine and the helper share src/cliopts.c, the two processes
 * cannot disagree about what a job means.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "cliopts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit_log(RfxLogLevel lvl, const char *line, gpointer ud)
{
	(void)ud;
	printf("LOG %d %s\n", (int)lvl, line);
	fflush(stdout);
}

static void emit_progress(const RfxProgress *p, gpointer ud)
{
	(void)ud;
	printf("PROGRESS %d %.4f %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
	       " %.2f %.2f %s\n",
	       (int)p->op, (double)p->progress, p->bytes_done, p->bytes_total,
	       p->speed_bps, p->eta_seconds, p->message);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	RfxCliOpts o;
	int rc = rfx_cli_parse(argc, argv, &o);
	if (rc != 0) return rc == 1 ? 0 : 2;

	if (o.command == NULL || strcmp(o.command, "write") != 0) {
		fprintf(stderr, "rufox-helper only accepts 'write'\n");
		return 2;
	}
	if (o.device == NULL) {
		fprintf(stderr, "rufox-helper needs -d DEVICE\n");
		return 2;
	}
	if (!rfx_is_root()) {
		fprintf(stderr, "rufox-helper must run as root (use pkexec)\n");
		return 2;
	}

	/* Everything the GUI shows comes back through the line protocol; stderr
	 * keeps a human-readable copy for journalctl. */
	rfx_log_set_callback(emit_log, NULL);
	rfx_log_set_level(RFX_LOG_INFO);

	RfxJobHandle *h = rfx_job_new();
	rfx_job_set_device(h, o.device);
	rfx_job_set_image(h, o.image);
	rfx_job_set_options(h, &o.opt);
	rfx_job_set_progress_cb(h, emit_progress, NULL);

	RfxResult r = rfx_job_run(h, NULL);
	RfxJob *j = rfx_job_peek(h);

	printf("RESULT %s %.2f %s\n",
	       r == RFX_RESULT_SUCCESS ? "success" :
	       r == RFX_RESULT_CANCELLED ? "cancelled" : "failed",
	       j->ElapsedSeconds, j->ErrorMessage);
	fflush(stdout);

	int exit_code = r == RFX_RESULT_SUCCESS ? 0 : 1;
	rfx_job_free(h);
	return exit_code;
}
