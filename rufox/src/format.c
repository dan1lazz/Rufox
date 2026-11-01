/*
 * rufox — file system creation
 *
 * Rufus uses fmifs.dll's FormatEx() and the VDS format provider. Linux has no
 * single equivalent, so we drive the canonical userspace tools, which is also
 * what GNOME/KDE do under the hood. The cluster-size tables below are Rufus'
 * own (src/rufus.c SetFileSystemAndClusterSize()).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <unistd.h>

/* ----------------------------------------------------------------------- */
/* Names & availability                                                    */
/* ----------------------------------------------------------------------- */
static const struct {
	int         fs;
	const char *name;
	const char *tool;
	const char *pkg_hint;
} fs_table[] = {
	{ RFX_FS_FAT16, "FAT16",  "mkfs.vfat",  "dosfstools"  },
	{ RFX_FS_FAT32, "FAT32",  "mkfs.vfat",  "dosfstools"  },
	{ RFX_FS_NTFS,  "NTFS",   "mkfs.ntfs",  "ntfs-3g"     },
	{ RFX_FS_UDF,   "UDF",    "mkudffs",    "udftools"    },
	{ RFX_FS_EXFAT, "exFAT",  "mkfs.exfat", "exfatprogs"  },
	{ RFX_FS_REFS,  "ReFS",   NULL,         NULL          },
	{ RFX_FS_EXT2,  "ext2",   "mkfs.ext2",  "e2fsprogs"   },
	{ RFX_FS_EXT3,  "ext3",   "mkfs.ext3",  "e2fsprogs"   },
	{ RFX_FS_EXT4,  "ext4",   "mkfs.ext4",  "e2fsprogs"   },
};

const char* rfx_format_fs_name(RfxFs fs)
{
	for (guint i = 0; i < G_N_ELEMENTS(fs_table); i++)
		if (fs_table[i].fs == (int)fs) return fs_table[i].name;
	return "Unknown";
}

bool rfx_format_fs_supported(RfxFs fs)
{
	for (guint i = 0; i < G_N_ELEMENTS(fs_table); i++) {
		if (fs_table[i].fs != (int)fs) continue;
		if (fs_table[i].tool == NULL) return false;	/* ReFS: Windows-only */
		return rfx_tool_available(fs_table[i].tool);
	}
	return false;
}

static const char* fs_tool(RfxFs fs, const char **pkg)
{
	for (guint i = 0; i < G_N_ELEMENTS(fs_table); i++) {
		if (fs_table[i].fs == (int)fs) {
			if (pkg) *pkg = fs_table[i].pkg_hint;
			return fs_table[i].tool;
		}
	}
	if (pkg) *pkg = NULL;
	return NULL;
}

/* ----------------------------------------------------------------------- */
/* Cluster sizes — Rufus' tables, translated                               */
/* ----------------------------------------------------------------------- */
uint32_t rfx_format_default_cluster_size(RfxFs fs, uint64_t part_size)
{
	uint64_t mb = part_size / RFX_MB;

	switch (fs) {
	case RFX_FS_FAT16:
		/* Rufus: Allowed = 0x1E00 << k, Default = 16 KB << k */
		if (mb <= 64)   return 2 * RFX_KB;
		if (mb <= 128)  return 4 * RFX_KB;
		if (mb <= 256)  return 8 * RFX_KB;
		if (mb <= 512)  return 16 * RFX_KB;
		if (mb <= 1024) return 32 * RFX_KB;
		return 64 * RFX_KB;
	case RFX_FS_FAT32:
		/* Rufus: Default = 8 KB << k for small media, 32 KB above 32 GB */
		if (mb <= 64)    return 512;
		if (mb <= 128)   return 1 * RFX_KB;
		if (mb <= 256)   return 2 * RFX_KB;
		if (mb <= 512)   return 4 * RFX_KB;
		if (mb <= 1024)  return 8 * RFX_KB;
		if (mb <= 2048)  return 16 * RFX_KB;
		if (mb <= 32768) return 32 * RFX_KB;
		return 32 * RFX_KB;
	case RFX_FS_NTFS:
		if (mb <= 8)    return 512;
		if (mb <= 16)   return 1 * RFX_KB;
		if (mb <= 32)   return 2 * RFX_KB;
		if (mb <= 64)   return 4 * RFX_KB;
		if (mb <= 128)  return 8 * RFX_KB;
		if (mb <= 256)  return 16 * RFX_KB;
		if (mb <= 512)  return 32 * RFX_KB;
		return 64 * RFX_KB;
	case RFX_FS_EXFAT:
		/* Rufus: 4 KB below 256 MB, 32 KB above */
		return mb < 256 ? 4 * RFX_KB : 32 * RFX_KB;
	case RFX_FS_EXT2:
	case RFX_FS_EXT3:
	case RFX_FS_EXT4:
		return mb < 512 ? 1 * RFX_KB : (mb < 8192 ? 2 * RFX_KB : 4 * RFX_KB);
	default:
		return 0;	/* let the tool decide */
	}
}

