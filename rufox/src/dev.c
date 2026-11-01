/*
 * rufox — device enumeration & filtering
 *
 * Rufus uses SetupAPI (GUID_DEVINTERFACE_DISK) + IOCTL_STORAGE_QUERY_PROPERTY +
 * IOCTL_DISK_GET_DRIVE_GEOMETRY_EX + hdd_vs_ufd.h to build its drive list.
 * None of that exists on Linux, so this is a full re-implementation on top of
 * sysfs, libblkid and /proc/mounts. The *semantics* we reproduce are:
 *
 *   - only real, whole, writable disks are offered
 *   - the disk the running system booted from is filtered out (IsFilteredDrive)
 *   - optical media, floppy, device-mapper/loop/LVM/MD members are hidden
 *   - removable USB / SD media are preferred and shown first
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "rufox-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mntent.h>
#include <sys/mount.h>
#include <stdarg.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <blkid/blkid.h>

#ifdef HAVE_GUDEV
#include <gudev/gudev.h>
#endif

struct _RfxDevList {
	GPtrArray *drives;		/* RfxDrive*, owned */
	GHashTable *filtered_reasons;
#ifdef HAVE_GUDEV
	GUdevClient *udev;
#endif
};

/* ----------------------------------------------------------------------- */
/* small sysfs helpers                                                     */
/* ----------------------------------------------------------------------- */
static char* sysfs_read(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static char* sysfs_read(const char *fmt, ...)
{
	va_list ap;
	char path[512];
	va_start(ap, fmt);
	g_vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);

	char *content = NULL;
	if (!g_file_get_contents(path, &content, NULL, NULL))
		return NULL;
	g_strchomp(content);
	if (*content == '\0') {
		g_free(content);
		return NULL;
	}
	return content;
}

static uint64_t sysfs_read_u64(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static uint64_t sysfs_read_u64(const char *fmt, ...)
{
	va_list ap;
	char path[512];
	va_start(ap, fmt);
	g_vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	char *s = NULL;
	uint64_t v = 0;
	if (g_file_get_contents(path, &s, NULL, NULL)) {
		v = g_ascii_strtoull(g_strstrip(s), NULL, 10);
		g_free(s);
	}
	return v;
}

static bool sysfs_exists(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static bool sysfs_exists(const char *fmt, ...)
{
	va_list ap;
	char path[512];
	va_start(ap, fmt);
	g_vsnprintf(path, sizeof(path), fmt, ap);
	va_end(ap);
	return g_file_test(path, G_FILE_TEST_EXISTS);
}

/* ----------------------------------------------------------------------- */
/* /proc/mounts                                                            */
/* ----------------------------------------------------------------------- */
static gint mount_path_deepest_first(gconstpointer a, gconstpointer b)
{
	const char *sa = *(char *const*)a;
	const char *sb = *(char *const*)b;
	size_t la = strlen(sa), lb = strlen(sb);
	if (la == lb) return strcmp(sa, sb);
	return la < lb ? 1 : -1;	/* deepest first */
}

GPtrArray* rfx_dev_mount_points(const char *devpath)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	FILE *fp = setmntent("/proc/mounts", "r");
	if (fp == NULL) return out;

	struct mntent *me;
	while ((me = getmntent(fp)) != NULL) {
		/* Direct match (/dev/sdb1) or child of the whole disk (/dev/sdb*) */
		if (strcmp(me->mnt_fsname, devpath) == 0 ||
		    (g_str_has_prefix(me->mnt_fsname, devpath) &&
		     isdigit((unsigned char)me->mnt_fsname[strlen(devpath)])))
			g_ptr_array_add(out, g_strdup(me->mnt_dir));
	}
	endmntent(fp);
	return out;
}

bool rfx_dev_unmount_all(const char *devpath)
{
	g_autoptr(GPtrArray) mps = rfx_dev_mount_points(devpath);
	bool ok = true;

	/* Unmount the deepest paths first so that a bind-mount stack collapses. */
	g_ptr_array_sort(mps, mount_path_deepest_first);

	for (guint i = 0; i < mps->len; i++) {
		const char *mp = g_ptr_array_index(mps, i);
		/* Try lazy-free umount(2) through the mount utility so that
		 * udisks2's bookkeeping stays consistent when present. */
		if (rfx_tool_available("umount")) {
			char *a1[] = { (char*)"umount", (char*)mp, NULL };
			if (!rfx_run_ok(a1, NULL, NULL)) {
				char *a2[] = { (char*)"umount", (char*)"-l", (char*)mp, NULL };
				ok &= rfx_run_ok(a2, NULL, NULL);
			}
		} else if (umount(mp) != 0 && umount2(mp, MNT_DETACH) != 0) {
			uprintfw("Could not unmount '%s': %s", mp, strerror(errno));
			ok = false;
		}
	}
	return ok;
}

/* ----------------------------------------------------------------------- */
/* Boot-device detection (Rufus: IsFilteredDrive)                          */
/* ----------------------------------------------------------------------- */
static GHashTable* boot_disk_cache;	/* "/sys/block/sda" -> GINT_TO_POINTER(1) */

static void mark_boot_disk(const char *disk_name)
{
	if (boot_disk_cache == NULL)
		boot_disk_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_add(boot_disk_cache, g_strdup(disk_name));
}

static void add_holders_of(const char *sys_block_name, int depth)
{
	/* device-mapper / md / lvm: walk the "slaves" links so that a root on
	 * /dev/dm-0 backed by /dev/sda2 marks sda as the boot disk. */
	if (depth > 8) return;
	char path[512];
	g_snprintf(path, sizeof(path), "/sys/block/%s/slaves", sys_block_name);

	DIR *d = opendir(path);
	if (d == NULL) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		mark_boot_disk(de->d_name);
		add_holders_of(de->d_name, depth + 1);
	}
	closedir(d);
}

