/*
 * rufox — bad blocks detection
 *
 * Port of Rufus' src/badblocks.c. The algorithm is unchanged: write a pattern
 * over the whole medium, read it back, compare. Rufus offers Quick (1 pass of
 * 0x00), Slow (4 passes of 0x00/0xFF/random/random) and Non-destructive
 * (read-only, keeps the data). We add the Linux-specific nicety that we can
 * also just call badblocks(8) when it is present, since that is what users
 * will expect from a Linux tool.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define BB_BUF (1024 * 1024)

static void fill_pattern(uint8_t *buf, size_t len, int pass, bool randomise)
{
	if (!randomise) {
		memset(buf, (pass % 2) ? 0xFF : 0x00, len);
		return;
	}
	/* Rufus uses a simple LCG so that a failing block can be reproduced. */
	static uint32_t state = 0x1234ABCD;
	uint32_t *w = (uint32_t*)buf;
	for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
		state = state * 1103515245u + 12345u;
		w[i] = state;
	}
}

static bool bb_native(const char *devpath, uint64_t size, uint32_t block_size,
                      RfxBadBlocksMode mode, int passes, RfxBadBlocksReport *report,
                      RfxProgressCb cb, gpointer ud, RfxCancel *cancel)
{
	int fd = rfx_open_block(devpath, mode != RFX_BB_NONDESTRUCTIVE);
	if (fd < 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s': %s", devpath, strerror(errno));

	g_autofree uint8_t *wbuf = g_malloc(BB_BUF);
	g_autofree uint8_t *rbuf = g_malloc(BB_BUF);

	int n_passes = passes;
	if (mode == RFX_BB_QUICK)          n_passes = 1;
	else if (mode == RFX_BB_NONDESTRUCTIVE) n_passes = 1;

	uprintf("Checking for bad blocks (%s, %d pass%s):",
	        mode == RFX_BB_QUICK ? "quick" :
	        mode == RFX_BB_SLOW ? "slow" : "non-destructive",
	        n_passes, n_passes == 1 ? "" : "es");

	GTimer *t = g_timer_new();
	uint64_t total = size;

	for (int pass = 0; pass < n_passes; pass++) {
		if (rfx_cancel_requested(cancel)) {
			close(fd);
			g_timer_destroy(t);
			return rfx_fail(ERR_CANCELLED, "Cancelled by user");
		}

		bool write_pass = (mode != RFX_BB_NONDESTRUCTIVE);
		fill_pattern(wbuf, BB_BUF, pass, mode == RFX_BB_SLOW && pass >= 2);
		uprintf("  Pass %d/%d (%s)", pass + 1, n_passes,
		        write_pass ? (pass % 2 ? "0xFF" : (mode == RFX_BB_SLOW && pass >= 2
		                                          ? "pseudo-random" : "0x00"))
		                   : "read-only");

		for (uint64_t off = 0; off < total; off += BB_BUF) {
			if (rfx_cancel_requested(cancel)) {
				close(fd);
				g_timer_destroy(t);
				return rfx_fail(ERR_CANCELLED, "Cancelled by user");
			}
			size_t chunk = (size_t)MIN((uint64_t)BB_BUF, total - off);
			if (chunk % block_size != 0 && chunk > block_size)
				chunk -= chunk % block_size;

			if (write_pass) {
				if (!rfx_write_exact(fd, wbuf, chunk, off)) {
					uprintfw("Write error at offset %" G_GUINT64_FORMAT
					         ": %s", off, strerror(errno));
				}
			}
			if (!rfx_read_exact(fd, rbuf, chunk, off)) {
				uprintfw("Read error at offset %" G_GUINT64_FORMAT ": %s",
				         off, strerror(errno));
				continue;
			}
			if (write_pass && memcmp(wbuf, rbuf, chunk) != 0) {
				for (size_t i = 0; i < chunk; i += block_size) {
					if (memcmp(wbuf + i, rbuf + i,
					           (size_t)MIN((size_t)block_size, chunk - i)) != 0) {
						uint64_t blk = (off + i) / block_size;
						g_array_append_val(report->corrupted, blk);
						report->blocks_corrupted++;
					}
				}
			}
			report->blocks_tested += chunk / block_size;

			if (cb != NULL) {
				double el = g_timer_elapsed(t, NULL);
				double frac = (double)(off + chunk) / (double)total;
				RfxProgress p = { 0 };
				p.op = RFX_OP_BADBLOCKS;
				p.bytes_done = off + chunk;
				p.bytes_total = total;
				p.progress = (float)((pass + frac) / n_passes);
				p.speed_bps = el > 0 ? (double)(off + chunk) / el : 0;
				p.eta_seconds = p.speed_bps > 0
				                ? (double)(total * n_passes - (off + chunk)) / p.speed_bps
				                : -1;
				g_snprintf(p.message, sizeof(p.message),
				           "Bad blocks pass %d/%d", pass + 1, n_passes);
				cb(&p, ud);
			}
			uprint_progress(off + chunk, total);
		}
	}

	rfx_sync_block(fd);
	close(fd);
	g_timer_destroy(t);
	uprintf("\nBad blocks: %" G_GUINT64_FORMAT " block(s) of %u bytes tested, %"
	        G_GUINT64_FORMAT " corrupted",
	        report->blocks_tested, block_size, report->blocks_corrupted);
	return true;
}

/* Prefer the system badblocks(8) when it is there: it understands the medium's
 * geometry better and its output is what Linux users already know. */
static bool bb_external(const char *devpath, uint64_t size, uint32_t block_size,
                        RfxBadBlocksMode mode, int passes, RfxBadBlocksReport *report,
                        RfxProgressCb cb, gpointer ud, RfxCancel *cancel)
{
	GStrv found = rfx_find_tool("badblocks", NULL);
	if (found == NULL) return false;

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(found[0]));
	g_strfreev(found);
	g_ptr_array_add(argv, g_strdup("-b"));
	g_ptr_array_add(argv, g_strdup_printf("%u", block_size));
	if (mode == RFX_BB_NONDESTRUCTIVE)
		g_ptr_array_add(argv, g_strdup("-n"));
	else if (mode == RFX_BB_SLOW)
		g_ptr_array_add(argv, g_strdup("-w"));
	g_ptr_array_add(argv, g_strdup("-s"));		/* show progress */
	g_ptr_array_add(argv, g_strdup("-o"));
	g_ptr_array_add(argv, g_strdup("-"));		/* list to stdout */
	g_ptr_array_add(argv, g_strdup(devpath));
	g_ptr_array_add(argv, NULL);

	(void)size; (void)passes; (void)cb; (void)ud; (void)cancel;

	RfxProcResult res = { 0 };
	GError *err = NULL;
	bool ok = rfx_run((GStrv)argv->pdata, NULL, &res, &err);
	g_ptr_array_free(argv, TRUE);
	g_clear_error(&err);

	if (res.std_out != NULL) {
		g_auto(GStrv) lines = g_strsplit(res.std_out, "\n", -1);
		for (int i = 0; lines[i] != NULL; i++) {
			char *line = g_strstrip(lines[i]);
			if (*line == '\0') continue;
			char *end = NULL;
			uint64_t blk = g_ascii_strtoull(line, &end, 10);
			if (end == line) continue;	/* a summary line, not a block */
			g_array_append_val(report->corrupted, blk);
			report->blocks_corrupted++;
		}
	}
	report->blocks_tested = size / block_size;
	rfx_proc_result_clear(&res);

	if (!ok)
		return rfx_fail(ERR_WRITE_FAULT, "badblocks(8) reported a failure");
	uprintf("Bad blocks: %" G_GUINT64_FORMAT " block(s) tested, %" G_GUINT64_FORMAT
	        " corrupted", report->blocks_tested, report->blocks_corrupted);
	return true;
}

bool rfx_badblocks_check(const char *devpath, uint64_t size, uint32_t block_size,
                         RfxBadBlocksMode mode, int passes, RfxBadBlocksReport *report,
                         RfxProgressCb cb, gpointer ud, RfxCancel *cancel)
{
	g_return_val_if_fail(devpath != NULL && report != NULL, false);

	if (mode == RFX_BB_DISABLED) return true;
	if (block_size == 0) block_size = 4096;
	if (passes < 1) passes = 1;
	if (report->corrupted == NULL)
		report->corrupted = g_array_new(FALSE, FALSE, sizeof(uint64_t));

	if (rfx_cancel_requested(cancel))
		return rfx_fail(ERR_CANCELLED, "Cancelled by user");

	if (bb_external(devpath, size, block_size, mode, passes, report, cb, ud, cancel))
		return true;

	uprintfd("badblocks(8) not available: using the built-in checker");
	return bb_native(devpath, size, block_size, mode, passes, report, cb, ud, cancel);
}
