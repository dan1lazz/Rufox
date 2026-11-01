/*
 * rufox — logging (Rufus' uprintf / PrintStatus)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static GMutex        log_mutex;
static RfxLogLevel   log_level = RFX_LOG_INFO;
static RfxLogCb      log_cb    = NULL;
static gpointer      log_cb_ud = NULL;
static FILE*         log_fp    = NULL;
static bool          stderr_is_tty;
static bool          tty_inited;

void rfx_log_set_level(RfxLogLevel lvl)
{
	log_level = lvl;
}

void rfx_log_set_callback(RfxLogCb cb, gpointer user_data)
{
	g_mutex_lock(&log_mutex);
	log_cb = cb;
	log_cb_ud = user_data;
	g_mutex_unlock(&log_mutex);
}

void rfx_log_set_file(const char *path)
{
	g_mutex_lock(&log_mutex);
	if (log_fp != NULL && log_fp != stderr && log_fp != stdout) {
		fclose(log_fp);
		log_fp = NULL;
	}
	if (path != NULL) {
		log_fp = fopen(path, "a");
		if (log_fp == NULL)
			fprintf(stderr, "rufox: cannot open log file '%s'\n", path);
	}
	g_mutex_unlock(&log_mutex);
}

static const char* level_tag(RfxLogLevel l)
{
	switch (l) {
	case RFX_LOG_DEBUG: return "DBG";
	case RFX_LOG_INFO:  return "   ";
	case RFX_LOG_WARN:  return "WRN";
	case RFX_LOG_ERROR: return "ERR";
	}
	return "???";
}

static const char* level_color(RfxLogLevel l)
{
	switch (l) {
	case RFX_LOG_DEBUG: return "\033[90m";
	case RFX_LOG_WARN:  return "\033[33m";
	case RFX_LOG_ERROR: return "\033[31m";
	default:            return "";
	}
}

void rfx_log(RfxLogLevel lvl, const char *fmt, ...)
{
	va_list ap;
	char body[2048];
	char stamp[16];
	time_t now;
	struct tm tm_buf;

	if (lvl < log_level)
		return;

	va_start(ap, fmt);
	g_vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	/* Strip a trailing newline: Rufus' uprintf() takes them, we normalise. */
	size_t n = strlen(body);
	while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r'))
		body[--n] = '\0';

	now = time(NULL);
	localtime_r(&now, &tm_buf);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_buf);

	g_mutex_lock(&log_mutex);
	if (!tty_inited) {
		stderr_is_tty = isatty(fileno(stderr));
		tty_inited = true;
	}

	if (stderr_is_tty)
		fprintf(stderr, "%s[%s]%s %s\n\033[0m", level_color(lvl), stamp,
		        (lvl == RFX_LOG_ERROR || lvl == RFX_LOG_WARN) ? "" : "", body);
	else
		fprintf(stderr, "[%s] %s %s\n", stamp, level_tag(lvl), body);
	fflush(stderr);

	if (log_fp != NULL) {
		fprintf(log_fp, "[%s] %s %s\n", stamp, level_tag(lvl), body);
		fflush(log_fp);
	}

	if (log_cb != NULL)
		log_cb(lvl, body, log_cb_ud);

	g_mutex_unlock(&log_mutex);
}

/* Rufus prints "\rWriting: 12.3%" style progress onto the console. We keep a
 * simplified carriage-return version that never pollutes a redirected log. */
void uprint_progress(uint64_t done, uint64_t total)
{
	if (!isatty(fileno(stderr)))
		return;

	if (total == 0) {
		fprintf(stderr, "\r%" G_GUINT64_FORMAT " bytes   ", done);
	} else {
		double pct = 100.0 * (double)done / (double)total;
		fprintf(stderr, "\r%6.2f%% (%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT ")",
		        pct, done, total);
	}
	fflush(stderr);
	if (done >= total && total != 0)
		fprintf(stderr, "\n");
}
