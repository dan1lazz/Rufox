/*
 * rufox — partition layout & creation
 *
 * Port of Rufus' CreatePartition() (src/drive.c). Rufus builds a
 * DRIVE_LAYOUT_INFORMATION_EX and pushes it with IOCTL_DISK_SET_DRIVE_LAYOUT_EX.
 * Linux has no such single-shot ioctl for MBR/GPT, so we use libparted — but
 * the *geometry decisions* (where the ESP goes, how much room the persistence
 * partition gets, how the main partition is aligned) are reproduced exactly,
 * because they are what makes the resulting stick actually boot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <parted/parted.h>
#include <parted/debug.h>

/* Rufus: MAX_SECTORS_TO_CLEAR * SectorSize is zeroed at the head of every
 * new partition "to avoid file system caching issues". */
#define MAX_SECTORS_TO_CLEAR 128

/* GPT reserves 34 sectors up front and 33 at the end. */
#define GPT_RESERVED_HEAD 34
#define GPT_RESERVED_TAIL 33

/* Rufus: 260 MB ESP by default, to keep 4Kn users and macOS happy. */
#define DEFAULT_ESP_SIZE (260ULL * RFX_MB)
/* Rufus: 128 MB MSR for Windows To Go on GPT. */
#define DEFAULT_MSR_SIZE (128ULL * RFX_MB)

/* Partition role, in the order Rufus lays them out. */
typedef enum {
	PI_ESP = 0,
	PI_MSR,
	PI_MAIN,
	PI_CASPER,	/* Linux persistence */
	PI_UEFI_NTFS,
	PI_COMPAT,	/* "Fixes for old BIOSes" */
} PartIndex;

static const char* part_name_for_role(int role)
{
	switch (role) {
	case PI_ESP:       return "EFI System Partition";
	case PI_MSR:       return "Microsoft Reserved Partition";
	case PI_MAIN:      return "Main Data Partition";
	case PI_CASPER:    return "Linux Persistence";
	case PI_UEFI_NTFS: return "UEFI:NTFS";
	case PI_COMPAT:    return "BIOS Compatibility";
	}
	return "Unknown";
}

