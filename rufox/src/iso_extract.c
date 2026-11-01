/*
 * rufox — ISO extraction
 *
 * Port of Rufus' ExtractISO()/iso_extract_files()/fix_config(). The config
 * patching is the part that actually matters: without it an extracted
 * Ubuntu/Mint/Debian/Fedora tree will not boot from USB, because the kernel
 * command line still points at the ISO volume label instead of the stick's.
 *
 * Data is read straight from the image file at (lsn * 2048). ISO9660 extents
 * are contiguous for anything mkisofs/xorriso/genisoimage produces; anything
 * else falls back to libcdio's own reassembly.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <utime.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cdio/cdio.h>
#include <cdio/ds.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>
#include <cdio/udf_file.h>
#include <cdio/rock.h>

#define ISO_BLOCKSIZE 2048
#define EXTRACT_BUF   (256 * 1024)
#define ISO_EXT_MASK  (ISO_EXTENSION_ROCK_RIDGE | ISO_EXTENSION_JOLIET)

/* ----------------------------------------------------------------------- */
/* Filename sanitisation (Rufus: sanitize_filename)                        */
/* ----------------------------------------------------------------------- */
static char* sanitize_filename(const char *in, bool *identical)
{
	/* Windows forbids \ / : * ? " < > | in a name. Rufus swaps them for
	 * full-width lookalikes so the file still extracts and stays findable,
	 * which also keeps FAT targets happy. */
	static const char bad[] = "\\/:*?\"<>|";
	static const char *const rep[] = {
		"\xEF\xBC\xBC", "\xEF\xBC\x8F", "\xEF\xBC\x9A", "\xEF\xBC\x8A",
		"\xEF\xBC\x9F", "\xEF\xBC\x82", "\xEF\xBC\x9C", "\xEF\xBC\x9E",
		"\xEF\xBD\x9C"
	};
	GString *out = g_string_new(in);
	bool changed = false;

	for (size_t i = 0; i < out->len; i++) {
		const char *hit = strchr(bad, out->str[i]);
		if (hit == NULL) continue;
		g_string_erase(out, (gssize)i, 1);
		g_string_insert(out, (gssize)i, rep[hit - bad]);
		i += 2;
		changed = true;
	}
	/* Trailing dots and spaces cannot be represented on FAT. */
	while (out->len > 0 &&
	       (out->str[out->len - 1] == '.' || out->str[out->len - 1] == ' '))
		g_string_truncate(out, out->len - 1);
	if (out->len == 0)
		g_string_assign(out, "_");

	if (identical) *identical = !changed;
	return g_string_free(out, FALSE);
}

/* ----------------------------------------------------------------------- */
/* Token-aware config patching (Rufus: replace_in_token_data)              */
/* ----------------------------------------------------------------------- */
static bool token_is(const char *p, const char *token)
{
	size_t n = strlen(token);
	if (strncmp(p, token, n) != 0) return false;
	char c = p[n];
	return c == ' ' || c == '\t' || c == '\0' || c == '\r' || c == '\n' || c == '=';
}

/*
 * Replace every occurrence of `what` with `with` after the command word
 * `token` on the same line. GString keeps the grow case trivial.
 */
static bool replace_in_token_data(GString *data, const char *token,
                                  const char *what, const char *with)
{
	g_return_val_if_fail(data != NULL && token != NULL && what != NULL, false);
	size_t what_len = strlen(what);
	if (what_len == 0) return false;
	size_t with_len = with != NULL ? strlen(with) : 0;
	bool modified = false;

	size_t line_start = 0;
	while (line_start <= data->len) {
		size_t line_end = line_start;
		while (line_end < data->len &&
		       data->str[line_end] != '\n' && data->str[line_end] != '\r')
			line_end++;

		/* Locate the command word at the head of this line. */
		size_t p = line_start;
		bool have_token = false;
		while (p < line_end) {
			while (p < line_end && (data->str[p] == ' ' || data->str[p] == '\t')) p++;
			if (p >= line_end) break;
			if (token_is(data->str + p, token)) { have_token = true; break; }
			while (p < line_end && data->str[p] != ' ' && data->str[p] != '\t') p++;
		}

		if (have_token) {
			/* Offsets, never pointers: GString may reallocate under us. */
			size_t search = p + strlen(token);
			while (search + what_len <= line_end) {
				if (memcmp(data->str + search, what, what_len) != 0) {
					search++;
					continue;
				}
				g_string_erase(data, (gssize)search, (gssize)what_len);
				if (with_len > 0)
					g_string_insert_len(data, (gssize)search, with, (gssize)with_len);
				modified = true;
				ssize_t delta = (ssize_t)with_len - (ssize_t)what_len;
				line_end = (size_t)((ssize_t)line_end + delta);
				search  += with_len > 0 ? with_len : 1;
			}
		}

		if (line_end >= data->len) break;
		line_start = line_end + 1;
		if (line_start < data->len && data->str[line_start - 1] == '\r' &&
		    data->str[line_start] == '\n')
			line_start++;
	}
	return modified;
}

