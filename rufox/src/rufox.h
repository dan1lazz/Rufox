/*
 * rufox: Rufus, the Reliable USB Formatting Utility — Linux port
 * Copyright © 2026 rufox contributors
 *
 * Derived from Rufus by Pete Batard <pete@akeo.ie>
 * https://github.com/pbatard/rufus
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RUFOX_H
#define RUFOX_H

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

G_BEGIN_DECLS

#ifndef RUFOX_VERSION_STR
#define RUFOX_VERSION_STR "4.15.0"
#endif

/* Keep the enum ordinals identical to Rufus' src/rufus.h so that logs,
 * settings files and translations stay interchangeable. */

/* ----------------------------------------------------------------------- */
/* Progress operations (Rufus: enum action_type)                           */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_OP_NOOP = -2,
	RFX_OP_INIT = -1,
	RFX_OP_ANALYZE_MBR = 0,
	RFX_OP_BADBLOCKS,
	RFX_OP_ZERO_MBR,
	RFX_OP_PARTITION,
	RFX_OP_FORMAT,
	RFX_OP_CREATE_FS,
	RFX_OP_FIX_MBR,
	RFX_OP_FILE_COPY,
	RFX_OP_PATCH,
	RFX_OP_FINALIZE,
	RFX_OP_EXTRACT_ZIP,
	RFX_OP_VERIFY,
	RFX_OP_MAX
} RfxOp;

/* ----------------------------------------------------------------------- */
/* File systems (Rufus: enum fs_type)                                      */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_FS_UNKNOWN = -1,
	RFX_FS_FAT16 = 0,
	RFX_FS_FAT32,
	RFX_FS_NTFS,
	RFX_FS_UDF,
	RFX_FS_EXFAT,
	RFX_FS_REFS,
	RFX_FS_EXT2,
	RFX_FS_EXT3,
	RFX_FS_EXT4,
	RFX_FS_MAX
} RfxFs;

/* ----------------------------------------------------------------------- */
/* Boot type (Rufus: enum boot_type)                                       */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_BT_NON_BOOTABLE = 0,
	RFX_BT_MSDOS,
	RFX_BT_FREEDOS,
	RFX_BT_IMAGE,
	RFX_BT_SYSLINUX_V4,
	RFX_BT_SYSLINUX_V6,
	RFX_BT_REACTOS,
	RFX_BT_GRUB4DOS,
	RFX_BT_GRUB2,
	RFX_BT_UEFI_NTFS,
	RFX_BT_MAX
} RfxBootType;

/* ----------------------------------------------------------------------- */
/* Target system (Rufus: enum target_type)                                 */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_TT_BIOS = 0,	/* BIOS or UEFI-CSM */
	RFX_TT_UEFI,		/* UEFI (non CSM)      */
	RFX_TT_MAX
} RfxTarget;

/* ----------------------------------------------------------------------- */
/* Partition scheme. Rufus reuses Microsoft's PARTITION_STYLE_* values,    */
/* where RAW == 0, MBR == 1, GPT == 2, and SFD is aliased onto RAW.       */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_PS_SFD = 0,		/* Superfloppy: no partition table at all */
	RFX_PS_MBR = 1,
	RFX_PS_GPT = 2,
} RfxPartitionScheme;

/* Extra partitions (Rufus: XP_*) */
typedef enum {
	RFX_XP_NONE       = 0,
	RFX_XP_ESP        = 1 << 0,
	RFX_XP_MSR        = 1 << 1,
	RFX_XP_PERSISTENCE= 1 << 2,
	RFX_XP_UEFI_NTFS  = 1 << 3,
	RFX_XP_COMPAT     = 1 << 4,	/* "Fixes for old BIOSes" */
} RfxExtraPart;

/* Image option (Rufus: enum image_option_type) */
typedef enum {
	RFX_IMOP_WIN_STANDARD = 0,
	RFX_IMOP_WIN_EXTENDED,
	RFX_IMOP_WIN_TO_GO,
	RFX_IMOP_MAX
} RfxImageOption;

/* Image mode: ISO extraction vs. bit-for-bit DD */
typedef enum {
	RFX_WRITE_AS_ISO = 0,	/* extract the ISO contents, install a bootloader */
	RFX_WRITE_AS_DD,	/* raw image write (Rufus' "write_as_image")   */
} RfxWriteMode;