/* Which sizes are legal to offer in the combo box (Rufus: .Allowed bitmask) */
bool rfx_format_cluster_allowed(RfxFs fs, uint32_t cluster, uint64_t part_size);
bool rfx_format_cluster_allowed(RfxFs fs, uint32_t cluster, uint64_t part_size)
{
	if (cluster == 0) return true;
	switch (fs) {
	case RFX_FS_FAT16:
		return cluster >= 512 && cluster <= 64 * RFX_KB && (cluster & (cluster - 1)) == 0;
	case RFX_FS_FAT32:
		/* FAT32 needs at least 65525 clusters, otherwise the formatter
		 * silently downgrades the volume to FAT16. The cluster count is
		 * part_size / cluster, so the cluster must be at MOST part/65525. */
		if ((cluster & (cluster - 1)) != 0) return false;	/* power of two */
		if (cluster < 512 || cluster > 32 * RFX_KB) return false;
		if (part_size != 0 && part_size / cluster < 65525) return false;
		return true;
	case RFX_FS_NTFS:
		return cluster >= 512 && cluster <= 64 * RFX_KB;
	case RFX_FS_EXFAT:
		return cluster >= 512 && cluster <= 32 * RFX_MB;
	case RFX_FS_EXT2: case RFX_FS_EXT3: case RFX_FS_EXT4:
		return cluster == 1024 || cluster == 2048 || cluster == 4096;
	default:
		return false;
	}
}

/* ----------------------------------------------------------------------- */
/* Labels (Rufus: ToValidLabel)                                            */
/* ----------------------------------------------------------------------- */
bool rfx_format_label_valid(RfxFs fs, const char *label, char *out, size_t out_size)
{
	if (label == NULL) label = "";
	switch (fs) {
	case RFX_FS_FAT16:
	case RFX_FS_FAT32: {
		/* FAT volume labels: max 11 chars, no lowercase, no . or \ / : ; etc. */
		size_t k = 0;
		for (const char *p = label; *p != '\0' && k + 1 < out_size && k < 11; p++) {
			char c = *p;
			if (c == '.' || c == '\\' || c == '/' || c == ':' || c == ';' ||
			    c == '+' || c == ',' || c == '*' || c == '?' || c == '[' ||
			    c == ']' || c == '=' || c == '"' || c == ' ')
				continue;
			out[k++] = (char)g_ascii_toupper(c);
		}
		out[k] = '\0';
		return k > 0;
	}
	case RFX_FS_NTFS:
		g_strlcpy(out, label, out_size);	/* 128 UTF-16 chars */
		return true;
	case RFX_FS_EXFAT:
		g_strlcpy(out, label, MIN(out_size, 16));
		return true;
	case RFX_FS_EXT2: case RFX_FS_EXT3: case RFX_FS_EXT4:
		g_strlcpy(out, label, MIN(out_size, 17));
		return true;
	default:
		g_strlcpy(out, label, out_size);
		return true;
	}
}

/* ----------------------------------------------------------------------- */
/* Which file systems may be offered (Rufus: SetAllowedFileSystems)        */
/* ----------------------------------------------------------------------- */
static bool has_ntfsless_grub(const RfxImgReport *img)
{
	return img != NULL && img->has_grub2 != 0 && img->has_grub4dos;
}