/* ----------------------------------------------------------------------- */
/* fix_config()                                                            */
/* ----------------------------------------------------------------------- */
typedef struct {
	bool is_cfg;
	bool is_syslinux_cfg;
	bool is_grub_cfg;
	bool is_conf;
} ExtractProps;

static bool name_ends_ci(const char *s, const char *suf)
{
	size_t ls = strlen(s), lf = strlen(suf);
	return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

static void classify(const char *name, ExtractProps *p)
{
	memset(p, 0, sizeof(*p));
	p->is_cfg  = name_ends_ci(name, ".cfg");
	p->is_conf = name_ends_ci(name, ".conf");
	p->is_syslinux_cfg = (strcasecmp(name, "isolinux.cfg") == 0 ||
	                      strcasecmp(name, "syslinux.cfg") == 0 ||
	                      strcasecmp(name, "extlinux.conf") == 0 ||
	                      strcasecmp(name, "live.cfg") == 0);
	p->is_grub_cfg = (strcasecmp(name, "grub.cfg") == 0 ||
	                  strcasecmp(name, "loopback.cfg") == 0 ||
	                  strcasecmp(name, "menu.cfg") == 0);
}

static void fix_config(const char *fullpath, const char *basename,
                       ExtractProps *props, const RfxImgReport *rep,
                       const RfxOptions *opt, RfxImgReport *mut)
{
	char *content = NULL;
	gsize len = 0;
	if (!g_file_get_contents(fullpath, &content, &len, NULL) || len == 0) {
		g_free(content);
		return;
	}

	GString *buf = g_string_new_len(content, (gssize)len);
	g_free(content);
	bool modified = false;

	/* 1. Persistence keyword in the kernel command line. Rufus tries five
	 *    dialects in this exact order; Mint and Ubuntu 23.04+ each need a
	 *    different one, and the order matters because Mint is inconsistent. */
	if (opt != NULL && opt->persistence_size > 0 && rep->uses_casper &&
	    (props->is_grub_cfg || props->is_syslinux_cfg || props->is_cfg)) {
		const char *ktok = props->is_grub_cfg ? "linux" : "append";
		if (replace_in_token_data(buf, ktok, "file=/cdrom/preseed",
		                          "persistent file=/cdrom/preseed")) {
			uprintf("  Added 'persistent' kernel option");
			modified = true;
			if (props->is_grub_cfg &&
			    replace_in_token_data(buf, "linux", "maybe-ubiquity", ""))
				uprintf("  Removed 'maybe-ubiquity' kernel option");
		} else if (replace_in_token_data(buf, ktok, "boot=casper", "boot=casper persistent")) {
			uprintf("  Added 'persistent' kernel option");
			modified = true;
		} else if (replace_in_token_data(buf, "linux", "/casper/vmlinuz",
		                                 "/casper/vmlinuz persistent")) {
			uprintf("  Added 'persistent' kernel option");
			modified = true;
		} else if (replace_in_token_data(buf, "kernel", "/casper/vmlinuz",
		                                 "/casper/vmlinuz persistent")) {
			uprintf("  Added 'persistent' kernel option");
			modified = true;
		} else if (replace_in_token_data(buf, ktok, "boot=live", "boot=live persistence")) {
			uprintf("  Added 'persistence' kernel option");
			modified = true;
		}
	}

	/* 2. Volume label rewrite: the extracted tree is no longer "the ISO".
	 *    Rufus escapes spaces as \x20, which is what the kernel wants. */
	if (props->is_cfg || props->is_conf) {
		static const char *cfg_token[] = {
			"options", "append", "linux", "linuxefi", "$linux", "search", "for"
		};
		g_auto(GStrv) iso_parts = g_strsplit(rep->label, " ", -1);
		g_auto(GStrv) usb_parts = g_strsplit(rep->usb_label, " ", -1);
		g_autofree char *iso_esc = g_strjoinv("\\x20", iso_parts);
		g_autofree char *usb_esc = g_strjoinv("\\x20", usb_parts);

		if (iso_esc != NULL && usb_esc != NULL && *iso_esc != '\0' &&
		    strcmp(iso_esc, usb_esc) != 0) {
			bool patched = false;
			for (guint i = 0; i < G_N_ELEMENTS(cfg_token); i++)
				if (replace_in_token_data(buf, cfg_token[i], iso_esc, usb_esc))
					patched = true;
			if (patched) {
				uprintf("  Patched %s: '%s' -> '%s'", basename, iso_esc, usb_esc);
				modified = true;
			}
		}

		/* RHEL 8+ derivatives: anaconda stopped accepting inst.stage2.
		 * Rufus skips this for netinst media, which we detect the same way. */
		if (rep->rh8_derivative && strstr(rep->ImagePath, "netinst") == NULL) {
			bool patched = false;
			for (guint i = 0; i < G_N_ELEMENTS(cfg_token); i++)
				if (replace_in_token_data(buf, cfg_token[i], "inst.stage2", "inst.repo"))
					patched = true;
			if (patched) {
				uprintf("  Patched %s: 'inst.stage2' -> 'inst.repo'", basename);
				modified = true;
			}
		}

		/* FreeNAS wants an msdosfs: path instead of cd9660: */
		if (props->is_grub_cfg) {
			g_autofree char *f_iso = g_strdup_printf("cd9660:/dev/iso9660/%s", rep->label);
			g_autofree char *f_usb = g_strdup_printf("msdosfs:/dev/msdosfs/%s", rep->usb_label);
			if (replace_in_token_data(buf, "set", f_iso, f_usb)) {
				uprintf("  Patched %s: '%s' -> '%s'", basename, f_iso, f_usb);
				modified = true;
			}
		}
	}

	if (modified) {
		if (!g_file_set_contents(fullpath, buf->str, -1, NULL))
			uprintfw("Could not save the patched '%s'", fullpath);
		else if (mut != NULL)
			mut->patched_files++;
	}
	g_string_free(buf, TRUE);
}

/* ----------------------------------------------------------------------- */
/* Extraction context                                                      */
/* ----------------------------------------------------------------------- */
typedef struct {
	iso9660_t          *iso;
	int                 iso_fd;		/* fast raw read path */
	const char         *dest;
	const RfxImgReport *rep;
	RfxImgReport       *mut;
	const RfxOptions   *opt;
	RfxProgressCb       cb;
	gpointer            ud;
	RfxCancel          *cancel;
	uint64_t            total;
	uint64_t            done;
	uint32_t            files;
	uint32_t            dirs;
	uint8_t            *buf;
	size_t              buf_size;
	bool                failed;
	GTimer             *timer;
	const char         *current_file;
} ExtractCtx;

static void report_progress(ExtractCtx *c)
{
	if (c->cb == NULL) return;
	double el = g_timer_elapsed(c->timer, NULL);
	uint64_t total = c->total ? c->total : 1;
	RfxProgress p = { 0 };
	p.op          = RFX_OP_FILE_COPY;
	p.bytes_done  = c->done;
	p.bytes_total = c->total;
	p.progress    = (float)((double)c->done / (double)total);
	p.speed_bps   = el > 0 ? (double)c->done / el : 0;
	p.eta_seconds = p.speed_bps > 0 ? (double)(c->total - c->done) / p.speed_bps : -1;
	g_snprintf(p.message, sizeof(p.message), "Copying %s",
	           c->current_file ? c->current_file : "files");
	c->cb(&p, c->ud);
}

static bool ensure_dir(const char *path)
{
	if (g_file_test(path, G_FILE_TEST_IS_DIR)) return true;
	if (g_mkdir_with_parents(path, 0755) == 0) return true;
	uprintfe("Could not create directory '%s': %s", path, strerror(errno));
	return false;
}

static void set_mtime(const char *path, const struct tm *tm_iso)
{
	if (tm_iso == NULL) return;
	struct tm copy = *tm_iso;
	time_t t = mktime(&copy);
	if (t <= 0) return;
	struct utimbuf ut = { .actime = t, .modtime = t };
	utime(path, &ut);
}

static bool raw_read(ExtractCtx *c, lsn_t lsn, void *out, uint32_t size)
{
	if (c->iso_fd < 0) return false;
	size_t done = 0;
	while (done < size) {
		ssize_t r = pread(c->iso_fd, (uint8_t*)out + done, size - done,
		                  (off_t)((uint64_t)lsn * ISO_BLOCKSIZE + done));
		if (r <= 0) return false;
		done += (size_t)r;
	}
	return true;
}

static bool extract_one_file(ExtractCtx *c, const iso9660_stat_t *st,
                             const char *dest_path)
{
	FILE *out = fopen(dest_path, "wb");
	if (out == NULL) {
		uprintfe("Could not create '%s': %s", dest_path, strerror(errno));
		c->failed = true;
		return false;
	}

	uint32_t remaining = st->size;
	uint32_t block_off = 0;
	lsn_t lsn = st->lsn;
	bool ok = true;

	while (remaining > 0 && ok) {
		if (rfx_cancel_requested(c->cancel)) {
			rfx_set_error(ERR_CANCELLED, 0, "Cancelled by user");
			ok = false;
			break;
		}
		uint32_t chunk = (uint32_t)MIN((size_t)remaining, c->buf_size);
		lsn_t here = lsn + (lsn_t)(block_off / ISO_BLOCKSIZE);

		if (!raw_read(c, here, c->buf, chunk)) {
			long got = iso9660_iso_seek_read(c->iso, c->buf, here,
			                                 (long)((chunk + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE));
			if (got < (long)chunk) {
				uprintfe("Read error on '%s' at offset %u", dest_path, block_off);
				ok = false;
				break;
			}
		}
		if (fwrite(c->buf, 1, chunk, out) != chunk) {
			uprintfe("Write error on '%s': %s", dest_path, strerror(errno));
			ok = false;
			break;
		}
		block_off += chunk;
		remaining -= chunk;
		c->done += chunk;
	}
	fclose(out);
	if (!ok) { c->failed = true; return false; }

	set_mtime(dest_path, &st->tm);
	c->files++;
	return true;
}

static void walk_extract(ExtractCtx *c, const char *iso_dir, const char *dest_dir, int depth)
{
	if (c->failed || depth > 40) return;

	CdioList_t *list = iso9660_ifs_readdir(c->iso, iso_dir);
	if (list == NULL) return;

	for (CdioListNode_t *node = _cdio_list_begin(list); node != NULL;
	     node = _cdio_list_node_next(node)) {
		if (rfx_cancel_requested(c->cancel)) { c->failed = true; break; }

		iso9660_stat_t *st = (iso9660_stat_t*)_cdio_list_node_data(node);
		if (st == NULL) continue;
		if (st->filename[0] == '\0' ||
		    strcasecmp(st->filename, ".") == 0 || strcasecmp(st->filename, "..") == 0)
			continue;

		bool identical = false;
		g_autofree char *safe = sanitize_filename(st->filename, &identical);
		if (!identical)
			uprintfd("Sanitised '%s' -> '%s'", st->filename, safe);

		g_autofree char *iso_path = g_strdup_printf("%s/%s", iso_dir, st->filename);
		if (iso_path[0] == '/' && iso_path[1] == '/')
			memmove(iso_path, iso_path + 1, strlen(iso_path));
		g_autofree char *dest_path = g_build_filename(dest_dir, safe, NULL);

		/* Rufus refuses to let the ISO's own ldlinux.sys clobber ours. */
		if (strcmp(iso_dir, "/") == 0 && strcasecmp(safe, "ldlinux.sys") == 0) {
			uprintf("Skipping '%s' file from ISO image", safe);
			continue;
		}

		if (st->type == _STAT_DIR) {
			c->dirs++;
			if (!ensure_dir(dest_path)) { c->failed = true; break; }
			set_mtime(dest_path, &st->tm);
			walk_extract(c, iso_path, dest_path, depth + 1);
			continue;
		}

		if (st->rr.psz_symlink != NULL && st->size == 0) {
			if (symlink(st->rr.psz_symlink, dest_path) != 0 && errno != EEXIST)
				uprintfw("Could not create symlink '%s': %s",
				         dest_path, strerror(errno));
			continue;
		}

		c->current_file = safe;
		if (!extract_one_file(c, st, dest_path))
			break;

		ExtractProps props;
		classify(safe, &props);
		if (props.is_cfg || props.is_conf)
			fix_config(dest_path, safe, &props, c->rep, c->opt, c->mut);

		report_progress(c);
		uprint_progress(c->done, c->total);
	}
	_cdio_list_free(list, true, (CdioDataFree_t)iso9660_stat_free);
}

/* ----------------------------------------------------------------------- */
/* Public API                                                              */
/* ----------------------------------------------------------------------- */
bool rfx_iso_extract(const char *iso, const char *dest, RfxImgReport *rep,
                     const RfxOptions *opt, RfxProgressCb cb, gpointer ud,
                     RfxCancel *cancel)
{
	g_return_val_if_fail(iso != NULL && dest != NULL && rep != NULL, false);

	if (!ensure_dir(dest)) return false;

	iso9660_t *ifs = iso9660_open_ext(iso, ISO_EXT_MASK);
	if (ifs == NULL)
		return rfx_fail(ERR_NOT_AN_IMAGE, "Cannot open '%s' as ISO9660", iso);

	int fd = open(iso, O_RDONLY | O_CLOEXEC);

	ExtractCtx c = { 0 };
	c.iso = ifs;
	c.iso_fd = fd;
	c.dest = dest;
	c.rep = rep;
	c.mut = rep;
	c.opt = opt;
	c.cb = cb;
	c.ud = ud;
	c.cancel = cancel;
	c.total = rep->projected_size ? rep->projected_size : rep->image_size;
	c.buf_size = EXTRACT_BUF;
	c.buf = g_malloc(c.buf_size);
	c.timer = g_timer_new();

	uprintf("Extracting files to '%s':", dest);
	walk_extract(&c, "/", dest, 0);

	g_timer_destroy(c.timer);
	g_free(c.buf);
	if (fd >= 0) close(fd);
	iso9660_close(ifs);

	if (rfx_cancel_requested(cancel))
		return rfx_fail(ERR_CANCELLED, "Cancelled by user");
	if (c.failed)
		return false;

	rep->file_count = c.files;
	rep->dir_count  = c.dirs;
	uprintf("\nExtracted %u file(s) into %u director%s", c.files, c.dirs,
	        c.dirs == 1 ? "y" : "ies");
	if (rep->patched_files > 0)
		uprintf("Patched %u config file(s)", rep->patched_files);
	return true;
}

/* Rufus: ExtractISOFile() */
bool rfx_iso_extract_file(const char *iso, const char *iso_file, const char *dest_file)
{
	iso9660_t *ifs = iso9660_open_ext(iso, ISO_EXT_MASK);
	if (ifs == NULL) return false;

	iso9660_stat_t *st = iso9660_ifs_stat_translate(ifs, iso_file);
	if (st == NULL) { iso9660_close(ifs); return false; }

	FILE *out = fopen(dest_file, "wb");
	if (out == NULL) { iso9660_stat_free(st); iso9660_close(ifs); return false; }

	int fd = open(iso, O_RDONLY | O_CLOEXEC);
	size_t buf_size = 64 * 1024;
	g_autofree uint8_t *buf = g_malloc(buf_size);
	uint32_t remaining = st->size, off = 0;
	lsn_t lsn = st->lsn;
	bool ok = true;

	while (remaining > 0) {
		uint32_t chunk = (uint32_t)MIN((size_t)remaining, buf_size);
		bool got_it = false;
		if (fd >= 0)
			got_it = rfx_read_exact(fd, buf, chunk,
			                        (uint64_t)lsn * ISO_BLOCKSIZE + off);
		if (!got_it) {
			long got = iso9660_iso_seek_read(ifs, buf, lsn + (lsn_t)(off / ISO_BLOCKSIZE),
			                                 (long)((chunk + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE));
			if (got < (long)chunk) { ok = false; break; }
		}
		if (fwrite(buf, 1, chunk, out) != chunk) { ok = false; break; }
		off += chunk;
		remaining -= chunk;
	}
	fclose(out);
	if (fd >= 0) close(fd);
	iso9660_stat_free(st);
	iso9660_close(ifs);
	return ok;
}

/* Rufus: ReadISOFileToBuffer() */
bool rfx_iso_read_file_to_buffer(const char *iso, const char *iso_file,
                                 uint8_t **buf, uint32_t *size)
{
	g_return_val_if_fail(buf != NULL && size != NULL, false);
	*buf = NULL;
	*size = 0;

	iso9660_t *ifs = iso9660_open_ext(iso, ISO_EXT_MASK);
	if (ifs == NULL) return false;

	iso9660_stat_t *st = iso9660_ifs_stat_translate(ifs, iso_file);
	if (st == NULL) { iso9660_close(ifs); return false; }

	uint32_t len = st->size;
	uint8_t *b = g_malloc(len + 1);
	long got = iso9660_iso_seek_read(ifs, b, st->lsn,
	                                 (long)((len + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE));
	iso9660_stat_free(st);
	iso9660_close(ifs);
	if (got < 0) { g_free(b); return false; }

	b[MIN((size_t)got, len)] = '\0';
	*buf = b;
	*size = len;
	return true;
}
