/*
 * rufox — image analysis
 *
 * Port of Rufus' ImageScanThread()/check_iso_props() (src/iso.c) plus the
 * compressed-image detection Rufus does in bled/. Rufus bundles its own fork
 * of libcdio under src/libcdio; we link against the system libcdio/libiso9660
 * and libudf, which is the same code lineage.
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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cdio/cdio.h>
#include <cdio/ds.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>
#include <cdio/udf_file.h>
#include <cdio/rock.h>

#define ISO_BLOCKSIZE 2048
#define ISO_EXT_MASK (ISO_EXTENSION_ROCK_RIDGE | ISO_EXTENSION_JOLIET)

static const char* const syslinux_cfg[] = {
	"isolinux.cfg", "syslinux.cfg", "extlinux.conf", "pxelinux.cfg"
};
static const char* const grub_dirname[] = { "/boot/grub", "/boot/grub2", "/grub", "/grub2" };
static const char* const wininst_name[] = {
	"install.wim", "install.esd", "install.swm", "install.wtm",
	"install2.swm", "install3.swm", "install4.swm", "install5.swm"
};
static const char* const md5sum_name[] = { "md5sum.txt", "MD5SUMS", "md5sum" };
static const char* const pe_dirname[] = { "/i386", "/amd64", "/minint" };
static const char* const pe_file[] = { "ntdetect.com", "setupldr.bin", "txtsetup.sif" };
static const char* const reactos_name[] = { "freeldr.sys", "setupldr.sys" };
static const char* const efi_bootname[] = { "boot", "grub", "mm", "bootmgr" };
static const char* const efi_archname[] = { "x64", "ia32", "aa64", "arm", "riscv64", "loongarch64" };

/* ----------------------------------------------------------------------- */
/* Rufus' safe_str*cmp() equivalents                                       */
/* ----------------------------------------------------------------------- */
/* Rufus' safe_stricmp() tolerates NULL. Here the callers always pass either a
 * literal or a local buffer, so a plain strcasecmp is equivalent and does not
 * trip -Waddress on "will always evaluate as true". */
#define safe_stricmp(a, b)      strcasecmp(a, b)
#define safe_strnicmp(a, b, n)  strncasecmp(a, b, n)

static bool ends_with_ci(const char *s, const char *suffix)
{
	if (s == NULL || suffix == NULL) return false;
	size_t ls = strlen(s), lf = strlen(suffix);
	return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

/* dirname/basename of an ISO-internal path, without allocation.
 * For "/foo/bar.txt" -> dir="/foo", base="bar.txt".
 * For "/bar.txt"     -> dir="",     base="bar.txt"  (Rufus uses "" for root) */
static void split_iso_path(const char *full, char *dir, size_t dir_size,
                           char *base, size_t base_size)
{
	const char *slash = strrchr(full, '/');
	if (slash == NULL) {
		g_strlcpy(dir, "", dir_size);
		g_strlcpy(base, full, base_size);
	} else if (slash == full) {
		g_strlcpy(dir, "", dir_size);
		g_strlcpy(base, full + 1, base_size);
	} else {
		size_t n = (size_t)(slash - full);
		if (n >= dir_size) n = dir_size - 1;
		memcpy(dir, full, n);
		dir[n] = '\0';
		g_strlcpy(base, slash + 1, base_size);
	}
}

/* ----------------------------------------------------------------------- */
/* Compression sniffing (Rufus: bled/)                                     */
/* ----------------------------------------------------------------------- */
static int detect_compression(const char *path)
{
	g_autofree char *lower = g_ascii_strdown(path, -1);
	int type = RFX_BLED_NONE;

	if (g_str_has_suffix(lower, ".gz") || g_str_has_suffix(lower, ".tgz"))
		type = RFX_BLED_GZIP;
	else if (g_str_has_suffix(lower, ".bz2")) type = RFX_BLED_BZIP2;
	else if (g_str_has_suffix(lower, ".xz"))  type = RFX_BLED_XZ;
	else if (g_str_has_suffix(lower, ".lzma"))type = RFX_BLED_LZMA;
	else if (g_str_has_suffix(lower, ".zst")) type = RFX_BLED_ZSTD;
	else if (g_str_has_suffix(lower, ".vhd") || g_str_has_suffix(lower, ".vhdx"))
		type = RFX_BLED_VHD;

	/* Also honour the magic bytes: plenty of images are misnamed. */
	uint8_t magic[8] = { 0 };
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t r = read(fd, magic, sizeof(magic));
		close(fd);
		if (r == (ssize_t)sizeof(magic)) {
			if (magic[0] == 0x1F && magic[1] == 0x8B)         type = RFX_BLED_GZIP;
			else if (memcmp(magic, "BZh", 3) == 0)            type = RFX_BLED_BZIP2;
			else if (memcmp(magic, "\xFD" "7zXZ", 6) == 0)    type = RFX_BLED_XZ;
			else if (memcmp(magic, "\x5D" "\x00" "\x00", 3) == 0) type = RFX_BLED_LZMA;
			else if (memcmp(magic, "\x28\xB5\x2F\xFD", 4) == 0) type = RFX_BLED_ZSTD;
			else if (memcmp(magic, "conectix", 8) == 0)       type = RFX_BLED_VHD;
		}
	}
	return type;
}