/* Hash algorithms (Rufus: enum hash_type) */
typedef enum {
	RFX_HASH_NONE = -1,
	RFX_HASH_MD5 = 0,
	RFX_HASH_SHA1,
	RFX_HASH_SHA256,
	RFX_HASH_SHA512,
	RFX_HASH_MAX
} RfxHashType;

/* Compression of a DD image (Rufus: BLED_COMPRESSION_*) */
typedef enum {
	RFX_BLED_NONE = 0,
	RFX_BLED_BZIP2,
	RFX_BLED_LZMA,
	RFX_BLED_XZ,
	RFX_BLED_GZIP,
	RFX_BLED_ZSTD,
	RFX_BLED_VHD,
	RFX_BLED_MAX
} RfxCompression;

/* Bad-block check mode (Rufus: badblocks) */
typedef enum {
	RFX_BB_DISABLED = 0,
	RFX_BB_QUICK,
	RFX_BB_SLOW,
	RFX_BB_NONDESTRUCTIVE,
} RfxBadBlocksMode;

/* Job result */
typedef enum {
	RFX_RESULT_NONE = 0,
	RFX_RESULT_SUCCESS,
	RFX_RESULT_FAILED,
	RFX_RESULT_CANCELLED,
} RfxResult;

/* ----------------------------------------------------------------------- */
/* Windows version detected inside an install image (subset of winver_t)   */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_WIN_UNDEFINED = 0,
	RFX_WIN_XP,
	RFX_WIN_2003,	/* Also covers XP 64 */
	RFX_WIN_VISTA,
	RFX_WIN_2008,
	RFX_WIN_7,
	RFX_WIN_8,
	RFX_WIN_81,
	RFX_WIN_10,
	RFX_WIN_11,
	RFX_WIN_MAX
} RfxWindowsVersion;

/* ----------------------------------------------------------------------- */
/* Drive description (Rufus: RUFUS_DRIVE / SelectedDrive)                  */
/* ----------------------------------------------------------------------- */
#define RFX_MAX_PARTITIONS 16

typedef struct {
	uint64_t Offset;
	uint64_t Size;
	char     Name[64];
	char     FsName[32];
	bool     IsEsp;
	bool     IsBoot;
} RfxPartition;

typedef struct {
	/* Identity */
	char     DevicePath[128];	/* /dev/sdb                      */
	char     DeviceId[64];		/* kernel name = basename(devpath): "sdb",
					 * "nvme0n1", "dm-0". NOT a by-id symlink
					 * target: rfx_dev_is_filtered() matches the
					 * dm-/md/ram/zram prefixes against this field,
					 * so putting a real by-id string here would
					 * silently disable virtual-device filtering
					 * (docs/REVIEW.md M6). */
	char     FriendlyName[256];	/* "SanDisk Ultra (sdb)"         */
	char     Vendor[64];
	char     Model[128];
	char     Serial[64];
	char     BusName[32];		/* usb / mmc / sd / nvme / virtio*/
	char     Hub[128];
	uint32_t Index;			/* our own enumeration index      */

	/* Geometry */
	uint64_t DiskSize;
	uint32_t SectorSize;		/* logical  */
	uint32_t PhysSectorSize;	/* physical */
	uint32_t SectorsPerTrack;
	uint32_t Heads;

	/* Attributes */
	bool     IsRemovable;
	bool     IsBootDevice;		/* contains the running system's root */
	bool     IsOptical;
	bool     IsFloppy;
	bool     IsLoop;
	bool     IsSynthetic;		/* image-file target used for testing */
	bool     HasMedia;
	bool     IsWholeDiskReadOnly;
	bool     IsSystemInternal;	/* udisks2 HintSystem */
	int      RotationRate;		/* 0 = unknown, -1 = SSD, >0 = HDD    */
	char     MountPoint[256];	/* first mountpoint, if any           */

	/* Layout cache */
	int              PartitionStyle;	/* RFX_PS_* */
	RfxPartition     Partition[RFX_MAX_PARTITIONS];
	int              PartitionCount;

	/* UDisks2 object paths (NULL when udisks2 is unavailable) */
	char* udisks_drive_object;
	char* udisks_block_object;
} RfxDrive;

/* ----------------------------------------------------------------------- */
/* Image report (Rufus: RUFUS_IMG_REPORT)                                  */
/* ----------------------------------------------------------------------- */
#define RFX_MAX_WININST    8
#define RFX_MAX_EFI_BOOT   64