/* ----------------------------------------------------------------------- */
/* The layout planner — pure function, no I/O. This is what the GUI shows  */
/* in the confirmation dialog and what rfx_partition_create() commits.     */
/* ----------------------------------------------------------------------- */
int rfx_partition_plan(const RfxDrive *drive, const RfxOptions *opt,
                       const RfxImgReport *img, uint64_t uefi_ntfs_size,
                       RfxPartSpec out[RFX_MAX_PARTITIONS])
{
	g_return_val_if_fail(drive != NULL && opt != NULL && out != NULL, 0);

	const uint32_t sector_size = drive->SectorSize ? drive->SectorSize : 512;
	/* HDIO_GETGEO reports sectors-per-track in 512-byte units. Rufus' CHS
	 * "track" is 63*512 bytes regardless of the media's sector size; on 4Kn
	 * (sector_size 4096) we still have to land on a sector boundary, so
	 * compute the track in 512-byte units and align up. The old
	 * `spt * sector_size` silently multiplied by 4096 and pushed the
	 * old-BIOS offset to 516096 instead of Rufus' 64512 (docs/REVIEW.md M1). */
	const uint64_t bytes_per_track = RFX_CEILING_ALIGN(
		(uint64_t)(drive->SectorsPerTrack ? drive->SectorsPerTrack : 63) * 512,
		sector_size);
	const uint64_t cluster = opt->cluster_size ? opt->cluster_size : 512;
	const int style = opt->partition_scheme;

	memset(out, 0, sizeof(RfxPartSpec) * RFX_MAX_PARTITIONS);

	if (style == RFX_PS_SFD) {
		/* Superfloppy: a single filesystem over the whole medium, no table. */
		out[0].Offset = 0;
		out[0].Size   = drive->DiskSize;
		g_strlcpy(out[0].Name, opt->label, sizeof(out[0].Name));
		out[0].Boot   = (opt->boot_type != RFX_BT_NON_BOOTABLE);
		return 1;
	}

	/* --- work out which extra partitions we need (Rufus: FormatThread) --- */
	unsigned extra = 0;
	bool windows_to_go = (opt->image_option == RFX_IMOP_WIN_TO_GO) &&
	                     (opt->boot_type == RFX_BT_IMAGE) &&
	                     img != NULL && img->is_windows_img;

	if (img != NULL && opt->write_mode == RFX_WRITE_AS_ISO &&
	    img->uses_casper && opt->persistence_size > 0)
		extra |= RFX_XP_PERSISTENCE;

	if (windows_to_go && opt->target_type == RFX_TT_UEFI && style == RFX_PS_GPT)
		extra |= RFX_XP_ESP | RFX_XP_MSR;
	else if (opt->boot_type == RFX_BT_UEFI_NTFS ||
	         (img != NULL && opt->write_mode == RFX_WRITE_AS_ISO &&
	          img->has_efi != 0 &&
	          (opt->fs_type == RFX_FS_NTFS || opt->fs_type == RFX_FS_EXFAT))) {
		extra |= RFX_XP_UEFI_NTFS;
		/* ...unless it is a Windows installer in BIOS mode without dual boot. */
		if (img != NULL && img->has_bootmgr && !windows_to_go &&
		    opt->target_type == RFX_TT_BIOS && !opt->allow_dual_uefi_bios)
			extra &= ~RFX_XP_UEFI_NTFS;
	}
	if (opt->old_bios_fixes)
		extra |= RFX_XP_COMPAT;

	int pi = 0;
	int role_of[RFX_MAX_PARTITIONS];
	int mi;

	/* Offset of the first partition. Rufus uses the Microsoft 1 MB wastage
	 * unless "Fixes for old BIOSes" is on, in which case it aligns to a
	 * doubled track size so that GRUB2 still fits in the gap. */
	uint64_t first_offset;
	if (style == RFX_PS_GPT || !opt->old_bios_fixes) {
		first_offset = 1 * RFX_MB;
	} else {
		first_offset = RFX_CEILING_ALIGN(bytes_per_track, cluster) * 2;
	}

	/* An ESP goes up front when we can (GPT + Windows To Go). */
	if ((extra & RFX_XP_ESP) && style == RFX_PS_GPT) {
		out[pi].Offset = first_offset;
		out[pi].Size   = DEFAULT_ESP_SIZE;
		out[pi].Esp    = true;
		out[pi].TypeGuid = 0;
		g_strlcpy(out[pi].Name, part_name_for_role(PI_ESP), sizeof(out[pi].Name));
		role_of[pi] = PI_ESP;
		first_offset = out[pi].Offset + out[pi].Size;
		first_offset = RFX_CEILING_ALIGN(first_offset, bytes_per_track);
		if (cluster % sector_size == 0)
			first_offset = RFX_FLOOR_ALIGN(first_offset, cluster);
		extra &= ~RFX_XP_ESP;
		pi++;
	}

	if (extra & RFX_XP_MSR) {
		out[pi].Offset = first_offset;
		out[pi].Size   = DEFAULT_MSR_SIZE;
		out[pi].TypeGuid = 1;
		g_strlcpy(out[pi].Name, part_name_for_role(PI_MSR), sizeof(out[pi].Name));
		role_of[pi] = PI_MSR;
		first_offset = out[pi].Offset + out[pi].Size;
		first_offset = RFX_CEILING_ALIGN(first_offset, bytes_per_track);
		if (cluster % sector_size == 0)
			first_offset = RFX_FLOOR_ALIGN(first_offset, cluster);
		extra &= ~RFX_XP_MSR;
		pi++;
	}

	/* Reserve the main partition slot now; its size is computed last. */
	mi = pi;
	role_of[pi] = PI_MAIN;
	g_strlcpy(out[pi].Name, part_name_for_role(PI_MAIN), sizeof(out[pi].Name));
	out[pi].Offset = first_offset;
	pi++;

	/* Extra partitions are stacked at the *end* of the drive. */
	if (extra) {
		if (extra & RFX_XP_PERSISTENCE) {
			role_of[pi] = PI_CASPER;
			g_strlcpy(out[pi].Name, part_name_for_role(PI_CASPER), sizeof(out[pi].Name));
			out[pi].Size = RFX_CEILING_ALIGN(opt->persistence_size, bytes_per_track);
			out[pi].TypeGuid = 4;
			pi++;
		}
		if (extra & RFX_XP_UEFI_NTFS) {
			role_of[pi] = PI_UEFI_NTFS;
			g_strlcpy(out[pi].Name, part_name_for_role(PI_UEFI_NTFS), sizeof(out[pi].Name));
			out[pi].Size = RFX_CEILING_ALIGN(uefi_ntfs_size, bytes_per_track);
			out[pi].TypeGuid = 2;
			out[pi].NoAutomount = true;
			pi++;
		} else if (extra & RFX_XP_COMPAT) {
			role_of[pi] = PI_COMPAT;
			g_strlcpy(out[pi].Name, part_name_for_role(PI_COMPAT), sizeof(out[pi].Name));
			out[pi].Size = bytes_per_track;		/* exactly one track */
			out[pi].Hidden = true;
			out[pi].TypeGuid = 5;
			pi++;
		}
	}

	/* Walk the extras backwards to give them their final offsets, exactly
	 * like Rufus does. */
	uint64_t last_offset = drive->DiskSize;
	if (style == RFX_PS_GPT)
		last_offset -= (uint64_t)GPT_RESERVED_TAIL * sector_size;

	for (int i = pi - 1; i > mi; i--) {
		if (out[i].Size >= last_offset) {
			rfx_set_error(ERR_IMAGE_TOO_LARGE, 0,
			              "The requested extra partitions do not fit on the device");
			return 0;
		}
		out[i].Offset = RFX_FLOOR_ALIGN(last_offset - out[i].Size, bytes_per_track);
		last_offset = out[i].Offset;
	}

	if (last_offset <= out[mi].Offset) {
		rfx_set_error(ERR_IMAGE_TOO_LARGE, 0, "No room left for the main partition");
		return 0;
	}
	out[mi].Size = RFX_FLOOR_ALIGN(last_offset - out[mi].Offset, bytes_per_track);
	if (cluster % sector_size == 0)
		out[mi].Size = RFX_FLOOR_ALIGN(out[mi].Size, cluster);

	if (out[mi].Size == 0) {
		rfx_set_error(ERR_IMAGE_TOO_LARGE, 0, "Invalid main partition size");
		return 0;
	}

	/* The main data partition is Microsoft Basic Data on GPT and, on MBR,
	 * gets its type byte from the file system (patched up afterwards by
	 * rfx_disk_fixup_mbr()). It must NOT be marked as an ESP: Rufus is
	 * explicit about this, because a second ESP on the disk makes the
	 * Windows installer fail at "Copying Windows Files". */
	out[mi].TypeGuid = 3;
	out[mi].Boot = (opt->boot_type != RFX_BT_NON_BOOTABLE);
	g_strlcpy(out[mi].Name, opt->label[0] ? opt->label : part_name_for_role(PI_MAIN),
	          sizeof(out[mi].Name));

	(void)role_of;
	return pi;
}

