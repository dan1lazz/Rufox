/*
 * rufox — misc utilities & the global error slot
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "rufox-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <linux/fs.h>
#include <pwd.h>

/* ----------------------------------------------------------------------- */
/* Error slot (Rufus' ErrorStatus + WindowsErrorString())                  */
/* ----------------------------------------------------------------------- */
/* Rufus keeps ErrorStatus in a global because its worker thread is the only
 * writer. We make it thread-local so that several jobs (or a job plus a
 * concurrent scan) cannot trample each other. */
static __thread RfxStatus tls_status;

static RfxStatus* status_get(void)
{
	return &tls_status;
}

void rfx_clear_error(void)
{
	memset(&tls_status, 0, sizeof(tls_status));
}

void rfx_set_error(RfxError code, int sys_errno, const char *fmt, ...)
{
	RfxStatus *s = status_get();
	va_list ap;
	s->code = code;
	s->sys_errno = sys_errno;
	va_start(ap, fmt);
	g_vsnprintf(s->message, sizeof(s->message), fmt, ap);
	va_end(ap);
	if (sys_errno != 0)
		uprintfe("%s (%s)", s->message, strerror(sys_errno));
	else
		uprintfe("%s", s->message);
}

RfxError rfx_last_error(void)           { return status_get()->code; }
const char* rfx_last_error_message(void){ return status_get()->message; }

bool rfx_fail(RfxError code, const char *fmt, ...)
{
	RfxStatus *s = status_get();
	va_list ap;
	s->code = code;
	s->sys_errno = errno;
	va_start(ap, fmt);
	g_vsnprintf(s->message, sizeof(s->message), fmt, ap);
	va_end(ap);
	return false;
}

const char* rfx_strerror(RfxError e)
{
	switch (e) {
	case ERR_OK:                return "Success";
	case ERR_CANCELLED:         return "Cancelled by user";
	case ERR_NO_DEVICE:         return "No such device";
	case ERR_DEVICE_BUSY:       return "Device is busy";
	case ERR_OPEN_FAILED:       return "Could not open device";
	case ERR_PERMISSION:        return "Permission denied (try running as root)";
	case ERR_NOT_ENOUGH_MEMORY: return "Not enough memory";
	case ERR_WRITE_FAULT:       return "Write error";
	case ERR_READ_FAULT:        return "Read error";
	case ERR_PARTITION_FAILURE: return "Could not partition the drive";
	case ERR_FORMAT_FAILURE:    return "Could not format the partition";
	case ERR_UNSUPPORTED_FS:    return "Unsupported file system";
	case ERR_IMAGE_TOO_LARGE:   return "Image does not fit on the target";
	case ERR_NOT_AN_IMAGE:      return "Not a recognised image file";
	case ERR_MISSING_TOOL:      return "A required external tool is missing";
	case ERR_VERIFY_MISMATCH:   return "Verification failed: data mismatch";
	case ERR_INTERNAL:          return "Internal error";
	default:                    return "Unknown error";
	}
}

/* ----------------------------------------------------------------------- */
/* Defaults — mirrors Rufus' own defaults for a UEFI target                */
/* ----------------------------------------------------------------------- */
void rfx_options_default(RfxOptions *o)
{
	g_return_if_fail(o != NULL);
	memset(o, 0, sizeof(*o));
	o->partition_scheme       = RFX_PS_GPT;
	o->target_type            = RFX_TT_UEFI;
	o->fs_type                = RFX_FS_FAT32;
	o->cluster_size           = 0;		/* auto */
	g_snprintf(o->label, sizeof(o->label), "%s", "RUFOX");
	o->quick_format           = true;
	o->create_extended_label  = false;
	o->old_bios_fixes         = false;
	o->uefi_media_validation  = false;
	o->allow_dual_uefi_bios   = false;
	o->boot_type              = RFX_BT_IMAGE;
	o->write_mode             = RFX_WRITE_AS_ISO;
	o->image_option           = RFX_IMOP_WIN_STANDARD;
	o->zero_drive             = false;
	o->fast_zeroing           = true;
	o->badblocks_mode         = RFX_BB_DISABLED;
	o->badblocks_passes       = 1;
	o->persistence_size       = 0;
	o->fix_win11_requirements = true;	/* Rufus 4.x defaults this on */
	o->no_online_account      = false;
	o->remove_edge            = false;
	o->hash_type              = RFX_HASH_NONE;
	o->verify_after_write     = false;
	o->dry_run                = false;
}