/*
 * Return a newly-allocated whole-disk kernel name for a partition name:
 *   "sda2"->"sda", "nvme0n1p2"->"nvme0n1", "mmcblk0p1"->"mmcblk0",
 *   "nvme0n1"->"nvme0n1" (whole disk, left intact), "sdp"->"sdp" (real disk
 *   name, left intact). Returns NULL if nothing sane is left. Shared by the
 *   /proc/mounts and /proc/cmdline boot-disk detection so both agree — the
 *   old root= path used a naive "strip trailing digits and 'p'" loop that
 *   mangled nvme0n1 -> nvme0n and sdp -> sd (docs/REVIEW.md M3).
 */
static char* whole_disk_name(const char *part)
{
	if (part == NULL || *part == '\0') return NULL;
	char *disk = g_strdup(part);
	size_t n = strlen(disk);
	if (n > 0 && isdigit((unsigned char)disk[n - 1])) {
		if (g_str_has_prefix(disk, "nvme")   || g_str_has_prefix(disk, "mmcblk") ||
		    g_str_has_prefix(disk, "loop")   || g_str_has_prefix(disk, "md")     ||
		    g_str_has_prefix(disk, "dm-")) {
			/* "…n1p2" -> cut at the 'p' that follows a digit. A whole
			 * device ("dm-0", "md0", "loop0", "nvme0n1") has no such 'p'
			 * and is deliberately left intact. */
			char *p = strrchr(disk, 'p');
			if (p != NULL && p > disk && isdigit((unsigned char)p[-1]))
				*p = '\0';
		} else {
			/* "sda2" / "vda2" / "xvda2" -> strip the trailing digits.
			 * A whole disk like "sdp" ends in a letter and is untouched. */
			while (n > 0 && isdigit((unsigned char)disk[n - 1]))
				disk[--n] = '\0';
		}
	}
	if (disk[0] == '\0') { g_free(disk); return NULL; }
	return disk;
}