bool rfx_format_allowed_fs(int boot_type, int target_type, const RfxImgReport *img,
                           RfxFs *out, int *n_out)
{
	bool allowed[RFX_FS_MAX] = { false };
	int n = 0;

	switch (boot_type) {
	case RFX_BT_NON_BOOTABLE:
		for (int i = 0; i < RFX_FS_MAX; i++) allowed[i] = true;
		break;
	case RFX_BT_MSDOS:
	case RFX_BT_FREEDOS:
		allowed[RFX_FS_FAT16] = true;
		allowed[RFX_FS_FAT32] = true;
		break;
	case RFX_BT_IMAGE:
		if (img == NULL || !has_ntfsless_grub(img))
			allowed[RFX_FS_NTFS] = true;
		/* Rufus refuses FAT32 for images containing files >= 4 GB,
		 * unless NTFS is unavailable. */
		if (img != NULL && img->has_4GB_file && allowed[RFX_FS_NTFS])
			break;
		allowed[RFX_FS_FAT16] = true;
		allowed[RFX_FS_FAT32] = true;
		allowed[RFX_FS_EXFAT] = true;
		if (img != NULL && (img->uses_casper || img->has_grub2)) {
			allowed[RFX_FS_EXT2] = true;
			allowed[RFX_FS_EXT3] = true;
			allowed[RFX_FS_EXT4] = true;
		}
		break;
	case RFX_BT_GRUB2:
		allowed[RFX_FS_EXT2] = allowed[RFX_FS_EXT3] = allowed[RFX_FS_EXT4] = true;
		/* fall through */
	case RFX_BT_SYSLINUX_V6:
	case RFX_BT_GRUB4DOS:
		allowed[RFX_FS_NTFS] = true;
		/* fall through */
	case RFX_BT_SYSLINUX_V4:
	case RFX_BT_REACTOS:
		allowed[RFX_FS_FAT16] = true;
		allowed[RFX_FS_FAT32] = true;
		break;
	case RFX_BT_UEFI_NTFS:
		allowed[RFX_FS_NTFS] = true;
		allowed[RFX_FS_EXFAT] = true;
		break;
	default:
		allowed[RFX_FS_FAT32] = true;
		break;
	}

	/* UEFI without CSM cannot boot from NTFS on most firmwares unless we add
	 * the UEFI:NTFS shim; keep NTFS offered but flag it in the log. */
	if (target_type == RFX_TT_UEFI && allowed[RFX_FS_NTFS] &&
	    (img == NULL || img->has_efi == 0))
		uprintfd("NTFS + pure UEFI: the UEFI:NTFS shim will be added");

	for (int i = 0; i < RFX_FS_MAX && n < RFX_FS_MAX; i++) {
		if (!allowed[i]) continue;
		if (!rfx_format_fs_supported((RfxFs)i)) continue;
		out[n++] = (RfxFs)i;
	}
	if (n_out) *n_out = n;
	return n > 0;
}

/* ----------------------------------------------------------------------- */
/* The actual mkfs invocation                                              */
/* ----------------------------------------------------------------------- */
static void label_args(RfxFs fs, const char *label, GPtrArray *argv)
{
	switch (fs) {
	case RFX_FS_FAT16:
	case RFX_FS_FAT32:
		g_ptr_array_add(argv, g_strdup("-n"));
		g_ptr_array_add(argv, g_strdup(label));
		break;
	case RFX_FS_NTFS:
		g_ptr_array_add(argv, g_strdup("-L"));
		g_ptr_array_add(argv, g_strdup(label));
		break;
	case RFX_FS_EXFAT:
		g_ptr_array_add(argv, g_strdup("-L"));
		g_ptr_array_add(argv, g_strdup(label));
		break;
	case RFX_FS_EXT2: case RFX_FS_EXT3: case RFX_FS_EXT4:
		g_ptr_array_add(argv, g_strdup("-L"));
		g_ptr_array_add(argv, g_strdup(label));
		break;
	default:
		break;
	}
}