/* ----------------------------------------------------------------------- */
/* Human readable formatting                                               */
/* ----------------------------------------------------------------------- */
char* rfx_human_size(uint64_t bytes, char *buf, size_t size)
{
	static const char *u[] = { "bytes", "KB", "MB", "GB", "TB", "PB" };
	double v = (double)bytes;
	int i = 0;
	while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
	if (i == 0)
		g_snprintf(buf, size, "%" G_GUINT64_FORMAT " %s", bytes, u[0]);
	else
		g_snprintf(buf, size, "%.1f %s", v, u[i]);
	return buf;
}

char* rfx_human_rate(double bps, char *buf, size_t size)
{
	char tmp[32];
	rfx_human_size((uint64_t)bps, tmp, sizeof(tmp));
	g_snprintf(buf, size, "%s/s", tmp);
	return buf;
}

char* rfx_human_eta(double seconds, char *buf, size_t size)
{
	if (seconds < 0 || seconds > 86400.0 * 7) {
		g_snprintf(buf, size, "--:--:--");
		return buf;
	}
	uint64_t s = (uint64_t)seconds;
	g_snprintf(buf, size, "%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT
	           ":%02" G_GUINT64_FORMAT, s / 3600, (s / 60) % 60, s % 60);
	return buf;
}

uint64_t rfx_parse_size(const char *str)
{
	if (str == NULL) return 0;
	char *end = NULL;
	double v = g_strtod(str, &end);
	if (v < 0) return 0;
	if (end != NULL) {
		while (*end == ' ') end++;
		switch (g_ascii_toupper(*end)) {
		case 'K': v *= 1024.0; break;
		case 'M': v *= 1024.0 * 1024; break;
		case 'G': v *= 1024.0 * 1024 * 1024; break;
		case 'T': v *= 1024.0 * 1024 * 1024 * 1024; break;
		default: break;
		}
	}
	return (uint64_t)v;
}

/* ----------------------------------------------------------------------- */
/* Privilege handling                                                      */
/* ----------------------------------------------------------------------- */
bool rfx_is_root(void)
{
	return geteuid() == 0;
}

/*
 * Rufus runs elevated as a single process. On Linux the idiomatic equivalent
 * is either udisks2 (which does its own polkit authorisation) or pkexec.
 * This helper covers the second case and is used only for the handful of
 * operations we do through external tools.
 */
bool rfx_run_as_root_cmd(GStrv argv, char **stdout_text, GError **error)
{
	(void)stdout_text;
	if (rfx_is_root())
		return rfx_run_ok(argv, NULL, error);

	GPtrArray *a = g_ptr_array_new();
	g_ptr_array_add(a, g_strdup("pkexec"));
	for (int i = 0; argv[i] != NULL; i++)
		g_ptr_array_add(a, g_strdup(argv[i]));
	g_ptr_array_add(a, NULL);

	bool r = rfx_run_ok((GStrv)a->pdata, NULL, error);
	g_ptr_array_free(a, TRUE);
	return r;
}

/* ----------------------------------------------------------------------- */
/* Block device I/O                                                        */
/* ----------------------------------------------------------------------- */
int rfx_open_block(const char *path, bool write)
{
	int flags = write ? O_RDWR : O_RDONLY;
	flags |= O_CLOEXEC;
	/* O_NONBLOCK first: opening a floppy/CD node without media blocks for
	 * seconds otherwise. We clear it right after. */
	int fd = open(path, flags | O_NONBLOCK);
	if (fd < 0) {
		if (!write) {
			/* Some removable nodes refuse O_RDONLY|O_NONBLOCK */
			fd = open(path, flags);
			if (fd < 0) return -1;
		} else {
			return -1;
		}
	}
	int fl = fcntl(fd, F_GETFL);
	if (fl >= 0 && (fl & O_NONBLOCK))
		fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	return fd;
}