static void detect_boot_disks(void)
{
	if (boot_disk_cache != NULL)
		return;
	boot_disk_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	FILE *fp = setmntent("/proc/mounts", "r");
	if (fp == NULL) return;

	struct mntent *me;
	while ((me = getmntent(fp)) != NULL) {
		/* Only care about the mounts that prove "this system lives here" */
		if (strcmp(me->mnt_dir, "/") != 0 && strcmp(me->mnt_dir, "/boot") != 0 &&
		    strcmp(me->mnt_dir, "/boot/efi") != 0 && strcmp(me->mnt_dir, "/home") != 0 &&
		    strcmp(me->mnt_dir, "/var") != 0 && strcmp(me->mnt_dir, "/usr") != 0)
			continue;
		if (strncmp(me->mnt_fsname, "/dev/", 5) != 0)
			continue;

		char *dev = me->mnt_fsname + 5;	/* "sda2", "nvme0n1p2", "dm-0", "mapper/vg-lv" */
		char *real = NULL;

		if (strncmp(dev, "mapper/", 7) == 0 || g_str_has_prefix(dev, "/dev/mapper/")) {
			const char *base = strrchr(dev, '/');
			g_autofree char *link = g_strdup_printf("/dev/mapper/%s", base ? base + 1 : dev);
			real = g_file_read_link(link, NULL);
			if (real != NULL) {
				char *b = g_path_get_basename(real);
				g_free(real);
				real = b;
			}
		} else {
			/* /proc/mounts can list a /dev/disk/by-* symlink here; resolve
			 * it so the whole-disk name matches /sys/block. realpath()
			 * resolves symlinks (g_canonicalize_filename() does not). Fall
			 * back to the raw name if the node is inaccessible (e.g. in a
			 * container) so we never lose boot-disk protection
			 * (docs/REVIEW.md M4). */
			g_autofree char *full = g_strdup_printf("/dev/%s", dev);
			char *rp = realpath(full, NULL);
			if (rp != NULL) {
				real = g_path_get_basename(rp);
				free(rp);
			} else {
				real = g_strdup(dev);
			}
		}
		if (real == NULL) continue;

		/* Strip the partition suffix to get the whole-disk name (shared with
		 * the /proc/cmdline path below so both agree). */
		char *disk = whole_disk_name(real);
		if (disk != NULL) {
			mark_boot_disk(disk);
			add_holders_of(disk, 0);
			g_free(disk);
		}
		g_free(real);
	}
	endmntent(fp);

	/* Also honour /proc/cmdline root= for the (rare) initramfs-less case.
	 * Modern distros default to root=UUID=…, so resolve the /dev/disk/by-*
	 * symlinks too, and reuse whole_disk_name() — the old naive "strip
	 * trailing digits and 'p'" loop mangled nvme0n1 -> nvme0n and a real
	 * disk named sdp -> sd (docs/REVIEW.md M3). */
	char *cmdline = NULL;
	if (g_file_get_contents("/proc/cmdline", &cmdline, NULL, NULL)) {
		g_auto(GStrv) toks = g_strsplit(cmdline, " ", -1);
		for (int i = 0; toks[i] != NULL; i++) {
			if (!g_str_has_prefix(toks[i], "root="))
				continue;
			const char *v = toks[i] + 5;
			g_autofree char *node = NULL;   /* device path to resolve */
			if (g_str_has_prefix(v, "/dev/")) {
				node = g_strdup(v);
			} else {
				const char *eq = strchr(v, '=');
				const char *dir =
					g_str_has_prefix(v, "PARTUUID=") ? "by-partuuid" :
					g_str_has_prefix(v, "LABEL=")    ? "by-label"    :
					g_str_has_prefix(v, "UUID=")     ? "by-uuid"     : NULL;
				if (eq != NULL && dir != NULL)
					node = g_strdup_printf("/dev/disk/%s/%s", dir, eq + 1);
			}
			if (node == NULL)
				continue;
			char *rp = realpath(node, NULL);
			g_autofree char *bname = g_path_get_basename(rp != NULL ? rp : node);
			if (rp != NULL) free(rp);
			char *disk = whole_disk_name(bname);
			if (disk != NULL) {
				mark_boot_disk(disk);
				add_holders_of(disk, 0);
				g_free(disk);
			}
		}
		g_free(cmdline);
	}
	uprintfd("Boot disks: %u", g_hash_table_size(boot_disk_cache));
}

/* ----------------------------------------------------------------------- */
/* Geometry (Rufus: IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)                      */
/* ----------------------------------------------------------------------- */
static void probe_geometry(RfxDrive *d)
{
	int fd = rfx_open_block(d->DevicePath, false);
	if (fd < 0) {
		d->SectorSize = 512;
		d->PhysSectorSize = 512;
		d->SectorsPerTrack = 63;
		d->Heads = 255;
		return;
	}

	int sector = 0;
	if (ioctl(fd, BLKSSZGET, &sector) == 0 && sector >= 512)
		d->SectorSize = (uint32_t)sector;
	else
		d->SectorSize = 512;

	/* BLKpbszget is Linux 5.10+; fall back to sysfs queue/physical_block_size */
	int pbsz = 0;
	if (ioctl(fd, BLKPBSZGET, &pbsz) == 0 && pbsz >= 512)
		d->PhysSectorSize = (uint32_t)pbsz;
	else {
		const char *base = strrchr(d->DevicePath, '/');
		uint64_t p = sysfs_read_u64("/sys/block/%s/queue/physical_block_size",
		                            base ? base + 1 : "");
		d->PhysSectorSize = p ? (uint32_t)p : d->SectorSize;
	}

	uint64_t nbytes = 0;
	if (ioctl(fd, BLKGETSIZE64, &nbytes) == 0 && nbytes != 0)
		d->DiskSize = nbytes;   /* BLKGETSIZE64 returns *bytes*, not sectors */

	struct hd_geometry geo;
	if (ioctl(fd, HDIO_GETGEO, &geo) == 0 && geo.sectors != 0) {
		d->Heads = geo.heads;
		d->SectorsPerTrack = geo.sectors;
	} else {
		/* Rufus uses these to align partitions to a "track"; modern
		 * kernels report a sane default for removable media. */
		d->Heads = 255;
		d->SectorsPerTrack = 63;
	}

	/* HDIO_GETGEO reports sectors-per-track in 512-byte units; partition.c
	 * turns that into a byte offset and aligns it to the real sector size
	 * (docs/REVIEW.md M1). The old `(spt * ss) % ss != 0` guard here was a
	 * tautology that could never fire, so it is gone. */

	close(fd);
}