/* ----------------------------------------------------------------------- */
/* File targets: build the file system in a scratch file, then splice it    */
/* into the image at the partition offset.                                  */
/*                                                                         */
/* Why not just point mkfs at the image with an offset?                     */
/*   - mformat's "-i file@@offset" is actively destructive: with mtools      */
/*     4.0.32 it zeroes the first 8 KB of the *whole* file, which wipes the  */
/*     protective MBR, the GPT header and the entry array.                   */
/*   - mkfs.vfat/mkfs.exfat/mkfs.ntfs have no offset option at all, and      */
/*     pointed at the image they would reformat the entire device.           */
/* A scratch file costs nothing (it is sparse) and gives us byte-identical   */
/* output to what mkfs would have written to /dev/sdb1.                      */
/* ----------------------------------------------------------------------- */
static bool splice_file_at(const char *src, const char *dst, uint64_t dst_offset,
                           uint64_t len)
{
	int in_fd = open(src, O_RDONLY | O_CLOEXEC);
	if (in_fd < 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot reopen '%s': %s", src, strerror(errno));
	int out_fd = open(dst, O_RDWR | O_CLOEXEC);
	if (out_fd < 0) {
		close(in_fd);
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s' for writing: %s",
		                dst, strerror(errno));
	}

	/* sendfile() writes at out_fd's own position when offset is NULL, so the
	 * destination has to be positioned explicitly. Getting this wrong
	 * silently overwrites the partition table with the new file system. */
	if (lseek(out_fd, (off_t)dst_offset, SEEK_SET) != (off_t)dst_offset) {
		rfx_set_error(ERR_WRITE_FAULT, errno,
		              "Cannot seek '%s' to offset %" G_GUINT64_FORMAT, dst, dst_offset);
		close(in_fd);
		close(out_fd);
		return false;
	}

	off_t in_off = 0;
	uint64_t done = 0;
	bool ok = true;
	while (done < len) {
		ssize_t n = sendfile(out_fd, in_fd, &in_off, (size_t)MIN(len - done, (uint64_t)1 << 30));
		if (n < 0) {
			if (errno == EINTR) continue;
			if (errno == EINVAL || errno == ENOSYS) {
				/* sendfile unsupported (e.g. onto some FS): copy manually. */
				size_t chunk = (size_t)MIN((uint64_t)(4 * RFX_MB), len - done);
				g_autofree uint8_t *buf = g_malloc0(chunk);
				ssize_t got = pread(in_fd, buf, chunk, in_off);
				if (got <= 0 ||
				    !rfx_write_exact(out_fd, buf, (size_t)got, dst_offset + done)) {
					ok = false;
					break;
				}
				done += (uint64_t)got;
				in_off += got;
				continue;
			}
			ok = rfx_fail(ERR_WRITE_FAULT,
			              "sendfile failed after %" G_GUINT64_FORMAT " bytes: %s",
			              done, strerror(errno));
			break;
		}
		if (n == 0) break;		/* EOF: the source was shorter */
		done += (uint64_t)n;
	}
	rfx_sync_block(out_fd);
	close(in_fd);
	close(out_fd);
	return ok;
}