/* ----------------------------------------------------------------------- */
/* libparted glue                                                          */
/*                                                                         */
/* libparted is the closest Linux analogue of what Rufus does with          */
/* IOCTL_DISK_CREATE_DISK + IOCTL_DISK_SET_DRIVE_LAYOUT_EX: it owns the       */
/* MBR and GPT serialisation, the backup GPT and the protective MBR.         */
/* ----------------------------------------------------------------------- */
static void parted_ex_handler(PedException *e)
{
	/* libparted's default handler aborts the process on a fatal error.
	 * We want the error to surface in our log instead. */
	uprintfe("parted: %s", e != NULL ? e->message : "unknown error");
}

static PedExceptionOption parted_ex_cb(PedException *e)
{
	parted_ex_handler(e);
	return PED_EXCEPTION_CANCEL;
}

#ifdef DEBUG
/* ped_debug_set_handler() only exists in a DEBUG build of libparted. */
static void parted_debug_cb(int level, const char *file, int line,
                            const char *function, const char *msg)
{
	(void)level; (void)file; (void)line; (void)function;
	uprintfd("parted: %s", msg);
}
#endif

static void parted_init_once(void)
{
	static gsize once = 0;
	if (g_once_init_enter(&once)) {
		ped_exception_set_handler(parted_ex_cb);
#ifdef DEBUG
		ped_debug_set_handler(parted_debug_cb);
#endif
		g_once_init_leave(&once, 1);
	}
}