/* ----------------------------------------------------------------------- */
/* Partition table parsing (Rufus: GetDrivePartitionData / AnalyzeMBR)     */
/* ----------------------------------------------------------------------- */
static int gpt_type_index_from_guid(const char *guid);

static void parse_partitions(RfxDrive *d)
{
	d->PartitionCount = 0;
	d->PartitionStyle = RFX_PS_SFD;
	memset(d->Partition, 0, sizeof(d->Partition));

	int fd = rfx_open_block(d->DevicePath, false);
	if (fd < 0) return;

	uint8_t sector[4096];
	memset(sector, 0, sizeof(sector));
	if (!rfx_read_exact(fd, sector, d->SectorSize, 0)) {
		close(fd);
		return;
	}

	bool has_mbr = (sector[510] == 0x55 && sector[511] == 0xAA);
	bool has_gpt = has_mbr && sector[450] == 0xEE;

	if (has_gpt) {
		/* Read the real GPT header at LBA1 */
		uint8_t gpth[4096];
		memset(gpth, 0, sizeof(gpth));
		if (rfx_read_exact(fd, gpth, d->SectorSize, (uint64_t)d->SectorSize) &&
		    memcmp(gpth, "EFI PART", 8) == 0) {
			d->PartitionStyle = RFX_PS_GPT;
			uint64_t first_usable, last_usable, part_lba;
			uint32_t n_entries, entry_size;
			memcpy(&part_lba,    gpth + 72, 8);
			memcpy(&n_entries,   gpth + 80, 4);
			memcpy(&entry_size,  gpth + 84, 4);
			memcpy(&first_usable,gpth + 40, 8);
			memcpy(&last_usable, gpth + 48, 8);
			(void)first_usable; (void)last_usable;

			if (entry_size < 128) entry_size = 128;
			if (n_entries > 128)  n_entries = 128;

			gsize table_len = (gsize)n_entries * entry_size;
			g_autofree uint8_t *table = g_malloc0(table_len);
			if (rfx_read_exact(fd, table, table_len, part_lba * d->SectorSize)) {
				for (uint32_t i = 0; i < n_entries && d->PartitionCount < RFX_MAX_PARTITIONS; i++) {
					uint8_t *e = table + (gsize)i * entry_size;
					uint8_t type_guid[16], uniq[16];
					uint64_t first, last, attrs;
					memcpy(type_guid, e, 16);
					memcpy(uniq, e + 16, 16);
					memcpy(&first, e + 32, 8);
					memcpy(&last,  e + 40, 8);
					memcpy(&attrs, e + 48, 8);
					/* An all-zero type GUID marks a free slot */
					bool empty = true;
					for (int j = 0; j < 16; j++) if (type_guid[j]) { empty = false; break; }
					if (empty) continue;

					RfxPartition *p = &d->Partition[d->PartitionCount];
					p->Offset = first * d->SectorSize;
					p->Size   = (last - first + 1) * d->SectorSize;
					p->IsEsp  = (attrs & 0x1) != 0;
					p->IsBoot = (attrs & 0x4) != 0;
					/* UTF-16LE name, 72 bytes */
					char name[128] = { 0 };
					int k = 0;
					for (int j = 72; j < 128 && k < (int)sizeof(name) - 1; j += 2) {
						uint16_t c = (uint16_t)(e[j] | (e[j+1] << 8));
						if (c == 0) break;
						name[k++] = (c < 0x80) ? (char)c : '?';
					}
					g_strlcpy(p->Name, name, sizeof(p->Name));
					(void)uniq;

					char guid[64];
					g_snprintf(guid, sizeof(guid),
					           "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
					           type_guid[3],type_guid[2],type_guid[1],type_guid[0],
					           type_guid[5],type_guid[4],type_guid[7],type_guid[6],
					           type_guid[8],type_guid[9],type_guid[10],type_guid[11],
					           type_guid[12],type_guid[13],type_guid[14],type_guid[15]);
					int ti = gpt_type_index_from_guid(guid);
					if (ti >= 0) g_snprintf(p->FsName, sizeof(p->FsName), "%s", "GPT");
					d->PartitionCount++;
				}
			}
		}
	} else if (has_mbr) {
		d->PartitionStyle = RFX_PS_MBR;
		for (int i = 0; i < 4 && d->PartitionCount < RFX_MAX_PARTITIONS; i++) {
			uint8_t *e = sector + 446 + i * 16;
			if (e[4] == 0) continue;
			uint32_t lba, nsec;
			memcpy(&lba,  e + 8, 4);
			memcpy(&nsec, e + 12, 4);
			if (nsec == 0) continue;
			RfxPartition *p = &d->Partition[d->PartitionCount++];
			p->Offset = (uint64_t)lba * d->SectorSize;
			p->Size   = (uint64_t)nsec * d->SectorSize;
			p->IsBoot = (e[0] & 0x80) != 0;
			p->IsEsp  = (e[4] == 0xEF);
			g_snprintf(p->Name, sizeof(p->Name), "Primary #%d", i + 1);
			g_snprintf(p->FsName, sizeof(p->FsName), "0x%02X", e[4]);
		}
	}

	close(fd);

	/* Identify the file system of each partition with libblkid. */
	const char *base = strrchr(d->DevicePath, '/');
	base = base ? base + 1 : d->DevicePath;
	for (int i = 0; i < d->PartitionCount; i++) {
		char *part_path = NULL;
		if (g_str_has_prefix(base, "nvme") || g_str_has_prefix(base, "mmcblk") ||
		    g_str_has_prefix(base, "loop") || g_str_has_prefix(base, "md"))
			part_path = g_strdup_printf("%sp%d", d->DevicePath, i + 1);
		else
			part_path = g_strdup_printf("%s%d", d->DevicePath, i + 1);

		blkid_probe pr = blkid_new_probe_from_filename(part_path);
		g_free(part_path);
		if (pr == NULL) continue;
		blkid_probe_enable_partitions(pr, 0);
		if (blkid_do_safeprobe(pr) == 0) {
			const char *type = NULL;
			if (blkid_probe_lookup_value(pr, "TYPE", &type, NULL) == 0 && type)
				g_strlcpy(d->Partition[i].FsName, type, sizeof(d->Partition[i].FsName));
			const char *label = NULL;
			if (blkid_probe_lookup_value(pr, "LABEL", &label, NULL) == 0 && label &&
			    d->Partition[i].Name[0] == '\0')
				g_strlcpy(d->Partition[i].Name, label, sizeof(d->Partition[i].Name));
		}
		blkid_free_probe(pr);
	}
}