static bool format_into_scratch_and_splice(const RfxPartTarget *t, RfxFs fs,
                                          uint32_t cluster_size, const char *label,
                                          bool quick, bool extended_label)
{
	if (t->Size == 0)
		return rfx_fail(ERR_INTERNAL, "The partition size is unknown");

	GError *err = NULL;
	g_autofree char *scratch = g_strdup_printf("%s/rufox-fs-XXXXXX",
	                                           g_get_tmp_dir());
	int sfd = g_file_open_tmp("rufox-fs-XXXXXX", &scratch, &err);
	if (sfd < 0) {
		rfx_set_error(ERR_INTERNAL, 0, "Cannot create a scratch file: %s",
		              err ? err->message : "?");
		g_clear_error(&err);
		return false;
	}
	if (ftruncate(sfd, (off_t)t->Size) != 0) {
		rfx_set_error(ERR_INTERNAL, errno, "Cannot size the scratch file to %"
		              G_GUINT64_FORMAT " bytes", t->Size);
		close(sfd);
		unlink(scratch);
		return false;
	}
	close(sfd);
	uprintfd("Scratch file '%s' (%s, sparse)", scratch, "partition-sized");

	/* Run the exact same mkfs we would run against a real partition node. */
	RfxPartTarget scratch_target = *t;
	scratch_target.IsFileTarget = false;
	scratch_target.Offset = 0;
	g_strlcpy(scratch_target.PartPath, scratch, sizeof(scratch_target.PartPath));

	bool ok = rfx_format_partition_real(&scratch_target, fs, cluster_size, label,
	                                    quick, extended_label, NULL, NULL, NULL);
	if (!ok) {
		uprintfe("mkfs on the scratch file failed: %s", rfx_last_error_message());
		unlink(scratch);
		return false;
	}

	uprintf("Splicing the new file system into %s at offset %" G_GUINT64_FORMAT,
	        t->DevicePath, t->Offset);
	ok = splice_file_at(scratch, t->DevicePath, t->Offset, t->Size);
	unlink(scratch);
	if (!ok) return false;

	uprintfd("Splice complete");
	return true;
}

bool rfx_format_partition_real(const RfxPartTarget *target, RfxFs fs,
                               uint32_t cluster_size, const char *label, bool quick,
                               bool extended_label, RfxProgressCb cb, gpointer ud,
                               RfxCancel *cancel);

/* The mkfs worker: always operates on something that *is* the partition. */
bool rfx_format_partition_real(const RfxPartTarget *target, RfxFs fs,
                               uint32_t cluster_size, const char *label, bool quick,
                               bool extended_label, RfxProgressCb cb, gpointer ud,
                               RfxCancel *cancel)
{
	g_return_val_if_fail(target != NULL, false);
	(void)cb; (void)ud; (void)cancel;

	const char *pkg = NULL;
	const char *tool = fs_tool(fs, &pkg);
	if (tool == NULL)
		return rfx_fail(ERR_UNSUPPORTED_FS, "%s is not supported on Linux",
		                rfx_format_fs_name(fs));

	GStrv found = rfx_find_tool(tool, NULL);
	if (found == NULL)
		return rfx_fail(ERR_MISSING_TOOL,
		                "'%s' is required to create a %s file system. Install the '%s' package.",
		                tool, rfx_format_fs_name(fs), pkg ? pkg : tool);

	char safe_label[64] = "";
	rfx_format_label_valid(fs, label, safe_label, sizeof(safe_label));
	if (safe_label[0] == '\0')
		g_strlcpy(safe_label, "RUFOX", sizeof(safe_label));

	const char *devpart = rfx_part_tool_path(target);
	uprintf("Formatting as %s (label '%s', cluster %u) on %s:",
	        rfx_format_fs_name(fs), safe_label, cluster_size, devpart);

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(found[0]));
	g_strfreev(found);

	switch (fs) {
	case RFX_FS_FAT16:
	case RFX_FS_FAT32:
		g_ptr_array_add(argv, g_strdup("-F"));
		g_ptr_array_add(argv, g_strdup_printf("%d", fs == RFX_FS_FAT16 ? 16 : 32));
		if (cluster_size) {
			g_ptr_array_add(argv, g_strdup("-s"));
			g_ptr_array_add(argv, g_strdup_printf("%u", cluster_size / 512));
		}
		g_ptr_array_add(argv, g_strdup("-I"));	/* allow unusual media sizes */
		if (extended_label)
			g_ptr_array_add(argv, g_strdup("-i"));
		label_args(fs, safe_label, argv);
		break;
	case RFX_FS_NTFS:
		g_ptr_array_add(argv, g_strdup(quick ? "-f" : "-Q"));
		if (cluster_size) {
			g_ptr_array_add(argv, g_strdup("-c"));
			g_ptr_array_add(argv, g_strdup_printf("%u", cluster_size));
		}
		label_args(fs, safe_label, argv);
		break;
	case RFX_FS_EXFAT:
		if (cluster_size) {
			g_ptr_array_add(argv, g_strdup("-c"));
			g_ptr_array_add(argv, g_strdup_printf("%u", cluster_size));
		}
		label_args(fs, safe_label, argv);
		break;
	case RFX_FS_EXT2:
	case RFX_FS_EXT3:
	case RFX_FS_EXT4:
		if (fs == RFX_FS_EXT3) {
			g_ptr_array_add(argv, g_strdup("-t"));
			g_ptr_array_add(argv, g_strdup("ext3"));
		}
		g_ptr_array_add(argv, g_strdup("-q"));
		g_ptr_array_add(argv, g_strdup("-F"));
		if (cluster_size) {
			g_ptr_array_add(argv, g_strdup("-b"));
			g_ptr_array_add(argv, g_strdup_printf("%u", cluster_size));
		}
		label_args(fs, safe_label, argv);
		break;
	case RFX_FS_UDF:
		g_ptr_array_add(argv, g_strdup("--utf8"));
		g_ptr_array_add(argv, g_strdup("-L"));
		g_ptr_array_add(argv, g_strdup(safe_label));
		break;
	default:
		g_ptr_array_free(argv, TRUE);
		return rfx_fail(ERR_UNSUPPORTED_FS, "Unhandled file system %d", fs);
	}

	g_ptr_array_add(argv, g_strdup(devpart));
	g_ptr_array_add(argv, NULL);

	GError *err = NULL;
	RfxProcResult res = { 0 };
	bool ok = rfx_run((GStrv)argv->pdata, NULL, &res, &err);
	g_ptr_array_free(argv, TRUE);
	g_clear_error(&err);

	if (!ok) {
		/* Do not nest the same message twice: rfx_run already logged it. */
		rfx_set_error(ERR_FORMAT_FAILURE, 0, "Could not format %s as %s: %s",
		              devpart, rfx_format_fs_name(fs),
		              res.std_err && *res.std_err ? g_strstrip(res.std_err)
		              : "unknown error");
	} else if (res.std_out != NULL && *res.std_out != '\0') {
		g_auto(GStrv) lines = g_strsplit(res.std_out, "\n", -1);
		for (int i = 0; lines[i] != NULL; i++)
			if (*lines[i]) uprintfd("  %s", lines[i]);
	}

	rfx_proc_result_clear(&res);
	return ok;
}