static const char* ped_table_name(int style)
{
	return style == RFX_PS_GPT ? "gpt" : "msdos";
}

/* File system hint, so that libparted writes the right MBR type byte and
 * GPT type GUID for us instead of guessing "unknown". */
static const PedFileSystemType* ped_fs_hint(int type_guid)
{
	switch (type_guid) {
	case 0:  return ped_file_system_type_get("fat32");	/* ESP        */
	case 1:  return ped_file_system_type_get("fat32");	/* MSR        */
	case 2:  return ped_file_system_type_get("ntfs");	/* UEFI:NTFS  */
	case 3:  return ped_file_system_type_get("fat32");	/* main data  */
	case 4:  return ped_file_system_type_get("ext2");	/* persistence*/
	default: return NULL;
	}
}

static bool ped_set_flag_if_available(PedPartition *part, PedPartitionFlag flag)
{
	if (!ped_partition_is_flag_available(part, flag))
		return false;
	return ped_partition_set_flag(part, flag, 1) != 0;
}

static PedPartition* ped_add_partition(PedDisk *disk, uint64_t offset, uint64_t size,
                                       const char *name, int type_guid, int style,
                                       bool boot, bool esp, bool hidden, bool no_automount)
{
	PedDevice *dev = disk->dev;
	PedSector ss = dev->sector_size;
	PedSector start = (PedSector)(offset / (uint64_t)ss);
	PedSector end   = (PedSector)((offset + size) / (uint64_t)ss) - 1;
	if (end <= start) {
		uprintfe("Invalid partition geometry: start=%lld end=%lld",
		         (long long)start, (long long)end);
		return NULL;
	}

	PedPartition *part = ped_partition_new(disk, PED_PARTITION_NORMAL,
	                                       ped_fs_hint(type_guid), start, end);
	if (part == NULL) {
		uprintfe("ped_partition_new() failed for '%s'", name ? name : "?");
		return NULL;
	}

	if (name != NULL && ped_disk_type_check_feature(disk->type,
	                                                PED_DISK_TYPE_PARTITION_NAME))
		ped_partition_set_name(part, name);	/* GPT only; no-op on msdos */

	/* Flags. libparted maps these onto the MBR type byte and the GPT GUID,
	 * which is exactly what Rufus does by hand in CreatePartition(). */
	if (esp || type_guid == 0) {
		ped_set_flag_if_available(part, PED_PARTITION_ESP);
		ped_set_flag_if_available(part, PED_PARTITION_BOOT);
	}
	if (type_guid == 1)
		ped_set_flag_if_available(part, PED_PARTITION_MSFT_RESERVED);
	if (type_guid == 2 || no_automount) {
		/* Rufus sets GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER on the
		 * UEFI:NTFS partition; the Linux equivalent is the "hidden"
		 * attribute, which also keeps automounters away. */
		ped_set_flag_if_available(part, PED_PARTITION_HIDDEN);
	}
	if (hidden)
		ped_set_flag_if_available(part, PED_PARTITION_HIDDEN);
	if (boot && style != RFX_PS_GPT)
		ped_set_flag_if_available(part, PED_PARTITION_BOOT);

	/* Try the exact geometry first: Rufus' alignment decisions are
	 * deliberate, and letting libparted snap them would move the ESP. */
	PedGeometry geom;
	bool ok = false;
	if (ped_geometry_init(&geom, dev, start, end - start + 1)) {
		PedConstraint *exact = ped_constraint_exact(&geom);
		if (exact != NULL) {
			ok = ped_disk_add_partition(disk, part, exact) != 0;
			ped_constraint_destroy(exact);
		}
	}
	if (!ok) {
		PedConstraint *any = ped_constraint_any(dev);
		ok = ped_disk_add_partition(disk, part, any) != 0;
		ped_constraint_destroy(any);
		if (ok)
			uprintfw("Partition '%s' was realigned by libparted",
			         name ? name : "?");
	}
	if (!ok)
		ped_partition_destroy(part);

	return ok ? part : NULL;
}