const char* rfx_compression_name(int type)
{
	switch (type) {
	case RFX_BLED_NONE:  return "none";
	case RFX_BLED_BZIP2: return "bzip2";
	case RFX_BLED_LZMA:  return "LZMA";
	case RFX_BLED_XZ:    return "xz";
	case RFX_BLED_GZIP:  return "gzip";
	case RFX_BLED_LZMA + 100: return "LZMA";
	case RFX_BLED_ZSTD:  return "zstd";
	case RFX_BLED_VHD:   return "VHD";
	default:             return "unknown";
	}
}

/* ----------------------------------------------------------------------- */
/* Hybrid / DD image detection                                             */
/* ----------------------------------------------------------------------- */
static bool looks_like_iso(int fd)
{
	uint8_t pvd[2048];
	if (pread(fd, pvd, sizeof(pvd), 32768) == (ssize_t)sizeof(pvd))
		if (pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0)
			return true;

	/* UDF: the AVDP sits 256 sectors in. */
	uint8_t avdp[512];
	if (pread(fd, avdp, sizeof(avdp), (off_t)256 * 2048) == (ssize_t)sizeof(avdp))
		if (avdp[0] == 2 && memcmp(avdp + 1, "NSR0", 4) == 0)
			return true;

	return false;
}

static bool looks_like_hybrid(int fd)
{
	/* xorriso's isohybrid images carry a valid MBR whose first partition is
	 * bootable and starts at LBA 0, 1 or 64. */
	uint8_t mbr[512];
	if (pread(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr)) return false;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
	uint8_t *e = mbr + 0x1BE;
	if ((e[0] & 0x80) == 0) return false;
	uint32_t lba;
	memcpy(&lba, e + 8, 4);
	return lba == 0 || lba == 1 || lba == 64;
}

/* ----------------------------------------------------------------------- */
/* The scan context — Rufus fills img_report through check_iso_props()     */
/* ----------------------------------------------------------------------- */
typedef struct {
	RfxImgReport *rep;
	uint64_t      total_blocks;
	char          isolinux_path[256];
	char          config_path[256];
	uint8_t       has_md5sum;
	bool          has_ldlinux_c32;
	bool          has_unattend;
} ScanCtx;

static void add_efi_entry(RfxImgReport *rep, int type, const char *full)
{
	for (int j = 0; j < RFX_MAX_EFI_BOOT; j++) {
		if (rep->efi_boot_entry[j].path[0] == '\0') {
			rep->efi_boot_entry[j].type = type;
			g_strlcpy(rep->efi_boot_entry[j].path, full,
			          sizeof(rep->efi_boot_entry[j].path));
			rep->efi_boot_entry_count = j + 1;
			return;
		}
	}
}