typedef struct {
	int      type;			/* RFX_EBT_* */
	uint16_t arch;
	char     path[256];
} RfxEfiBootEntry;

typedef enum {
	RFX_EBT_MAIN = 0,
	RFX_EBT_GRUB,
	RFX_EBT_MOKMANAGER,
	RFX_EBT_BOOTMGR,
} RfxEfiBootType;

typedef struct {
	char     ImagePath[1024];
	uint64_t image_size;
	uint64_t projected_size;	/* bytes that will actually land on the target */
	int64_t  mismatch_size;

	bool     is_iso;
	bool     is_bootable_img;	/* DD-able hybrid image            */
	bool     is_vhd;
	bool     is_windows_img;	/* install.wim / install.esd       */
	bool     disable_iso;		/* force DD mode                   */
	bool     rh8_derivative;

	int      compression_type;	/* RFX_BLED_* */

	uint16_t has_efi;
	uint8_t  has_secureboot_bootloader;
	bool     has_bootmgr;
	bool     has_bootmgr_efi;
	bool     has_autorun;
	bool     has_4GB_file;
	bool     has_symlinks;
	bool     has_long_filename;
	bool     has_deep_directories;
	bool     uses_casper;		/* Ubuntu-style live image -> persistence */
	bool     uses_minint;
	bool     winpe_seen;		/* i386/amd64/minint PE markers present */
	bool     has_grub4dos;
	uint8_t  has_grub2;
	char     grub2_version[192];
	char     wininst_path[RFX_MAX_WININST][256];
	uint8_t  wininst_index;
	uint32_t wininst_version;
	int      win_version;		/* RfxWindowsVersion */

	uint16_t sl_version;		/* syslinux/isolinux version */
	char     sl_version_str[12];

	char     label[192];		/* ISO volume id            */
	char     usb_label[192];	/* label safe for the target FS */
	char     cfg_path[128];		/* isolinux.cfg               */
	char     reactos_path[128];
	char     efi_img_path[128];
	RfxEfiBootEntry efi_boot_entry[RFX_MAX_EFI_BOOT];
	int      efi_boot_entry_count;

	/* Populated by iso_extract() */
	uint32_t file_count;
	uint64_t dir_count;
	/* Set when the on-disk tree was patched (Rufus: modified_files) */
	uint32_t patched_files;
} RfxImgReport;

/* ----------------------------------------------------------------------- */
/* User options (Rufus: the global settings of the main dialog)            */
/* ----------------------------------------------------------------------- */
typedef struct {
	int      partition_scheme;	/* RFX_PS_*          */
	int      target_type;		/* RFX_TT_*          */
	int      fs_type;		/* RFX_FS_*          */
	uint32_t cluster_size;		/* 0 = default        */
	char     label[64];
	bool     quick_format;
	bool     create_extended_label;	/* Rufus' "Use extended label and icon files" */
	bool     old_bios_fixes;
	bool     uefi_media_validation;	/* runtime UEFI media validation */
	bool     allow_dual_uefi_bios;
	int      boot_type;		/* RFX_BT_*          */
	int      write_mode;		/* RFX_WRITE_AS_*    */
	int      image_option;		/* RFX_IMOP_*        */
	bool     zero_drive;
	bool     fast_zeroing;
	int      badblocks_mode;	/* RFX_BB_*          */
	int      badblocks_passes;
	uint64_t persistence_size;	/* bytes, 0 = disabled */
	bool     fix_win11_requirements;	/* no RAM/SecureBoot/TPM checks */
	bool     no_online_account;
	bool     remove_edge;
	int      hash_type;		/* RFX_HASH_* or RFX_HASH_NONE */
	bool     verify_after_write;
	bool     dry_run;		/* never touch a real device: log only */
	/* Where the ESP ended up being mounted, if the job created one.
	 * Windows To Go needs it to drop the EFI boot files there. */
	const char *esp_mountpoint;
	int      wintogo_index;		/* WIM image index, 1-based */
	char     wintogo_username[64];
} RfxOptions;

