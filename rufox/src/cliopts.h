/*
 * rufox — shared command-line parsing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RUFOX_CLIOPTS_H
#define RUFOX_CLIOPTS_H

#include "rufox.h"

G_BEGIN_DECLS

typedef struct {
	const char *command;
	const char *device;
	const char *image;
	const char *log_file;
	bool        extra_devs;
	bool        assume_yes;
	int         bb_mode;
	int         bb_passes;
	int         hash_type;
	RfxOptions  opt;
} RfxCliOpts;

/* Returns 0 on success, 1 for --help, 2 on a usage error. */
int rfx_cli_parse(int argc, char **argv, RfxCliOpts *out);

/* Re-serialise a job into argv for the privileged helper. */
GPtrArray* rfx_cli_job_argv(const char *helper_path, const RfxCliOpts *o);

G_END_DECLS

#endif