/* A compact table for the GPT types we care about (Rufus: gpt_types.h) */
static const struct { const char *guid; const char *name; } gpt_types[] = {
	{ "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI System Partition" },
	{ "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft Reserved Partition" },
	{ "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft Basic Data" },
	{ "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", "Windows Recovery" },
	{ "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux Filesystem Data" },
	{ "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux Swap" },
};
static int gpt_type_index_from_guid(const char *guid)
{
	for (guint i = 0; i < G_N_ELEMENTS(gpt_types); i++)
		if (g_ascii_strcasecmp(gpt_types[i].guid, guid) == 0)
			return (int)i;
	return -1;
}
const char* rfx_gpt_type_name(const char *guid);
const char* rfx_gpt_type_name(const char *guid)
{
	int i = gpt_type_index_from_guid(guid);
	return i >= 0 ? gpt_types[i].name : "Unknown";
}

/* ----------------------------------------------------------------------- */
/* Probing a single device                                                 */
/* ----------------------------------------------------------------------- */
static void probe_bus_and_identity(RfxDrive *d, const char *name)
{
	g_autofree char *dev_link = g_file_read_link(d->DevicePath, NULL);
	g_autofree char *subsystem = NULL;

	/* /sys/block/<name>/device is a symlink into the bus hierarchy */
	g_autofree char *devpath = g_strdup_printf("/sys/block/%s/device", name);
	char *real = g_file_read_link(devpath, NULL);
	if (real != NULL) {
		if (strstr(real, "/usb") != NULL || strstr(real, "usb") == real)
			g_strlcpy(d->BusName, "usb", sizeof(d->BusName));
		else if (strstr(real, "/nvme") != NULL)
			g_strlcpy(d->BusName, "nvme", sizeof(d->BusName));
		else if (strstr(real, "/ata") != NULL || strstr(real, "/scsi") != NULL)
			g_strlcpy(d->BusName, "scsi", sizeof(d->BusName));
		else if (strstr(real, "/mmc") != NULL)
			g_strlcpy(d->BusName, "mmc", sizeof(d->BusName));
		else if (strstr(real, "/virtio") != NULL)
			g_strlcpy(d->BusName, "virtio", sizeof(d->BusName));
		g_free(real);
	}

	/* mmc host controllers report themselves under /sys/block/mmcblk0/device */
	if (g_str_has_prefix(name, "mmcblk"))
		g_strlcpy(d->BusName, "mmc", sizeof(d->BusName));
	if (g_str_has_prefix(name, "nvme"))
		g_strlcpy(d->BusName, "nvme", sizeof(d->BusName));
	if (g_str_has_prefix(name, "loop"))
		g_strlcpy(d->BusName, "loop", sizeof(d->BusName));
	if (g_str_has_prefix(name, "sr"))
		g_strlcpy(d->BusName, "ata", sizeof(d->BusName));
	if (g_str_has_prefix(name, "fd"))
		g_strlcpy(d->BusName, "floppy", sizeof(d->BusName));

	if (d->BusName[0] == '\0')
		g_strlcpy(d->BusName, "unknown", sizeof(d->BusName));

	(void)subsystem; (void)dev_link;
}