/* ----------------------------------------------------------------------- */
/* Progress / logging callback                                             */
/* ----------------------------------------------------------------------- */
typedef struct {
	RfxOp    op;
	float    progress;	/* 0.0 .. 1.0, or -1.0 for "indeterminate" */
	uint64_t bytes_done;
	uint64_t bytes_total;
	double   speed_bps;
	double   eta_seconds;	/* -1 when unknown */
	char     message[256];
} RfxProgress;

typedef void (*RfxProgressCb)(const RfxProgress *p, gpointer user_data);

/* ----------------------------------------------------------------------- */
/* Cancellation token                                                      */
/* ----------------------------------------------------------------------- */
typedef struct _RfxCancel RfxCancel;

RfxCancel* rfx_cancel_new(void);
void       rfx_cancel_request(RfxCancel *c);
bool       rfx_cancel_requested(RfxCancel *c);
void       rfx_cancel_free(RfxCancel *c);
/* Throws a controlled abort out of the current operation. */
#define RFX_CHECK_CANCEL(tok) \
	do { if (rfx_cancel_requested(tok)) return rfx_fail(ERR_CANCELLED, "cancelled by user"); } while (0)

/* ----------------------------------------------------------------------- */
/* Error reporting                                                         */
/* ----------------------------------------------------------------------- */
typedef enum {
	ERR_OK = 0,
	ERR_CANCELLED,
	ERR_NO_DEVICE,
	ERR_DEVICE_BUSY,
	ERR_OPEN_FAILED,
	ERR_PERMISSION,
	ERR_NOT_ENOUGH_MEMORY,
	ERR_WRITE_FAULT,
	ERR_READ_FAULT,
	ERR_PARTITION_FAILURE,
	ERR_FORMAT_FAILURE,
	ERR_UNSUPPORTED_FS,
	ERR_IMAGE_TOO_LARGE,
	ERR_NOT_AN_IMAGE,
	ERR_MISSING_TOOL,
	ERR_VERIFY_MISMATCH,
	ERR_INTERNAL,
	ERR_MAX
} RfxError;

typedef struct {
	RfxError code;
	char     message[512];
	int      sys_errno;
} RfxStatus;