/* ----------------------------------------------------------------------- */
/* Rufus' "extended label and icon files" — autorun.inf + drive.ico        */
/* ----------------------------------------------------------------------- */
bool rfx_format_write_extended_label(const char *mountpoint, const char *label);
bool rfx_format_write_extended_label(const char *mountpoint, const char *label)
{
	if (mountpoint == NULL) return false;
	g_autofree char *p = g_build_filename(mountpoint, "autorun.inf", NULL);
	g_autofree char *txt = g_strdup_printf(
		"[autorun]\nicon=drive.ico\nlabel=%s\n", label ? label : "RUFOX");
	return g_file_set_contents(p, txt, -1, NULL);
}

/* Free space on a mounted partition, used to size persistence sanely. */
uint64_t rfx_format_free_space(const char *mountpoint);
uint64_t rfx_format_free_space(const char *mountpoint)
{
	struct statvfs st;
	if (mountpoint == NULL || statvfs(mountpoint, &st) != 0) return 0;
	return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
}

/*
 * Public entry: pick the right strategy for the target.
 *   real device / loop node -> hand the node straight to mkfs
 *   image file              -> build it in a scratch file and splice
 */
bool rfx_format_partition(const RfxPartTarget *target, RfxFs fs,
                          uint32_t cluster_size, const char *label, bool quick,
                          bool extended_label, RfxProgressCb cb, gpointer ud,
                          RfxCancel *cancel)
{
	g_return_val_if_fail(target != NULL, false);

	if (target->IsFileTarget && target->LoopDevice[0] == '\0')
		return format_into_scratch_and_splice(target, fs, cluster_size, label,
		                                      quick, extended_label);

	return rfx_format_partition_real(target, fs, cluster_size, label, quick,
	                                 extended_label, cb, ud, cancel);
}
