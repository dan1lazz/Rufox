/*
 * rufox — checksums
 *
 * Rufus hand-rolls MD5/SHA-1/SHA-256/SHA-512 in src/hash.c (2700 lines)
 * because Windows has no portable API for it. GLib does, so we use GChecksum
 * and spend the saved 2600 lines elsewhere.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define HASH_BUF (512 * 1024)

static GChecksumType glib_type(RfxHashType t)
{
	switch (t) {
	case RFX_HASH_MD5:    return G_CHECKSUM_MD5;
	case RFX_HASH_SHA1:   return G_CHECKSUM_SHA1;
	case RFX_HASH_SHA256: return G_CHECKSUM_SHA256;
	case RFX_HASH_SHA512: return G_CHECKSUM_SHA512;
	default:              return G_CHECKSUM_MD5;
	}
}

const char* rfx_hash_name(RfxHashType t)
{
	switch (t) {
	case RFX_HASH_MD5:    return "MD5";
	case RFX_HASH_SHA1:   return "SHA-1";
	case RFX_HASH_SHA256: return "SHA-256";
	case RFX_HASH_SHA512: return "SHA-512";
	default:              return "none";
	}
}

size_t rfx_hash_hex_length(RfxHashType t);
size_t rfx_hash_hex_length(RfxHashType t)
{
	switch (t) {
	case RFX_HASH_MD5:    return 32;
	case RFX_HASH_SHA1:   return 40;
	case RFX_HASH_SHA256: return 64;
	case RFX_HASH_SHA512: return 128;
	default:              return 0;
	}
}

bool rfx_hash_file(const char *path, RfxHashType type, char *out_hex, size_t out_size,
                   RfxProgressCb cb, gpointer ud, RfxCancel *cancel)
{
	g_return_val_if_fail(path != NULL && out_hex != NULL, false);

	if (type <= RFX_HASH_NONE || type >= RFX_HASH_MAX)
		return rfx_fail(ERR_INTERNAL, "Invalid hash algorithm");
	if (out_size < rfx_hash_hex_length(type) + 1)
		return rfx_fail(ERR_INTERNAL, "Hash output buffer too small");

	struct stat st;
	if (stat(path, &st) != 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot stat '%s': %s", path, strerror(errno));

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s': %s", path, strerror(errno));

	g_autoptr(GChecksum) cs = g_checksum_new(glib_type(type));
	if (cs == NULL) {
		close(fd);
		return rfx_fail(ERR_INTERNAL, "GChecksum does not support algorithm %d", type);
	}

	g_autofree uint8_t *buf = g_malloc(HASH_BUF);
	uint64_t total = (uint64_t)st.st_size, done = 0;
	GTimer *t = g_timer_new();
	bool ok = true;

	uprintf("Computing %s checksum of '%s':", rfx_hash_name(type), path);

	for (;;) {
		if (rfx_cancel_requested(cancel)) {
			rfx_set_error(ERR_CANCELLED, 0, "Cancelled by user");
			ok = false;
			break;
		}
		ssize_t r = read(fd, buf, HASH_BUF);
		if (r < 0) {
			if (errno == EINTR) continue;
			rfx_set_error(ERR_READ_FAULT, errno, "Read error on '%s'", path);
			ok = false;
			break;
		}
		if (r == 0) break;

		g_checksum_update(cs, buf, (gssize)r);
		done += (uint64_t)r;

		if (cb != NULL) {
			double el = g_timer_elapsed(t, NULL);
			RfxProgress p = { 0 };
			p.op = RFX_OP_VERIFY;
			p.bytes_done = done;
			p.bytes_total = total;
			p.progress = total ? (float)((double)done / (double)total) : 0;
			p.speed_bps = el > 0 ? (double)done / el : 0;
			p.eta_seconds = p.speed_bps > 0
			                ? (double)(total - done) / p.speed_bps : -1;
			g_snprintf(p.message, sizeof(p.message), "Computing %s",
			           rfx_hash_name(type));
			cb(&p, ud);
		}
		uprint_progress(done, total);
	}
	close(fd);
	g_timer_destroy(t);

	if (ok) {
		g_strlcpy(out_hex, g_checksum_get_string(cs), out_size);
		uprintf("\n%s: %s", rfx_hash_name(type), out_hex);
	} else {
		out_hex[0] = '\0';
	}
	return ok;
}

/* Rufus compares against the md5sum.txt / SHA256SUMS file shipped inside the
 * image. We support the two conventional formats:
 *   "<hex>  <filename>"     (coreutils)
 *   "<hex> *<filename>"     (binary mode)
 */
bool rfx_hash_match_sumfile(const char *sumfile, const char *target_basename,
                            RfxHashType type, const char *computed_hex);
bool rfx_hash_match_sumfile(const char *sumfile, const char *target_basename,
                            RfxHashType type, const char *computed_hex)
{
	char *content = NULL;
	if (!g_file_get_contents(sumfile, &content, NULL, NULL))
		return false;

	size_t want = rfx_hash_hex_length(type);
	bool found = false, matched = false;
	g_auto(GStrv) lines = g_strsplit(content, "\n", -1);
	for (int i = 0; lines[i] != NULL && !found; i++) {
		char *line = g_strstrip(lines[i]);
		if (strlen(line) < want + 2) continue;

		g_autofree char *hex = g_strndup(line, want);
		const char *rest = g_strstrip(line + want);
		while (*rest == ' ' || *rest == '*') rest++;

		g_autofree char *base = g_path_get_basename(rest);
		if (target_basename != NULL && g_strcmp0(base, target_basename) != 0)
			continue;

		found = true;
		matched = (g_ascii_strcasecmp(hex, computed_hex) == 0);
		uprintf("%s check against '%s': %s", rfx_hash_name(type), sumfile,
		        matched ? "OK" : "MISMATCH");
		if (!matched)
			uprintfe("  expected %s, got %s", hex, computed_hex);
	}
	g_free(content);

	if (!found)
		uprintfw("No '%s' entry for '%s' in %s", rfx_hash_name(type),
		         target_basename ? target_basename : "?", sumfile);
	return found && matched;
}