bool rfx_partition_create(const RfxDrive *drive, int partition_style,
                          const RfxPartSpec *parts, int n_parts, bool dry_run)
{
	g_return_val_if_fail(drive != NULL && parts != NULL && n_parts > 0, false);
	parted_init_once();

	static const char *style_name[] = { "SFD", "MBR", "GPT" };
	const char *sname = (partition_style >= 0 && partition_style <= 2)
	                        ? style_name[partition_style] : "?";
	uprintf("Creating %s partition table on %s", sname, drive->DevicePath);

	char size_buf[32];
	for (int i = 0; i < n_parts; i++) {
		uprintf("● Creating %s%s (offset: %" G_GUINT64_FORMAT ", size: %s)",
		        parts[i].Name,
		        strstr(parts[i].Name, "Partition") == NULL ? " Partition" : "",
		        parts[i].Offset,
		        rfx_human_size(parts[i].Size, size_buf, sizeof(size_buf)));
	}

	if (dry_run) {
		uprintf("[dry-run] partition table not written");
		return true;
	}

	/* Superfloppy: no table at all, just clobber whatever is there. */
	if (partition_style == RFX_PS_SFD) {
		PedDevice *dev = ped_device_get(drive->DevicePath);
		if (dev == NULL)
			return rfx_fail(ERR_OPEN_FAILED, "libparted cannot open %s",
			                drive->DevicePath);
		if (!ped_device_open(dev)) {
			ped_device_destroy(dev);
			return rfx_fail(ERR_OPEN_FAILED, "Cannot open %s: %s",
			                drive->DevicePath, strerror(errno));
		}
		ped_disk_clobber(dev);
		ped_device_close(dev);
		ped_device_destroy(dev);
		rfx_dev_repartition_notify(drive->DevicePath);
		return true;
	}

	PedDevice *dev = ped_device_get(drive->DevicePath);
	if (dev == NULL)
		return rfx_fail(ERR_OPEN_FAILED, "libparted cannot open %s", drive->DevicePath);
	if (!ped_device_open(dev)) {
		ped_device_destroy(dev);
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open %s: %s",
		                drive->DevicePath, strerror(errno));
	}

	PedDiskType *dtype = ped_disk_type_get(ped_table_name(partition_style));
	if (dtype == NULL) {
		ped_device_close(dev);
		ped_device_destroy(dev);
		return rfx_fail(ERR_INTERNAL, "libparted has no '%s' label support",
		                ped_table_name(partition_style));
	}

	PedDisk *disk = ped_disk_new_fresh(dev, dtype);
	if (disk == NULL) {
		ped_device_close(dev);
		ped_device_destroy(dev);
		return rfx_fail(ERR_PARTITION_FAILURE, "Could not create a fresh %s table",
		                ped_table_name(partition_style));
	}

	/* Rufus writes its own MBR_UEFI_MARKER into the disk signature when the
	 * user picks MBR + UEFI, so that it can re-select the same scheme next
	 * time the stick is plugged in. We do the same after the commit. */
	bool ok = true;
	for (int i = 0; i < n_parts && ok; i++) {
		if (ped_add_partition(disk, parts[i].Offset, parts[i].Size, parts[i].Name,
		                      parts[i].TypeGuid, partition_style, parts[i].Boot,
		                      parts[i].Esp, parts[i].Hidden,
		                      parts[i].NoAutomount) == NULL) {
			uprintfe("Could not add partition '%s' at offset %" G_GUINT64_FORMAT
			         " (size %" G_GUINT64_FORMAT ")",
			         parts[i].Name, parts[i].Offset, parts[i].Size);
			ok = false;
		}
	}

	if (ok && !ped_disk_commit(disk)) {
		rfx_set_error(ERR_PARTITION_FAILURE, 0, "Could not commit the partition table");
		ok = false;
	}

	ped_disk_destroy(disk);
	ped_device_close(dev);
	ped_device_destroy(dev);

	if (!ok) return false;

	/* Zero the head of every new partition (Rufus: ClearPartition), so that
	 * stale superblocks from a previous layout cannot be resurrected. */
	int fd = rfx_open_block(drive->DevicePath, true);
	if (fd >= 0) {
		uint32_t ss = drive->SectorSize ? drive->SectorSize : 512;
		size_t clear_len = (size_t)MAX_SECTORS_TO_CLEAR * ss;
		g_autofree uint8_t *zeros = g_malloc0(clear_len);
		rfx_dev_repartition_notify(drive->DevicePath);
		for (int i = 0; i < n_parts; i++) {
			size_t n = (size_t)MIN((uint64_t)clear_len, parts[i].Size);
			if (!rfx_write_exact(fd, zeros, n, parts[i].Offset))
				uprintfw("Could not zero the head of '%s': %s",
				         parts[i].Name, strerror(errno));
		}
		rfx_sync_block(fd);
		close(fd);
	}

	rfx_dev_repartition_notify(drive->DevicePath);
	return true;
}