bool rfx_dev_probe(const char *devpath, RfxDrive *out)
{
	g_return_val_if_fail(devpath != NULL && out != NULL, false);
	memset(out, 0, sizeof(*out));
	g_strlcpy(out->DevicePath, devpath, sizeof(out->DevicePath));

	const char *name = strrchr(devpath, '/');
	name = name ? name + 1 : devpath;

	struct stat st;
	if (stat(devpath, &st) != 0) {
		rfx_set_error(ERR_NO_DEVICE, errno, "Cannot stat '%s'", devpath);
		return false;
	}
	/* A regular file is a legitimate target here: it is how the whole
	 * pipeline gets exercised without a spare stick. Everything else must
	 * be a block device. Fail quietly, the caller decides what it means. */
	if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode)) {
		rfx_set_error(ERR_NO_DEVICE, 0, "'%s' is not a block device", devpath);
		return false;
	}
	if (S_ISREG(st.st_mode))
		return false;

	g_strlcpy(out->DeviceId, name, sizeof(out->DeviceId));
	probe_geometry(out);
	probe_bus_and_identity(out, name);

	g_autofree char *vendor  = sysfs_read("/sys/block/%s/device/vendor", name);
	g_autofree char *model   = sysfs_read("/sys/block/%s/device/model", name);
	g_autofree char *serial  = sysfs_read("/sys/block/%s/device/serial", name);
	g_autofree char *rev     = sysfs_read("/sys/block/%s/device/rev", name);
	g_autofree char *rm      = sysfs_read("/sys/block/%s/removable", name);
	g_autofree char *ro      = sysfs_read("/sys/block/%s/ro", name);
	g_autofree char *media   = sysfs_read("/sys/block/%s/device/media_type", name);
	uint64_t rotational      = sysfs_read_u64("/sys/block/%s/queue/rotational", name);

	if (vendor == NULL) vendor = sysfs_read("/sys/block/%s/device/subsystem_vendor", name);
	if (model  == NULL) model  = sysfs_read("/sys/block/%s/device/name", name);

	/* NVMe and MMC keep their identity elsewhere */
	if (model == NULL && g_str_has_prefix(name, "nvme"))
		model = sysfs_read("/sys/block/%s/device/model", name);
	if (serial == NULL && g_str_has_prefix(name, "nvme"))
		serial = sysfs_read("/sys/block/%s/device/serial", name);
	if (model == NULL && g_str_has_prefix(name, "mmcblk")) {
		model  = sysfs_read("/sys/block/%s/device/name", name);
		serial = sysfs_read("/sys/block/%s/device/serial", name);
	}

	if (vendor) g_strlcpy(out->Vendor, g_strstrip(vendor), sizeof(out->Vendor));
	if (model)  g_strlcpy(out->Model,  g_strstrip(model),  sizeof(out->Model));
	if (serial) g_strlcpy(out->Serial, g_strstrip(serial),  sizeof(out->Serial));

	out->IsRemovable   = (rm != NULL && atoi(rm) == 1) ||
	                     strcmp(out->BusName, "usb") == 0 ||
	                     strcmp(out->BusName, "mmc") == 0;
	out->IsWholeDiskReadOnly = (ro != NULL && atoi(ro) == 1);
	out->IsOptical     = g_str_has_prefix(name, "sr") || g_str_has_prefix(name, "scd");
	out->IsFloppy      = g_str_has_prefix(name, "fd");
	out->IsLoop        = g_str_has_prefix(name, "loop");
	out->RotationRate  = out->IsRemovable ? -1 : (rotational ? 5400 : -1);
	out->HasMedia      = out->DiskSize > 0;
	if (media != NULL && atoi(media) == 1)		/* FixedMedia */
		out->IsRemovable = false;

	/* device-mapper / md / lvm are virtual: never offered as a target */
	if (g_str_has_prefix(name, "dm-") || g_str_has_prefix(name, "md") ||
	    sysfs_exists("/sys/block/%s/dm", name) ||
	    sysfs_exists("/sys/block/%s/md", name))
		out->IsSynthetic = true;

	/* udisks2 "HintSystem" equivalent */
	out->IsSystemInternal = sysfs_exists("/sys/block/%s/device", name) &&
	                        strcmp(out->BusName, "usb") != 0 &&
	                        strcmp(out->BusName, "mmc") != 0 &&
	                        !out->IsRemovable;

	/* Friendly name: "SanDisk Ultra (sdb)" — same shape as Rufus' list */
	char size_buf[32];
	rfx_human_size(out->DiskSize, size_buf, sizeof(size_buf));
	const char *brand = *out->Model ? out->Model : (*out->Vendor ? out->Vendor : NULL);
	if (brand != NULL)
		g_snprintf(out->FriendlyName, sizeof(out->FriendlyName), "%s (%s) [%s]",
		           brand, name, size_buf);
	else
		g_snprintf(out->FriendlyName, sizeof(out->FriendlyName), "%s [%s]",
		           name, size_buf);

	/* First mountpoint, if any (Rufus: drive letters) */
	g_autoptr(GPtrArray) mps = rfx_dev_mount_points(devpath);
	if (mps->len > 0)
		g_strlcpy(out->MountPoint, g_ptr_array_index(mps, 0), sizeof(out->MountPoint));

	detect_boot_disks();
	out->IsBootDevice = (boot_disk_cache != NULL &&
	                     g_hash_table_contains(boot_disk_cache, name));

	parse_partitions(out);

	(void)rev;
	return true;
}