static void scan_props(const char *dirname, int64_t file_length,
                       const char *basename, const char *fullpath, ScanCtx *ctx)
{
	RfxImgReport *rep = ctx->rep;
	char bootloader_name[64];
	bool at_root = (dirname[0] == '\0');

	/* isolinux/syslinux config anywhere */
	for (guint i = 0; i < G_N_ELEMENTS(syslinux_cfg); i++) {
		if (safe_stricmp(basename, syslinux_cfg[i]) == 0) {
			if (ctx->config_path[0] == '\0')
				g_strlcpy(ctx->config_path, fullpath, sizeof(ctx->config_path));
			if (rep->cfg_path[0] == '\0')
				g_strlcpy(rep->cfg_path, fullpath, sizeof(rep->cfg_path));
		}
	}

	/* GRUB2 */
	for (guint i = 0; i < G_N_ELEMENTS(grub_dirname); i++)
		if (safe_stricmp(dirname, grub_dirname[i]) == 0)
			rep->has_grub2 = (uint8_t)i + 1;

	if (safe_stricmp(basename, "ldlinux.c32") == 0)
		ctx->has_ldlinux_c32 = true;

	/* Ubuntu-family live images live under /casper */
	if (safe_strnicmp(dirname, "/casper", 7) == 0) {
		rep->uses_casper = true;
		if (g_ascii_strncasecmp(dirname, "/casper/pop", 11) == 0 ||
		    strstr(dirname, "pop_os") != NULL)
			rep->disable_iso = true;	/* Pop!_OS must be DD-written */
	}
	/* Proxmox and Manjaro refuse to boot from an extracted tree too */
	if (safe_stricmp(dirname, "/proxmox") == 0)
		rep->disable_iso = true;

	if (at_root) {
		if (safe_stricmp(basename, "bootmgr") == 0)
			rep->has_bootmgr = true;
		if (safe_stricmp(basename, "bootmgr.efi") == 0) {
			add_efi_entry(rep, RFX_EBT_BOOTMGR, fullpath);
			rep->has_efi |= 1;
			rep->has_bootmgr_efi = true;
		}
		if (safe_stricmp(basename, "grldr") == 0)
			rep->has_grub4dos = true;
		if (safe_stricmp(basename, "manjaro") == 0 ||
		    safe_stricmp(basename, "manjaro.iso") == 0 ||
		    safe_stricmp(basename, "livecd.sfs") == 0)
			rep->disable_iso = true;
		for (guint i = 0; i < G_N_ELEMENTS(md5sum_name); i++)
			if (safe_stricmp(basename, md5sum_name[i]) == 0)
				ctx->has_md5sum = (uint8_t)(i + 1);
	}

	/* ReactOS */
	if (rep->reactos_path[0] == '\0')
		for (guint i = 0; i < G_N_ELEMENTS(reactos_name); i++)
			if (safe_stricmp(basename, reactos_name[i]) == 0)
				g_strlcpy(rep->reactos_path, fullpath, sizeof(rep->reactos_path));

	/* El-Torito EFI image */
	if (rep->efi_img_path[0] == '\0' && strlen(basename) >= 7 &&
	    safe_strnicmp(basename, "efi", 3) == 0 && ends_with_ci(basename, ".img"))
		g_strlcpy(rep->efi_img_path, fullpath, sizeof(rep->efi_img_path));

	/* Lenovo UEFI firmware-update ISOs use an emulated HDD image */
	if (rep->efi_img_path[0] == '\0' &&
	    safe_stricmp(fullpath, "/[BOOT]/0-Boot-HardDisk.img") == 0)
		g_strlcpy(rep->efi_img_path, fullpath, sizeof(rep->efi_img_path));

	/* EFI boot entries. Rufus checks anything below /efi/, not just
	 * /efi/boot/, because Fedora uses /efi/fedora/ and friends. */
	if (safe_strnicmp(dirname, "/efi/", 5) == 0 || safe_stricmp(dirname, "/efi") == 0) {
		for (guint k = 0; k < G_N_ELEMENTS(efi_bootname); k++) {
			for (guint i = 0; i < G_N_ELEMENTS(efi_archname); i++) {
				g_snprintf(bootloader_name, sizeof(bootloader_name),
				           "%s%s.efi", efi_bootname[k], efi_archname[i]);
				if (safe_stricmp(basename, bootloader_name) == 0) {
					if (k == 0)
						rep->has_efi |= (2u << i);
					add_efi_entry(rep, (int)k, fullpath);
				}
			}
		}
		/* Linux Mint ships a bootx64.efi that is a dangling symlink
		 * (< 256 bytes); Rufus falls back to the El-Torito image. */
		if (safe_stricmp(basename, "bootx64.efi") == 0 && file_length < 256) {
			rep->has_efi |= 0x4000;
			g_strlcpy(rep->efi_img_path, "[BOOT]/1-Boot-NoEmul.img",
			          sizeof(rep->efi_img_path));
		}
	}

	/* Windows install images live in /sources/ */
	if (dirname != NULL && (safe_stricmp(dirname, "/sources") == 0 ||
	                        ends_with_ci(dirname, "/sources"))) {
		for (guint i = 0; i < G_N_ELEMENTS(wininst_name); i++) {
			if (safe_stricmp(basename, wininst_name[i]) == 0) {
				if (rep->wininst_index < RFX_MAX_WININST) {
					g_snprintf(rep->wininst_path[rep->wininst_index],
					           sizeof(rep->wininst_path[0]), "%s", fullpath);
					rep->wininst_index++;
				}
				rep->is_windows_img = true;
				if (file_length >= 4LL * 1024 * 1024 * 1024)
					rep->has_4GB_file = true;
			}
		}
	}

	/* sources/$OEM$/$$/Panther/unattend.xml */
	if (safe_stricmp(dirname, "/sources/$OEM$/$$/Panther") == 0 &&
	    safe_stricmp(basename, "unattend.xml") == 0)
		ctx->has_unattend = true;

	/* WinPE markers (Rufus: winpe bitmask) */
	for (guint i = 0; i < G_N_ELEMENTS(pe_dirname); i++)
		if (safe_stricmp(dirname, pe_dirname[i]) == 0)
			for (guint j = 0; j < G_N_ELEMENTS(pe_file); j++)
				if (safe_stricmp(basename, pe_file[j]) == 0) {
					rep->winpe_seen = true;
					if (j == 1 && (i == 2)) rep->uses_minint = true;
				}

	if (safe_stricmp(basename, "isolinux.bin") == 0 ||
	    safe_stricmp(basename, "isolinux-debug.bin") == 0) {
		if (ctx->isolinux_path[0] == '\0')
			g_strlcpy(ctx->isolinux_path, fullpath, sizeof(ctx->isolinux_path));
	}

	/* Deep directories and long names matter for FAT targets */
	if (file_length >= 4LL * 1024 * 1024 * 1024)
		rep->has_4GB_file = true;
	if (strlen(basename) > 8 && strchr(basename, '.') != strrchr(basename, '.'))
		rep->has_long_filename = true;
	{
		int slashes = 0;
		for (const char *p = fullpath; *p; p++) if (*p == '/') slashes++;
		if (slashes > 8) rep->has_deep_directories = true;
	}

	if (file_length != 0)
		ctx->total_blocks += (uint64_t)((file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
}

/* ----------------------------------------------------------------------- */
/* Tree walkers                                                            */
/* ----------------------------------------------------------------------- */
static void walk_iso9660(iso9660_t *iso, const char *psz_path, ScanCtx *ctx, int depth)
{
	if (depth > 40) return;		/* guard against a cyclic image */

	CdioList_t *entlist = iso9660_ifs_readdir(iso, psz_path);
	if (entlist == NULL) return;

	for (CdioListNode_t *node = _cdio_list_begin(entlist); node != NULL;
	     node = _cdio_list_node_next(node)) {
		iso9660_stat_t *st = (iso9660_stat_t*)_cdio_list_node_data(node);
		if (st == NULL) continue;

		const char *name = st->filename;
		if (name == NULL || safe_stricmp(name, ".") == 0 || safe_stricmp(name, "..") == 0)
			continue;

		char *full = g_strdup_printf("%s/%s", psz_path, name);
		if (full[0] == '/' && full[1] == '/')
			memmove(full, full + 1, strlen(full));

		char dir[256], base[256];
		split_iso_path(full, dir, sizeof(dir), base, sizeof(base));

		time_t mtime = mktime(&st->tm);
		(void)mtime;
		scan_props(dir, (int64_t)st->size, base, full, ctx);

		if (st->type == _STAT_DIR)
			walk_iso9660(iso, full, ctx, depth + 1);

		g_free(full);
	}
	_cdio_list_free(entlist, true, (CdioDataFree_t)iso9660_stat_free);
}

static void walk_udf(udf_dirent_t *root, const char *psz_path, ScanCtx *ctx, int depth)
{
	if (root == NULL || depth > 40) return;

	udf_dirent_t *d = udf_opendir(root);
	if (d == NULL) return;

	udf_dirent_t *ent;
	while ((ent = udf_readdir(d)) != NULL) {
		const char *fn = udf_get_filename(ent);
		if (fn == NULL) { udf_dirent_free(ent); continue; }

		char *full = g_strdup_printf("%s/%s", psz_path, fn);
		if (full[0] == '/' && full[1] == '/')
			memmove(full, full + 1, strlen(full));

		char dir[256], base[256];
		split_iso_path(full, dir, sizeof(dir), base, sizeof(base));

		scan_props(dir, (int64_t)udf_get_file_length(ent), base, full, ctx);
		if (udf_is_dir(ent))
			walk_udf(ent, full, ctx, depth + 1);

		g_free(full);
		udf_dirent_free(ent);
	}
	udf_dirent_free(d);
}

/* ----------------------------------------------------------------------- */
/* ISOLINUX version (Rufus: sl_version / sl_version_str)                   */
/* ----------------------------------------------------------------------- */
static void detect_sl_version(iso9660_t *iso, RfxImgReport *rep, const char *isolinux_path)
{
	if (isolinux_path == NULL || isolinux_path[0] == '\0') return;

	iso9660_stat_t *st = iso9660_ifs_stat_translate(iso, isolinux_path);
	if (st == NULL) return;
	uint32_t len = (uint32_t)MIN((uint64_t)st->size, 4096u);
	lsn_t lsn = st->lsn;
	iso9660_stat_free(st);
	if (len == 0) return;

	g_autofree uint8_t *buf = g_malloc(len);
	long got = iso9660_iso_seek_read(iso, buf, lsn, (long)((len + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE));
	if (got < 0) return;
	uint32_t have = (uint32_t)MIN((long)len, got);

	/* ISOLINUX embeds an "ISOLINUX x.yy " banner early in the binary. */
	for (uint32_t i = 0; i + 16 < have; i++) {
		if (memcmp(buf + i, "ISOLINUX ", 9) == 0) {
			char v[12] = { 0 };
			int k = 0;
			for (uint32_t j = i + 9; j < have && k < 10 &&
			     (isdigit(buf[j]) || buf[j] == '.'); j++)
				v[k++] = (char)buf[j];
			if (k > 0) {
				g_strlcpy(rep->sl_version_str, v, sizeof(rep->sl_version_str));
				g_auto(GStrv) parts = g_strsplit(v, ".", 2);
				uint16_t maj = parts[0] ? (uint16_t)atoi(parts[0]) : 0;
				uint16_t min = parts[1] ? (uint16_t)atoi(parts[1]) : 0;
				rep->sl_version = (uint16_t)((maj << 8) | min);
			}
			break;
		}
	}
}

/* ----------------------------------------------------------------------- */
/* Windows version from the WIM header (Rufus: winver_t)                   */
/* ----------------------------------------------------------------------- */
static void detect_windows_version(iso9660_t *iso, const char *iso_path,
                                   RfxImgReport *rep, const char *wim_path)
{
	iso9660_stat_t *st = iso9660_ifs_stat_translate(iso, wim_path);
	if (st == NULL) return;
	lsn_t lsn = st->lsn;
	iso9660_stat_free(st);

	uint8_t hdr[64];
	long got = iso9660_iso_seek_read(iso, hdr, lsn, 1);
	if (got < 64) return;
	if (memcmp(hdr, "MSWIM", 5) != 0) return;

	/* WIM header: version at offset 12 (LE uint32)
	 *   0x0000CE00 -> Windows 7 / Server 2008 R2
	 *   0x0000D800 -> Windows 8
	 *   0x0000E000 -> Windows 8.1
	 *   0x0000E100 -> Windows 10 / 11  */
	uint32_t ver;
	memcpy(&ver, hdr + 12, 4);
	ver = GUINT32_FROM_LE(ver);
	if (ver == 0x0000CE00)       rep->win_version = RFX_WIN_7;
	else if (ver == 0x0000D800)  rep->win_version = RFX_WIN_8;
	else if (ver == 0x0000E000)  rep->win_version = RFX_WIN_81;
	else if (ver >= 0x0000E100)  rep->win_version = RFX_WIN_10;
	else                         rep->win_version = RFX_WIN_VISTA;
	rep->wininst_version = ver;

	/* Rufus distinguishes 10 from 11 by the build number inside the image;
	 * without unpacking the WIM we can only go by the file name. */
	if (rep->win_version == RFX_WIN_10) {
		const char *base = strrchr(iso_path, '/');
		base = base ? base + 1 : iso_path;
		if (strstr(base, "11") != NULL || strstr(base, "win11") != NULL ||
		    strstr(base, "Win11") != NULL)
			rep->win_version = RFX_WIN_11;
	}
}

/* ----------------------------------------------------------------------- */
/* Public entry point (Rufus: ImageScanThread)                             */
/* ----------------------------------------------------------------------- */
bool rfx_iso_scan(const char *path, RfxImgReport *rep, RfxCancel *cancel)
{
	g_return_val_if_fail(path != NULL && rep != NULL, false);
	memset(rep, 0, sizeof(*rep));
	g_strlcpy(rep->ImagePath, path, sizeof(rep->ImagePath));

	struct stat st;
	if (stat(path, &st) != 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot stat '%s': %s", path, strerror(errno));
	rep->image_size = (uint64_t)st.st_size;

	rep->compression_type = detect_compression(path);
	if (rep->compression_type != RFX_BLED_NONE) {
		uprintf("Detected a %s-compressed image", rfx_compression_name(rep->compression_type));
		/* Rufus decompresses on the fly through bled. We decompress to a
		 * temporary file first: same result, no partial-write risk. */
		rep->disable_iso = true;
		rep->is_bootable_img = true;
		rep->projected_size = rep->image_size;
		return true;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s': %s", path, strerror(errno));
	bool iso = looks_like_iso(fd);
	bool hybrid = looks_like_hybrid(fd);
	close(fd);

	rep->is_iso = iso;
	rep->is_bootable_img = hybrid;
	rep->is_vhd = ends_with_ci(path, ".vhd") || ends_with_ci(path, ".vhdx");

	g_autofree char *base = g_path_get_basename(path);

	if (!iso) {
		uprintf("Using image: %s (%s, raw/DD mode)", base,
		        hybrid ? "hybrid" : "not an ISO");
		rep->disable_iso = true;
		rep->projected_size = rep->image_size;
		g_strlcpy(rep->label, base, sizeof(rep->label));
		char *dot = strrchr(rep->label, '.');
		if (dot != NULL) *dot = '\0';
		for (char *p = rep->label; *p != '\0'; p++)
			*p = (char)g_ascii_toupper(*p);
		return true;
	}

	uprintf("Using image: %s", base);

	ScanCtx ctx = { 0 };
	ctx.rep = rep;

	/* UDF first: Rufus prefers it when present (Windows 11 media, macOS). */
	udf_t *udf = udf_open(path);
	if (udf != NULL) {
		char volid[256] = { 0 };
		if (udf_get_volume_id(udf, volid, sizeof(volid)) == 0 && volid[0] != '\0') {
			uprintf("ISO has a UDF file system, volume id '%s'", volid);
			g_strlcpy(rep->label, g_strstrip(volid), sizeof(rep->label));
		} else {
			uprintf("ISO has a UDF file system");
		}
		udf_dirent_t *root = udf_get_root(udf, true, 0);
		if (root == NULL) root = udf_get_root(udf, false, 0);
		if (root != NULL) {
			walk_udf(root, "", &ctx, 0);
			udf_dirent_free(root);
		}
		udf_close(udf);
	}

	iso9660_t *ifs = iso9660_open_ext(path, ISO_EXT_MASK);
	if (ifs == NULL) {
		if (rep->label[0] != '\0' && ctx.total_blocks > 0) {
			/* UDF-only image: we already walked it. */
			goto summarise;
		}
		return rfx_fail(ERR_NOT_AN_IMAGE, "libcdio cannot open '%s'", path);
	}

	if (iso9660_ifs_is_xa(ifs))
		uprintfd("ISO is XA (Rock Ridge / Joliet)");

	if (rep->label[0] == '\0') {
		cdio_utf8_t *volid = NULL;
		if (iso9660_ifs_get_volume_id(ifs, &volid) && volid != NULL) {
			g_strlcpy(rep->label, g_strstrip(volid), sizeof(rep->label));
			uprintf("ISO volume id: '%s'", rep->label);
			free(volid);
		}
	}

	walk_iso9660(ifs, "/", &ctx, 0);
	detect_sl_version(ifs, rep, ctx.isolinux_path);
	if (rep->sl_version != 0)
		uprintf("Detected: ISOLINUX %s", rep->sl_version_str);

	if (rep->is_windows_img && rep->wininst_index > 0)
		detect_windows_version(ifs, path, rep, rep->wininst_path[0]);

	iso9660_close(ifs);

summarise:
	/* Rufus' projected_size is the number of 2 KB blocks the extracted tree
	 * will occupy; the mismatch against image_size is what it warns about. */
	rep->projected_size = ctx.total_blocks * ISO_BLOCKSIZE;
	if (rep->projected_size == 0)
		rep->projected_size = rep->image_size;
	rep->mismatch_size = (int64_t)rep->projected_size - (int64_t)rep->image_size;

	/* The label Rufus proposes for the USB stick (SetProposedLabel) */
	{
		char usb_label[192];
		rfx_format_label_valid((RfxFs)RFX_FS_FAT32, rep->label, usb_label, sizeof(usb_label));
		if (usb_label[0] == '\0')
			g_strlcpy(usb_label, "RUFOX", sizeof(usb_label));
		g_strlcpy(rep->usb_label, usb_label, sizeof(rep->usb_label));
	}

	/* --- the report, in Rufus' own wording --- */
	if (rep->has_efi & 1) uprintf("Bootable UEFI: bootmgr.efi");
	for (int i = 0; i < rep->efi_boot_entry_count; i++)
		uprintfd("  UEFI bootloader: %s (%s)", rep->efi_boot_entry[i].path,
		         rep->efi_boot_entry[i].type == RFX_EBT_MAIN ? "main" :
		         rep->efi_boot_entry[i].type == RFX_EBT_GRUB ? "GRUB" :
		         rep->efi_boot_entry[i].type == RFX_EBT_MOKMANAGER ? "MokManager" : "bootmgr");
	if (rep->has_bootmgr)       uprintf("Detected: Windows bootloader (BIOS)");
	if (rep->has_grub2)         uprintf("Detected: GRUB2");
	if (rep->has_grub4dos)      uprintf("Detected: Grub4DOS");
	if (rep->reactos_path[0])   uprintf("Detected: ReactOS (%s)", rep->reactos_path);
	if (rep->uses_casper)       uprintf("Detected: Ubuntu-style live image (persistence possible)");
	if (ctx.has_md5sum)         uprintf("Detected: an md5sum file (integrity check available)");
	if (ctx.has_unattend)       uprintf("Detected: a Panther unattend.xml");
	if (rep->is_windows_img)    uprintf("Detected: Windows %s installation media",
	                                    rep->win_version == RFX_WIN_11 ? "11" :
	                                    rep->win_version == RFX_WIN_10 ? "10" :
	                                    rep->win_version == RFX_WIN_81 ? "8.1" :
	                                    rep->win_version == RFX_WIN_8 ? "8" :
	                                    rep->win_version == RFX_WIN_7 ? "7" : "");
	if (rep->disable_iso)       uprintf("This image must be written in DD mode");
	if (rep->has_4GB_file)      uprintf("Image contains a file >= 4 GB: FAT32 is not an option");

	{
		char sz[32], pz[32];
		uprintf("Image size: %s, projected size: %s",
		        rfx_human_size(rep->image_size, sz, sizeof(sz)),
		        rfx_human_size(rep->projected_size, pz, sizeof(pz)));
	}

	if (rfx_cancel_requested(cancel))
		return rfx_fail(ERR_CANCELLED, "Cancelled by user");

	return true;
}