const char* rfx_strerror(RfxError e);
/* Convenience for functions returning bool/RfxError */
bool rfx_fail(RfxError code, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
RfxError rfx_last_error(void);
const char* rfx_last_error_message(void);
void rfx_clear_error(void);
void rfx_set_error(RfxError code, int sys_errno, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

/* ----------------------------------------------------------------------- */
/* rlog.c — Rufus' uprintf()/PrintStatus() replacement                     */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_LOG_DEBUG = 0,
	RFX_LOG_INFO,
	RFX_LOG_WARN,
	RFX_LOG_ERROR,
} RfxLogLevel;

typedef void (*RfxLogCb)(RfxLogLevel lvl, const char *line, gpointer user_data);

void rfx_log_set_level(RfxLogLevel lvl);
void rfx_log_set_callback(RfxLogCb cb, gpointer user_data);
void rfx_log_set_file(const char *path);
void rfx_log(RfxLogLevel lvl, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
#define uprintf(...)  rfx_log(RFX_LOG_INFO,  __VA_ARGS__)
#define uprintfd(...) rfx_log(RFX_LOG_DEBUG, __VA_ARGS__)
#define uprintfw(...) rfx_log(RFX_LOG_WARN,  __VA_ARGS__)
#define uprintfe(...) rfx_log(RFX_LOG_ERROR, __VA_ARGS__)
/* printf-style progress line, like Rufus' uprint_progress() */
void uprint_progress(uint64_t done, uint64_t total);

/* ----------------------------------------------------------------------- */
/* rutil.c                                                                 */
/* ----------------------------------------------------------------------- */
void    rfx_options_default(RfxOptions *o);
char*   rfx_human_size(uint64_t bytes, char *buf, size_t size);
char*   rfx_human_rate(double bps, char *buf, size_t size);
char*   rfx_human_eta(double seconds, char *buf, size_t size);
uint64_t rfx_parse_size(const char *str);	/* "4G", "512M", "1024" */
bool    rfx_is_root(void);
bool    rfx_run_as_root_cmd(GStrv argv, char **stdout_text, GError **error);
int     rfx_open_block(const char *path, bool write);
bool    rfx_sync_block(int fd);
bool    rfx_read_exact(int fd, void *buf, size_t len, uint64_t offset);
bool    rfx_write_exact(int fd, const void *buf, size_t len, uint64_t offset);
char*   rfx_resolve_device(const char *name);	/* "sdb" -> "/dev/sdb" */
bool    rfx_path_is_block_device(const char *path);
GStrv   rfx_find_tool(const char *name, const char *const *extra_paths);
bool    rfx_tool_available(const char *name);
/* CEILING_ALIGN/FLOOR_ALIGN from Rufus */
#define RFX_CEILING_ALIGN(s, a)  (((s) + (a) - 1) & ~((uint64_t)(a) - 1))
#define RFX_FLOOR_ALIGN(s, a)    ((s) & ~((uint64_t)(a) - 1))
#define RFX_KB  1024ULL
#define RFX_MB  (1024ULL * 1024ULL)
#define RFX_GB  (1024ULL * 1024ULL * 1024ULL)
#define RFX_TB  (1024ULL * 1024ULL * 1024ULL * 1024ULL)

/* ----------------------------------------------------------------------- */
/* rproc.c — spawn external helpers (mkfs.*, syslinux, xorriso, …)         */
/* ----------------------------------------------------------------------- */
typedef struct {
	int   exit_status;
	char *std_out;
	char *std_err;
} RfxProcResult;

bool rfx_run(GStrv argv, const char *cwd, RfxProcResult *res, GError **error);
bool rfx_run_ok(GStrv argv, const char *cwd, GError **error); /* discards output */
void rfx_proc_result_clear(RfxProcResult *res);
#define RFX_ARGV(...) ((GStrv)(const char*[]){ __VA_ARGS__, NULL })

/* ----------------------------------------------------------------------- */
/* dev.c — enumeration & hotplug (SetupAPI replacement)                    */
/* ----------------------------------------------------------------------- */
typedef struct _RfxDevList RfxDevList;

RfxDevList* rfx_dev_list_new(void);
void        rfx_dev_list_free(RfxDevList *l);
bool        rfx_dev_list_refresh(RfxDevList *l);
int         rfx_dev_list_count(const RfxDevList *l);
const RfxDrive* rfx_dev_list_get(const RfxDevList *l, int i);
const RfxDrive* rfx_dev_list_find_by_path(const RfxDevList *l, const char *devpath);
/* Fill in a single synthetic drive backed by a regular file (test mode) */
bool        rfx_dev_probe_file(const char *path, RfxDrive *out);
bool        rfx_dev_probe(const char *devpath, RfxDrive *out);
/* Rufus' IsFilteredDrive(): never offer the running system's disk */
bool        rfx_dev_is_filtered(const RfxDrive *d);
/* Reload the partition table of a device so the kernel picks it up */
bool        rfx_dev_repartition_notify(const char *devpath);
/* Unmount everything below a device (Rufus: RemoveDriveLetters) */
bool        rfx_dev_unmount_all(const char *devpath);
GPtrArray*  rfx_dev_mount_points(const char *devpath);

/* ----------------------------------------------------------------------- */
/* mbr.c / partition.c / diskio.c                                          */
/* ----------------------------------------------------------------------- */
bool rfx_disk_analyze_mbr(int fd, const char *target_name);
bool rfx_disk_write_mbr_bootcode(int fd, const uint8_t *code, size_t len,
                                 uint32_t sector_size, uint8_t drive_number);
bool rfx_disk_clear_mbr_gpt(int fd, uint64_t disk_size, uint32_t sector_size);
bool rfx_disk_zero(int fd, uint64_t size, uint32_t sector_size, bool fast,
                   RfxProgressCb cb, gpointer ud, RfxCancel *cancel);
/* The DD path (Rufus: WriteDrive with write_as_image) */
bool rfx_disk_write_image(int src_fd, uint64_t src_size, int dst_fd,
                          uint64_t dst_size, uint32_t sector_size,
                          int compression, RfxProgressCb cb, gpointer ud,
                          RfxCancel *cancel);
bool rfx_disk_verify_image(int src_fd, uint64_t src_size, int dst_fd,
                           uint32_t sector_size, RfxProgressCb cb, gpointer ud,
                           RfxCancel *cancel);
/* Post-partitioning MBR fixups (Rufus: WriteMBR) */
bool rfx_disk_fixup_mbr(int fd, int fs_type, bool bootable, bool masquerade,
                        uint32_t sector_size);
bool rfx_disk_set_mbr_signature(int fd, uint32_t signature);

typedef struct {
	uint64_t Offset;
	uint64_t Size;
	char     Name[64];
	int      TypeGuid;		/* index into our GPT type table */
	bool     Boot;
	bool     Esp;
	bool     Hidden;
	bool     NoAutomount;
} RfxPartSpec;

/* Rufus' CreatePartition(): builds the whole layout, including the extra
 * partitions (ESP / MSR / persistence / UEFI:NTFS / compat). */
bool rfx_partition_create(const RfxDrive *drive, int partition_style,
                          const RfxPartSpec *parts, int n_parts,
                          bool dry_run);
/* Compute a layout the same way Rufus does, without touching the disk.
 * Returns the number of entries written into out[] (<= RFX_MAX_PARTITIONS). */
int  rfx_partition_plan(const RfxDrive *drive, const RfxOptions *opt,
                        const RfxImgReport *img, uint64_t uefi_ntfs_size,
                        RfxPartSpec out[RFX_MAX_PARTITIONS]);

/* ----------------------------------------------------------------------- */
/* rtarget.c — addressing a partition on a device *or* inside an image file */
/* ----------------------------------------------------------------------- */
typedef struct {
	char     DevicePath[128];	/* whole disk / image file          */
	char     PartPath[160];	/* /dev/sdb1, or "<file>@<offset>"  */
	char     LoopDevice[64];	/* loop node backing a file target  */
	int      Index;			/* 1-based partition number, 0 = whole */
	uint64_t Offset;
	uint64_t Size;
	bool     IsFileTarget;
} RfxPartTarget;

RfxPartTarget rfx_part_resolve(const char *devpath, int part_index,
                               uint64_t offset, uint64_t size);
int           rfx_part_open(const RfxPartTarget *t, bool write);
bool          rfx_part_pread(const RfxPartTarget *t, int fd, void *buf, size_t len,
                             uint64_t off);
bool          rfx_part_pwrite(const RfxPartTarget *t, int fd, const void *buf, size_t len,
                              uint64_t off);
bool          rfx_part_zero(const RfxPartTarget *t, int fd, uint64_t len);
bool          rfx_part_mount(RfxPartTarget *t, const char *where,
                             const char *fstype, bool *used_loop);
bool          rfx_part_unmount(RfxPartTarget *t, const char *where);
const char*   rfx_part_tool_path(const RfxPartTarget *t);

/* ----------------------------------------------------------------------- */
/* format.c                                                                */
/* ----------------------------------------------------------------------- */
bool     rfx_format_partition_real(const RfxPartTarget *target, RfxFs fs,
                                   uint32_t cluster_size, const char *label,
                                   bool quick, bool extended_label,
                                   RfxProgressCb cb, gpointer ud, RfxCancel *cancel);
bool     rfx_format_partition(const RfxPartTarget *target, RfxFs fs,
                              uint32_t cluster_size, const char *label,
                              bool quick, bool extended_label,
                              RfxProgressCb cb, gpointer ud, RfxCancel *cancel);
uint32_t rfx_format_default_cluster_size(RfxFs fs, uint64_t part_size);
bool     rfx_format_fs_supported(RfxFs fs);
const char* rfx_format_fs_name(RfxFs fs);
bool     rfx_format_allowed_fs(int partition_style, int target_type,
                               const RfxImgReport *img, RfxFs *out, int *n_out);
bool     rfx_format_label_valid(RfxFs fs, const char *label, char *out, size_t out_size);
bool     rfx_format_write_extended_label(const char *mountpoint, const char *label);
bool     rfx_format_cluster_allowed(RfxFs fs, uint32_t cluster, uint64_t part_size);
uint64_t rfx_format_free_space(const char *mountpoint);

/* ----------------------------------------------------------------------- */
/* iso_scan.c / iso_extract.c                                              */
/* ----------------------------------------------------------------------- */
bool rfx_iso_scan(const char *path, RfxImgReport *report, RfxCancel *cancel);
bool rfx_iso_extract(const char *iso, const char *dest, RfxImgReport *report,
                     const RfxOptions *opt, RfxProgressCb cb, gpointer ud,
                     RfxCancel *cancel);
bool rfx_iso_extract_file(const char *iso, const char *iso_file,
                          const char *dest_file);
bool rfx_iso_read_file_to_buffer(const char *iso, const char *iso_file,
                                 uint8_t **buf, uint32_t *size);
const char* rfx_compression_name(int type);

/* ----------------------------------------------------------------------- */
/* bootloaders.c                                                           */
/* ----------------------------------------------------------------------- */
typedef enum {
	RFX_BL_NONE = 0,
	RFX_BL_SYSLINUX,
	RFX_BL_EXTLINUX,
	RFX_BL_GRUB2_BIOS,
	RFX_BL_GRUB2_UEFI,
	RFX_BL_FREEDOS,
	RFX_BL_CHAINLOAD,
} RfxBootloader;

bool rfx_boot_install_syslinux_mbr(int fd, int partition_style, uint32_t sector_size);
bool rfx_boot_install_syslinux(const char *devpart, const char *mountpoint,
                               const char *cfg_source_dir);
bool rfx_boot_install_extlinux(const char *devpart, const char *mountpoint);
bool rfx_boot_install_grub2(const char *devpath, int target_type,
                            const char *label, const char *boot_dir);
bool rfx_boot_write_uefi_ntfs(const char *devpart, uint64_t part_size);
bool rfx_boot_isohybrid(const char *iso_path, const char *out_path);
bool rfx_boot_freedos_available(void);
const char* rfx_boot_find_resource(const char *name);

/* ----------------------------------------------------------------------- */
/* hash.c / badblocks.c / persistence.c / wintogo.c                        */
/* ----------------------------------------------------------------------- */
bool rfx_hash_file(const char *path, RfxHashType type, char *out_hex,
                   size_t out_size, RfxProgressCb cb, gpointer ud,
                   RfxCancel *cancel);
bool rfx_hash_match_sumfile(const char *sumfile, const char *target_basename,
                            RfxHashType type, const char *computed_hex);
const char* rfx_hash_name(RfxHashType t);
size_t rfx_hash_hex_length(RfxHashType t);

typedef struct {
	uint64_t blocks_tested;
	uint64_t blocks_corrupted;
	GArray  *corrupted;	/* uint64_t block numbers */
} RfxBadBlocksReport;

bool rfx_badblocks_check(const char *devpath, uint64_t size, uint32_t block_size,
                         RfxBadBlocksMode mode, int passes,
                         RfxBadBlocksReport *report,
                         RfxProgressCb cb, gpointer ud, RfxCancel *cancel);

bool rfx_persistence_create(const RfxPartTarget *target, uint64_t size, const char *label);
bool rfx_persistence_supported(const RfxImgReport *img);
const char* rfx_persistence_label(const RfxImgReport *img);

bool rfx_wintogo_supported(const RfxImgReport *img);
GPtrArray* rfx_wintogo_list_editions(const char *wim_path);
bool rfx_wintogo_create(const char *devpart, const RfxImgReport *img,
                        const RfxOptions *opt, RfxProgressCb cb, gpointer ud,
                        RfxCancel *cancel);

/* ----------------------------------------------------------------------- */
/* job.c — the orchestration engine (Rufus' FormatThread)                  */
/* ----------------------------------------------------------------------- */
typedef struct {
	/* inputs */
	char       DevicePath[128];
	char       ImagePath[1024];
	RfxOptions Options;
	/* resolved at start */
	RfxDrive     Drive;
	RfxImgReport Image;
	/* outputs */
	RfxResult  Result;
	char       ErrorMessage[512];
	char       ImageHash[256];
	double     ElapsedSeconds;
	double     AverageSpeed;
} RfxJob;

typedef struct _RfxJobHandle RfxJobHandle;

RfxJobHandle* rfx_job_new(void);
void          rfx_job_free(RfxJobHandle *h);
void          rfx_job_set_device(RfxJobHandle *h, const char *devpath);
void          rfx_job_set_image(RfxJobHandle *h, const char *image);
void          rfx_job_set_options(RfxJobHandle *h, const RfxOptions *opt);
void          rfx_job_set_progress_cb(RfxJobHandle *h, RfxProgressCb cb, gpointer ud);
RfxJob*       rfx_job_peek(RfxJobHandle *h);
/* Blocking. Run this in a worker thread. */
RfxResult     rfx_job_run(RfxJobHandle *h, RfxCancel *cancel);
/* The single "what will happen" preview shown in the confirmation dialog */
char*         rfx_job_describe(RfxJobHandle *h);

G_END_DECLS

#endif /* RUFOX_H */