bool rfx_dev_probe_file(const char *path, RfxDrive *out)
{
	g_return_val_if_fail(path != NULL && out != NULL, false);
	memset(out, 0, sizeof(*out));

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		rfx_set_error(ERR_NO_DEVICE, errno, "'%s' is not a regular file", path);
		return false;
	}

	g_strlcpy(out->DevicePath, path, sizeof(out->DevicePath));
	g_autofree char *base = g_path_get_basename(path);
	g_strlcpy(out->DeviceId, base, sizeof(out->DeviceId));
	out->DiskSize       = (uint64_t)st.st_size;
	out->SectorSize     = 512;
	out->PhysSectorSize = 512;
	out->SectorsPerTrack= 63;
	out->Heads          = 255;
	out->IsRemovable    = true;
	out->IsSynthetic    = true;
	out->HasMedia       = true;
	g_strlcpy(out->BusName, "file", sizeof(out->BusName));
	{
		char tmp[32];
		rfx_human_size(out->DiskSize, tmp, sizeof(tmp));
		g_snprintf(out->FriendlyName, sizeof(out->FriendlyName),
		           "Image file '%s' (%s)", base, tmp);
	}
	parse_partitions(out);
	return true;
}

/* ----------------------------------------------------------------------- */
/* Filtering (Rufus: IsFilteredDrive / IsMsDevDrive)                       */
/* ----------------------------------------------------------------------- */
bool rfx_dev_is_filtered(const RfxDrive *d)
{
	if (d == NULL) return true;
	if (!d->HasMedia || d->DiskSize == 0)        return true;
	if (d->IsBootDevice)                         return true;	/* never nuke the system disk */
	if (d->IsOptical || d->IsFloppy)             return true;
	if (g_str_has_prefix(d->DeviceId, "dm-"))    return true;
	if (g_str_has_prefix(d->DeviceId, "md"))     return true;
	if (g_str_has_prefix(d->DeviceId, "ram"))    return true;
	if (g_str_has_prefix(d->DeviceId, "zram"))   return true;
	if (d->IsWholeDiskReadOnly)                  return true;
	/* loop devices are allowed, but only when they are not synthetic placeholders */
	return false;
}

/* ----------------------------------------------------------------------- */
/* List management                                                         */
/* ----------------------------------------------------------------------- */
static void drive_free(RfxDrive *d)
{
	if (d == NULL) return;
	g_free(d->udisks_drive_object);
	g_free(d->udisks_block_object);
	g_free(d);
}

RfxDevList* rfx_dev_list_new(void)
{
	RfxDevList *l = g_new0(RfxDevList, 1);
	l->drives = g_ptr_array_new_with_free_func((GDestroyNotify)drive_free);
	l->filtered_reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
#ifdef HAVE_GUDEV
	l->udev = g_udev_client_new((const gchar*[]){ "block", NULL });
#endif
	return l;
}

void rfx_dev_list_free(RfxDevList *l)
{
	if (l == NULL) return;
	g_ptr_array_free(l->drives, TRUE);
	g_hash_table_destroy(l->filtered_reasons);
#ifdef HAVE_GUDEV
	g_clear_object(&l->udev);
#endif
	g_free(l);
}

static gint drive_sort_cmp(gconstpointer a, gconstpointer b)
{
	const RfxDrive *da = *(RfxDrive *const*)a;
	const RfxDrive *db = *(RfxDrive *const*)b;
	/* Removable media first (that is what people plug in), then by size desc */
	if (da->IsRemovable != db->IsRemovable)
		return da->IsRemovable ? -1 : 1;
	if (da->DiskSize != db->DiskSize)
		return da->DiskSize > db->DiskSize ? -1 : 1;
	return strcmp(da->DevicePath, db->DevicePath);
}

