/*
 * rufox — partition targets
 *
 * On a real disk, a partition is a device node (/dev/sdb1) that the kernel
 * created for us after BLKRRPART. On a regular file there is no kernel
 * partitioning at all, so we address partitions by (offset, size) inside the
 * file instead. Rufus has the same split for VHD targets, where it hands
 * FormatEx() a \\.\PhysicalDriveN handle plus a StartingOffset rather than a
 * volume path.
 *
 * This module is the single place that knows the difference, so that
 * format.c, job.c and persistence.c can stay target-agnostic.
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
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* ----------------------------------------------------------------------- */
/* Resolution                                                              */
/* ----------------------------------------------------------------------- */
static bool is_regular_file(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

RfxPartTarget rfx_part_resolve(const char *devpath, int part_index,
                               uint64_t offset, uint64_t size)
{
	RfxPartTarget t = { 0 };
	g_strlcpy(t.DevicePath, devpath, sizeof(t.DevicePath));
	t.Index = part_index;
	t.Offset = offset;
	t.Size = size;
	t.IsFileTarget = is_regular_file(devpath);

	if (t.IsFileTarget) {
		/* No node to name; the offset *is* the identity. */
		g_snprintf(t.PartPath, sizeof(t.PartPath), "%s@%" G_GUINT64_FORMAT,
		           devpath, offset);
	} else if (part_index <= 0) {
		g_strlcpy(t.PartPath, devpath, sizeof(t.PartPath));
	} else {
		const char *base = strrchr(devpath, '/');
		base = base ? base + 1 : devpath;
		if (g_str_has_prefix(base, "nvme") || g_str_has_prefix(base, "mmcblk") ||
		    g_str_has_prefix(base, "loop") || g_str_has_prefix(base, "md") ||
		    g_str_has_prefix(base, "nbd"))
			g_snprintf(t.PartPath, sizeof(t.PartPath), "%sp%d", devpath, part_index);
		else
			g_snprintf(t.PartPath, sizeof(t.PartPath), "%s%d", devpath, part_index);
	}
	return t;
}

int rfx_part_open(const RfxPartTarget *t, bool write)
{
	g_return_val_if_fail(t != NULL, -1);

	if (!t->IsFileTarget)
		return rfx_open_block(t->PartPath, write);

	int fd = open(t->DevicePath, write ? (O_RDWR | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC));
	if (fd < 0) {
		rfx_set_error(ERR_OPEN_FAILED, errno, "Cannot open '%s'", t->DevicePath);
		return -1;
	}
	/* Handing back the whole file with an implied offset is dangerous, so
	 * callers that want raw I/O must use rfx_part_pread/pwrite. Mark it. */
	return fd;
}

bool rfx_part_pread(const RfxPartTarget *t, int fd, void *buf, size_t len, uint64_t off)
{
	uint64_t abs_off = t->Offset + off;
	if (t->Size != 0 && off + len > t->Size)
		return false;
	return rfx_read_exact(fd, buf, len, abs_off);
}

bool rfx_part_pwrite(const RfxPartTarget *t, int fd, const void *buf, size_t len, uint64_t off)
{
	uint64_t abs_off = t->Offset + off;
	if (t->Size != 0 && off + len > t->Size)
		return rfx_fail(ERR_WRITE_FAULT,
		                "Write of %zu bytes at %" G_GUINT64_FORMAT " overruns the partition",
		                len, off);
	return rfx_write_exact(fd, buf, len, abs_off);
}

bool rfx_part_zero(const RfxPartTarget *t, int fd, uint64_t len)
{
	if (len == 0) return true;
	size_t chunk = 256 * 1024;
	g_autofree uint8_t *zeros = g_malloc0(chunk);
	uint64_t done = 0;
	while (done < len) {
		size_t n = (size_t)MIN(chunk, len - done);
		if (!rfx_part_pwrite(t, fd, zeros, n, done))
			return false;
		done += n;
	}
	return true;
}

/* ----------------------------------------------------------------------- */
/* Mounting                                                                */
/* ----------------------------------------------------------------------- */
bool rfx_part_mount(RfxPartTarget *t, const char *where, const char *fstype,
                    bool *used_loop)
{
	g_return_val_if_fail(t != NULL && where != NULL, false);
	if (used_loop) *used_loop = false;

	if (g_mkdir_with_parents(where, 0755) != 0) {
		rfx_set_error(ERR_INTERNAL, errno, "Cannot create mountpoint '%s'", where);
		return false;
	}

	if (!t->IsFileTarget) {
		if (mount(t->PartPath, where, fstype, MS_NOATIME, NULL) == 0)
			return true;
		if (mount(t->PartPath, where, NULL, MS_NOATIME, NULL) == 0)
			return true;
		rfx_set_error(ERR_OPEN_FAILED, errno, "Cannot mount %s on %s",
		              t->PartPath, where);
		return false;
	}

	/* File target: loop devices are the only way to get a mountable node. */
	GStrv lo = rfx_find_tool("losetup", NULL);
	if (lo == NULL) {
		rfx_set_error(ERR_MISSING_TOOL, 0,
		              "Mounting a partition inside an image file needs losetup(8), "
		              "which is not installed. Use mtools-based access instead, or "
		              "test against a real device.");
		return false;
	}

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(lo[0]));
	g_strfreev(lo);
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("--show"));
	g_ptr_array_add(argv, g_strdup("-o"));
	g_ptr_array_add(argv, g_strdup_printf("%" G_GUINT64_FORMAT, t->Offset));
	if (t->Size != 0) {
		g_ptr_array_add(argv, g_strdup("--sizelimit"));
		g_ptr_array_add(argv, g_strdup_printf("%" G_GUINT64_FORMAT, t->Size));
	}
	g_ptr_array_add(argv, g_strdup(t->DevicePath));
	g_ptr_array_add(argv, NULL);

	RfxProcResult res = { 0 };
	GError *err = NULL;
	bool ok = rfx_run((GStrv)argv->pdata, NULL, &res, &err);
	g_ptr_array_free(argv, TRUE);
	g_clear_error(&err);

	if (!ok || res.std_out == NULL || *res.std_out == '\0') {
		rfx_set_error(ERR_OPEN_FAILED, 0, "losetup failed: %s",
		              res.std_err ? res.std_err : "no loop device available");
		rfx_proc_result_clear(&res);
		return false;
	}

	char *loopdev = g_strstrip(res.std_out);	/* takes ownership */
	rfx_proc_result_clear(&res);
	g_strlcpy(t->LoopDevice, loopdev, sizeof(t->LoopDevice));
	g_free(loopdev);
	uprintfd("Attached %s+%llu -> %s", t->DevicePath, (unsigned long long)t->Offset,
	         t->LoopDevice);

	if (mount(t->LoopDevice, where, fstype, MS_NOATIME, NULL) == 0 ||
	    mount(t->LoopDevice, where, NULL, MS_NOATIME, NULL) == 0) {
		if (used_loop) *used_loop = true;
		return true;
	}

	rfx_set_error(ERR_OPEN_FAILED, errno, "Cannot mount %s on %s", t->LoopDevice, where);
	char *d[] = { (char*)"losetup", (char*)"-d", (char*)t->LoopDevice, NULL };
	rfx_run_ok(d, NULL, NULL);
	t->LoopDevice[0] = '\0';
	return false;
}

bool rfx_part_unmount(RfxPartTarget *t, const char *where)
{
	bool ok = true;
	if (umount(where) != 0 && umount2(where, MNT_DETACH) != 0) {
		uprintfw("Could not unmount %s: %s", where, strerror(errno));
		ok = false;
	}
	if (t != NULL && t->LoopDevice[0] != '\0') {
		char *d[] = { (char*)"losetup", (char*)"-d", (char*)t->LoopDevice, NULL };
		if (!rfx_run_ok(d, NULL, NULL))
			uprintfw("Could not detach %s", t->LoopDevice);
		t->LoopDevice[0] = '\0';
	}
	return ok;
}

/* The path that filesystem tools should be pointed at: a real node when we
 * have one, otherwise the loop device we set up. */
const char* rfx_part_tool_path(const RfxPartTarget *t)
{
	if (t == NULL) return NULL;
	if (!t->IsFileTarget) return t->PartPath;
	return t->LoopDevice[0] != '\0' ? t->LoopDevice : t->DevicePath;
}