/* ----------------------------------------------------------------------- */
/* MBR-level fixups Rufus performs *after* creating the table              */
/* ----------------------------------------------------------------------- */
/*
 * Rufus: WriteMBR(). Two things matter for a bootable stick:
 *   - the LBA partition type of the main FAT partition (0x0C for FAT32 LBA,
 *     0x0E for FAT16 LBA), because FormatEx strips it,
 *   - the boot indicator, plus the WinPE "masquerade as second disk" flag 0x81.
 * libparted sets 0x0B/0x06, so we patch it up here.
 */
bool rfx_disk_fixup_mbr(int fd, int fs_type, bool bootable, bool masquerade,
                        uint32_t sector_size);

bool rfx_disk_fixup_mbr(int fd, int fs_type, bool bootable, bool masquerade,
                        uint32_t sector_size)
{
	if (sector_size < 512) sector_size = 512;
	g_autofree uint8_t *buf = g_malloc0(sector_size);
	if (!rfx_read_exact(fd, buf, sector_size, 0))
		return rfx_fail(ERR_READ_FAULT, "Could not read the MBR");

	if (buf[510] != 0x55 || buf[511] != 0xAA)
		uprintfw("No boot marker in the MBR - continuing anyway");

	/* First primary entry lives at 0x1BE; its type byte is at +4. */
	uint8_t *e = buf + 0x1BE;
	switch (fs_type) {
	case RFX_FS_FAT16:
		if (e[4] == 0x0E) uprintf("Partition is already FAT16 LBA...");
		else if (e[4] != 0x04 && e[4] != 0x06)
			uprintfw("WARNING: converting a non FAT16 partition to FAT16 LBA: FS type=0x%02x", e[4]);
		e[4] = 0x0E;
		break;
	case RFX_FS_FAT32:
		if (e[4] == 0x0C) uprintf("Partition is already FAT32 LBA...");
		else if (e[4] != 0x0B)
			uprintfw("WARNING: converting a non FAT32 partition to FAT32 LBA: FS type=0x%02x", e[4]);
		e[4] = 0x0C;
		break;
	default:
		break;
	}

	if (bootable) {
		e[0] = masquerade ? 0x81 : 0x80;
		uprintf("Set bootable USB partition as 0x%02X", e[0]);
	}

	if (!rfx_write_exact(fd, buf, sector_size, 0))
		return rfx_fail(ERR_WRITE_FAULT, "Could not write the MBR");
	rfx_sync_block(fd);
	return true;
}

/* Rufus writes a "protective MBR" with its own signature when the target is
 * GPT, so that it can remember which scheme the user picked. */
bool rfx_disk_set_mbr_signature(int fd, uint32_t signature);
bool rfx_disk_set_mbr_signature(int fd, uint32_t signature)
{
	uint8_t sig[4] = {
		(uint8_t)(signature & 0xFF),
		(uint8_t)((signature >> 8) & 0xFF),
		(uint8_t)((signature >> 16) & 0xFF),
		(uint8_t)((signature >> 24) & 0xFF),
	};
	if (!rfx_write_exact(fd, sig, 4, 440))
		return rfx_fail(ERR_WRITE_FAULT, "Could not set the MBR signature");
	return true;
}