bool rfx_dev_list_refresh(RfxDevList *l)
{
	g_return_val_if_fail(l != NULL, false);
	g_ptr_array_set_size(l->drives, 0);
	g_hash_table_remove_all(l->filtered_reasons);

	DIR *dir = opendir("/sys/block");
	if (dir == NULL) {
		rfx_set_error(ERR_NO_DEVICE, errno, "Cannot enumerate /sys/block");
		return false;
	}

	detect_boot_disks();

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') continue;

		g_autofree char *devpath = g_strdup_printf("/dev/%s", de->d_name);
		if (!g_file_test(devpath, G_FILE_TEST_EXISTS))
			continue;

		RfxDrive *d = g_new0(RfxDrive, 1);
		if (!rfx_dev_probe(devpath, d)) {
			drive_free(d);
			continue;
		}
		d->Index = l->drives->len;

		if (rfx_dev_is_filtered(d)) {
			const char *why =
				d->IsBootDevice ? "boot device" :
				d->IsOptical    ? "optical drive" :
				d->IsFloppy     ? "floppy" :
				!d->HasMedia    ? "no media" :
				d->IsWholeDiskReadOnly ? "read-only" : "virtual device";
			g_hash_table_insert(l->filtered_reasons,
			                    g_strdup(d->DevicePath), g_strdup(why));
			uprintfd("Filtered out %s (%s)", d->DevicePath, why);
			drive_free(d);
			continue;
		}
		g_ptr_array_add(l->drives, d);
	}
	closedir(dir);

	g_ptr_array_sort(l->drives, drive_sort_cmp);
	for (guint i = 0; i < l->drives->len; i++)
		((RfxDrive*)g_ptr_array_index(l->drives, i))->Index = i;

	uprintfd("Found %u candidate device(s)", l->drives->len);
	return true;
}

int rfx_dev_list_count(const RfxDevList *l)
{
	return l == NULL ? 0 : (int)l->drives->len;
}

const RfxDrive* rfx_dev_list_get(const RfxDevList *l, int i)
{
	if (l == NULL || i < 0 || i >= (int)l->drives->len) return NULL;
	return g_ptr_array_index(l->drives, i);
}

const RfxDrive* rfx_dev_list_find_by_path(const RfxDevList *l, const char *devpath)
{
	if (l == NULL || devpath == NULL) return NULL;
	for (guint i = 0; i < l->drives->len; i++) {
		const RfxDrive *d = g_ptr_array_index(l->drives, i);
		if (strcmp(d->DevicePath, devpath) == 0)
			return d;
	}
	return NULL;
}

/* Rufus issues IOCTL_STORAGE_MEDIA_REMOVAL / a VDS rescan after repartitioning
 * so that Windows notices the new layout. Linux needs BLKRRPART. */
bool rfx_dev_repartition_notify(const char *devpath)
{
	struct stat st;
	if (stat(devpath, &st) == 0 && S_ISREG(st.st_mode)) {
		/* A regular file has no partition nodes and no udev; callers
		 * reach its partitions by offset instead (see RfxPartTarget). */
		sync();
		return true;
	}

	int fd = rfx_open_block(devpath, true);
	if (fd < 0) return false;
	sync();
	int r = ioctl(fd, BLKRRPART, 0);
	if (r != 0)
		uprintfw("BLKRRPART on %s failed: %s", devpath, strerror(errno));
	close(fd);

	/* Give udev a moment to create the new /dev/sdXN nodes. A bare
	 * container has no udev daemon, so only wait for it when it exists. */
	if (rfx_tool_available("udevadm")) {
		char *args[] = { (char*)"udevadm", (char*)"settle", (char*)"--timeout=2", NULL };
		rfx_run_ok(args, NULL, NULL);
	} else {
		g_usleep(250 * 1000);
	}
	return r == 0;
}

/* ----------------------------------------------------------------------- */
/* Hotplug (Rufus: WM_DEVICECHANGE + RefreshTimer)                         */
/* ----------------------------------------------------------------------- */
#ifdef HAVE_GUDEV
typedef struct {
	GMainContext *ctx;
	RfxDevList   *list;
} HotplugData;

static void on_uevent(GUdevClient *client, const gchar *action,
                      GUdevDevice *device, gpointer user_data)
{
	(void)client; (void)device;
	HotplugData *hd = user_data;
	if (g_strcmp0(action, "add") != 0 && g_strcmp0(action, "remove") != 0 &&
	    g_strcmp0(action, "change") != 0)
		return;
	uprintfd("udev: %s %s", action, g_udev_device_get_name(device));
	rfx_dev_list_refresh(hd->list);
}

/* Called by the UI layer once the main loop exists. */
void rfx_dev_enable_hotplug(RfxDevList *l, GMainContext *ctx);
void rfx_dev_enable_hotplug(RfxDevList *l, GMainContext *ctx)
{
	if (l == NULL || l->udev == NULL) return;
	HotplugData *hd = g_new0(HotplugData, 1);
	hd->ctx = ctx;
	hd->list = l;
	g_signal_connect(l->udev, "uevent", G_CALLBACK(on_uevent), hd);
}
#else
void rfx_dev_enable_hotplug(RfxDevList *l, GMainContext *ctx)
{
	(void)l; (void)ctx;
	uprintfd("Built without GUdev: hotplug detection disabled");
}
#endif