bool rfx_sync_block(int fd)
{
	if (fd < 0) return false;
	/* fsync() is what actually flushes the device's volatile write cache.
	 * syncfs() only means something for a filesystem fd; on a raw block
	 * device it is a harmless no-op, kept for the mounted-target case. */
	syncfs(fd);
	if (fsync(fd) != 0) {
		if (errno == EINVAL)
			return true;	/* this fd does not support fsync — not our fault */
		/* EIO here means the data never reached the media. Reporting
		 * success would hand the user a corrupt, non-bootable stick
		 * (docs/REVIEW.md H4). */
		return false;
	}
	return true;
}

bool rfx_read_exact(int fd, void *buf, size_t len, uint64_t offset)
{
	uint8_t *p = buf;
	size_t done = 0;
	while (done < len) {
		ssize_t r = pread(fd, p + done, len - done, (off_t)(offset + done));
		if (r < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (r == 0) break;	/* short read at EOF is allowed */
		done += (size_t)r;
	}
	return done == len;
}

bool rfx_write_exact(int fd, const void *buf, size_t len, uint64_t offset)
{
	const uint8_t *p = buf;
	size_t done = 0;
	while (done < len) {
		ssize_t w = pwrite(fd, p + done, len - done, (off_t)(offset + done));
		if (w < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (w == 0) return false;
		done += (size_t)w;
	}
	return true;
}

bool rfx_path_is_block_device(const char *path)
{
	struct stat st;
	if (path == NULL || stat(path, &st) != 0) return false;
	return S_ISBLK(st.st_mode);
}

char* rfx_resolve_device(const char *name)
{
	if (name == NULL) return NULL;
	if (g_str_has_prefix(name, "/dev/"))
		return g_strdup(name);
	if (g_file_test(name, G_FILE_TEST_EXISTS))
		return g_canonicalize_filename(name, NULL);
	return g_strdup_printf("/dev/%s", name);
}

/* ----------------------------------------------------------------------- */
/* External tool lookup                                                    */
/* ----------------------------------------------------------------------- */
GStrv rfx_find_tool(const char *name, const char *const *extra_paths)
{
	GPtrArray *dirs = g_ptr_array_new();
	static const char *const sys_dirs[] = {
		"/usr/sbin", "/sbin", "/usr/local/sbin",
		"/usr/bin", "/bin", "/usr/local/bin", NULL
	};
	const char *env_path = g_getenv("PATH");
	if (env_path != NULL) {
		g_auto(GStrv) parts = g_strsplit(env_path, ":", -1);
		for (int i = 0; parts[i] != NULL; i++)
			g_ptr_array_add(dirs, (gpointer)parts[i]);
	}
	for (int i = 0; sys_dirs[i] != NULL; i++)
		g_ptr_array_add(dirs, (gpointer)sys_dirs[i]);
	if (extra_paths != NULL)
		for (int i = 0; extra_paths[i] != NULL; i++)
			g_ptr_array_add(dirs, (gpointer)extra_paths[i]);

	char *found = NULL;
	for (guint i = 0; i < dirs->len && found == NULL; i++) {
		char *p = g_build_filename((const char*)g_ptr_array_index(dirs, i),
		                           name, NULL);
		if (g_file_test(p, G_FILE_TEST_EXISTS) && g_file_test(p, G_FILE_TEST_IS_EXECUTABLE))
			found = p;
		else
			g_free(p);
	}
	g_ptr_array_free(dirs, TRUE);
	if (found == NULL)
		return NULL;

	GStrv out = g_new0(char*, 2);
	out[0] = found;		/* transfers ownership */
	return out;
}

bool rfx_tool_available(const char *name)
{
	GStrv t = rfx_find_tool(name, NULL);
	if (t == NULL) return false;
	g_strfreev(t);
	return true;
}
