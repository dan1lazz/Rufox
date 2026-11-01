/*
 * rufox — bootloader installation
 *
 * Rufus embeds its bootloader payloads as Win32 resources (src/res/): the
 * syslinux MBRs from ms-sys, ldlinux.sys, the GRUB2 images and uefi-ntfs.img.
 * On Linux the same payloads are already installed by the distribution:
 *
 *   /usr/lib/syslinux/mbr/mbr.bin        SYSLINUX MBR (Rufus: ms-sys' mbr)
 *   /usr/lib/syslinux/mbr/gptmbr.bin     SYSLINUX GPT MBR
 *   /usr/lib/syslinux/mbr/altmbr.bin     alternate MBR
 *   /usr/lib/ISOLINUX/isohdpfx.bin       hybrid ISO prefix
 *   /usr/lib/grub/i386-pc                GRUB2 BIOS modules
 *   /usr/lib/grub/x86_64-efi             GRUB2 UEFI modules
 *
 * rfx_boot_find_resource() resolves a payload by name across the usual
 * locations, which keeps the code working on Debian, Fedora and Arch without
 * a bundled copy of every binary (and keeps us honest about licensing).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "rufox-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Candidate roots, in the order Rufus' resource lookup effectively resolves. */
static const char *const resource_roots[] = {
	"/usr/lib/syslinux/mbr",
	"/usr/lib/syslinux/mbr/extra",
	"/usr/lib/syslinux/bios",
	"/usr/lib/ISOLINUX",
	"/usr/lib/ISOLINUX/extra",
	"/usr/share/syslinux",
	"/usr/lib/grub",
	"/usr/share/rufox",
	RUFOX_DATADIR,
	NULL
};

const char* rfx_boot_find_resource(const char *name)
{
	static char found[512];
	if (name == NULL) return NULL;

	/* Absolute paths win (the GUI lets a user point at a custom payload). */
	if (name[0] == '/' && g_file_test(name, G_FILE_TEST_EXISTS)) {
		g_strlcpy(found, name, sizeof(found));
		return found;
	}

	for (int i = 0; resource_roots[i] != NULL; i++) {
		g_autofree char *p = g_build_filename(resource_roots[i], name, NULL);
		if (g_file_test(p, G_FILE_TEST_EXISTS)) {
			g_strlcpy(found, p, sizeof(found));
			return found;
		}
	}

	/* Some distros (Fedora, Arch) put the MBRs directly under syslinux/. */
	g_autoptr(GDir) dir = NULL;
	for (int i = 0; resource_roots[i] != NULL; i++) {
		dir = g_dir_open(resource_roots[i], 0, NULL);
		if (dir == NULL) continue;
		const char *ent;
		while ((ent = g_dir_read_name(dir)) != NULL) {
			if (strcmp(ent, name) == 0) {
				g_snprintf(found, sizeof(found), "%s/%s", resource_roots[i], ent);
				g_dir_close(dir);
				dir = NULL;
				return found;
			}
		}
		g_dir_close(dir);
		dir = NULL;
	}
	return NULL;
}

static bool read_resource(const char *name, uint8_t **buf, size_t *len)
{
	const char *path = rfx_boot_find_resource(name);
	if (path == NULL) {
		rfx_set_error(ERR_MISSING_TOOL, 0,
		              "Bootloader payload '%s' not found. Install 'syslinux-common' "
		              "or 'isolinux'.", name);
		return false;
	}
	if (!g_file_get_contents(path, (char**)buf, len, NULL)) {
		rfx_set_error(ERR_OPEN_FAILED, errno, "Cannot read '%s'", path);
		return false;
	}
	uprintfd("Loaded bootloader payload '%s' (%zu bytes) from %s", name, *len, path);
	return true;
}

/* ----------------------------------------------------------------------- */
/* SYSLINUX MBR (Rufus: write_syslinux_mbr / ms-sys)                       */
/* ----------------------------------------------------------------------- */
bool rfx_boot_install_syslinux_mbr(int fd, int partition_style, uint32_t sector_size)
{
	const char *payload = (partition_style == RFX_PS_GPT) ? "gptmbr.bin" : "mbr.bin";
	uint8_t *code = NULL;
	size_t len = 0;

	if (!read_resource(payload, &code, &len))
		return false;

	uprintf("Using SYSLINUX %s MBR", partition_style == RFX_PS_GPT ? "GPT" : "");
	bool ok = rfx_disk_write_mbr_bootcode(fd, code, len, sector_size, 0x80);
	g_free(code);
	if (!ok)
		return rfx_fail(ERR_WRITE_FAULT, "Could not write the SYSLINUX MBR: %s",
		                rfx_last_error_message());
	return true;
}

/* ----------------------------------------------------------------------- */
/* SYSLINUX on a FAT partition                                             */
/* ----------------------------------------------------------------------- */
bool rfx_boot_install_syslinux(const char *devpart, const char *mountpoint,
                               const char *cfg_source_dir)
{
	/* The upstream `syslinux` installer writes ldlinux.sys and patches its
	 * sector map into the FAT; doing that by hand is the one piece of Rufus
	 * we deliberately do not reimplement, because getting the sector map
	 * wrong produces a stick that fails to boot in ways that are very hard
	 * to diagnose. Use the distro tool when it is there. */
	GStrv syslinux = rfx_find_tool("syslinux", NULL);
	if (syslinux != NULL) {
		GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(argv, g_strdup(syslinux[0]));
		g_ptr_array_add(argv, g_strdup("-i"));	/* install the MBR too */
		if (mountpoint != NULL) {
			g_ptr_array_add(argv, g_strdup("-d"));
			g_ptr_array_add(argv, g_strdup("/"));
		}
		g_ptr_array_add(argv, g_strdup(devpart));
		g_ptr_array_add(argv, NULL);
		g_strfreev(syslinux);

		uprintf("Installing SYSLINUX on %s", devpart);
		GError *err = NULL;
		bool ok = rfx_run_ok((GStrv)argv->pdata, NULL, &err);
		g_ptr_array_free(argv, TRUE);
		if (ok) return true;
		uprintfw("The syslinux installer failed: %s",
		         err && err->message ? err->message : "unknown error");
		g_clear_error(&err);
		/* Fall through to the MBR-only path below. */
	} else {
		uprintfw("'syslinux' is not installed: only the MBR will be written. "
		         "Install the 'syslinux' package for full BIOS support.");
	}

	/* extlinux covers ext2/3/4 targets, which is what GRUB2-mode sticks use. */
	if (rfx_boot_install_extlinux(devpart, mountpoint))
		return true;

	/* Last resort: at least make the MBR chainload into the partition. */
	int fd = rfx_open_block(devpart, true);
	if (fd < 0)
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s': %s", devpart, strerror(errno));
	bool ok = rfx_boot_install_syslinux_mbr(fd, RFX_PS_MBR, 512);
	close(fd);

	if (cfg_source_dir != NULL && mountpoint != NULL) {
		g_autofree char *dst = g_build_filename(mountpoint, "syslinux.cfg", NULL);
		g_autofree char *src = g_build_filename(cfg_source_dir, "syslinux.cfg", NULL);
		if (g_file_test(src, G_FILE_TEST_EXISTS)) {
			GError *e = NULL;
			if (!g_file_test(src, G_FILE_TEST_IS_DIR))
				g_file_get_contents(src, NULL, NULL, NULL);
			g_autoptr(GFile) gs = g_file_new_for_path(src);
			g_autoptr(GFile) gd = g_file_new_for_path(dst);
			if (!g_file_copy(gs, gd, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &e)) {
				uprintfw("Could not copy '%s': %s", src, e ? e->message : "?");
				g_clear_error(&e);
			}
		}
	}
	return ok;
}

bool rfx_boot_install_extlinux(const char *devpart, const char *mountpoint)
{
	GStrv ext = rfx_find_tool("extlinux", NULL);
	if (ext == NULL || mountpoint == NULL) {
		if (ext != NULL) g_strfreev(ext);
		return false;
	}

	g_autofree char *bootdir = g_build_filename(mountpoint, "extlinux", NULL);
	if (g_mkdir_with_parents(bootdir, 0755) != 0) {
		g_strfreev(ext);
		return false;
	}

	uprintf("Installing EXTLINUX into %s", bootdir);
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(ext[0]));
	g_strfreev(ext);
	g_ptr_array_add(argv, g_strdup("--install"));
	g_ptr_array_add(argv, g_strdup(bootdir));
	g_ptr_array_add(argv, NULL);

	GError *err = NULL;
	bool ok = rfx_run_ok((GStrv)argv->pdata, NULL, &err);
	g_ptr_array_free(argv, TRUE);
	if (!ok) {
		uprintfw("extlinux failed: %s", err && err->message ? err->message : "?");
		g_clear_error(&err);
	}

	/* The MBR still has to chainload into the partition. */
	int fd = rfx_open_block(devpart, true);
	if (fd >= 0) {
		rfx_boot_install_syslinux_mbr(fd, RFX_PS_MBR, 512);
		close(fd);
	}
	(void)devpart;
	return ok;
}

/* ----------------------------------------------------------------------- */
/* GRUB2 (Rufus: BT_GRUB2, installed via its embedded grub2 payload)       */
/* ----------------------------------------------------------------------- */
bool rfx_boot_install_grub2(const char *devpath, int target_type,
                            const char *label, const char *boot_dir)
{
	GStrv install = rfx_find_tool(target_type == RFX_TT_UEFI ? "grub-install"
	                                                         : "grub-install", NULL);
	if (install == NULL)
		return rfx_fail(ERR_MISSING_TOOL,
		                "grub-install is required for a GRUB2 boot entry");

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(install[0]));
	g_strfreev(install);

	if (target_type == RFX_TT_UEFI) {
		g_ptr_array_add(argv, g_strdup("--target=x86_64-efi"));
		g_ptr_array_add(argv, g_strdup("--efi-directory"));
		g_ptr_array_add(argv, g_strdup(boot_dir ? boot_dir : "/mnt"));
		g_ptr_array_add(argv, g_strdup("--removable"));	/* no NVRAM entry */
		g_ptr_array_add(argv, g_strdup("--no-nvram"));
	} else {
		g_ptr_array_add(argv, g_strdup("--target=i386-pc"));
		if (boot_dir != NULL) {
			g_ptr_array_add(argv, g_strdup("--boot-directory"));
			g_autofree char *bd = g_build_filename(boot_dir, "boot", NULL);
			g_ptr_array_add(argv, g_strdup(bd));
		}
		g_ptr_array_add(argv, g_strdup("--force"));
	}
	if (label != NULL && *label != '\0') {
		g_ptr_array_add(argv, g_strdup("--label"));
		g_ptr_array_add(argv, g_strdup(label));
	}
	g_ptr_array_add(argv, g_strdup(devpath));
	g_ptr_array_add(argv, NULL);

	uprintf("Installing GRUB2 (%s) on %s",
	        target_type == RFX_TT_UEFI ? "UEFI" : "BIOS", devpath);
	GError *err = NULL;
	bool ok = rfx_run_ok((GStrv)argv->pdata, NULL, &err);
	g_ptr_array_free(argv, TRUE);
	if (!ok) {
		rfx_set_error(ERR_MISSING_TOOL, 0, "grub-install failed: %s",
		              err && err->message ? err->message : "unknown error");
		g_clear_error(&err);
	}
	return ok;
}

/* ----------------------------------------------------------------------- */
/* UEFI:NTFS shim (Rufus: uefi-ntfs.img, from pbatard/uefi-ntfs)           */
/* ----------------------------------------------------------------------- */
bool rfx_boot_write_uefi_ntfs(const char *devpart, uint64_t part_size)
{
	const char *img = rfx_boot_find_resource("uefi-ntfs.img");
	if (img == NULL) {
		return rfx_fail(ERR_MISSING_TOOL,
		                "uefi-ntfs.img not found. It ships with Rufus as an embedded "
		                "resource; on Linux drop it into %s or /usr/share/rufox.",
		                RUFOX_DATADIR);
	}

	char *content = NULL;
	gsize len = 0;
	if (!g_file_get_contents(img, &content, &len, NULL))
		return rfx_fail(ERR_OPEN_FAILED, "Cannot read '%s'", img);

	if ((uint64_t)len > part_size) {
		g_free(content);
		return rfx_fail(ERR_IMAGE_TOO_LARGE,
		                "uefi-ntfs.img (%" G_GSIZE_FORMAT " bytes) does not fit in "
		                "the %" G_GUINT64_FORMAT "-byte partition", len, part_size);
	}

	int fd = rfx_open_block(devpart, true);
	if (fd < 0) {
		g_free(content);
		return rfx_fail(ERR_OPEN_FAILED, "Cannot open '%s': %s", devpart, strerror(errno));
	}

	uprintf("Writing UEFI:NTFS data (%" G_GSIZE_FORMAT " bytes)...", len);
	bool ok = rfx_write_exact(fd, content, len, 0);
	rfx_sync_block(fd);
	close(fd);
	g_free(content);

	if (!ok)
		return rfx_fail(ERR_WRITE_FAULT, "Could not write the UEFI:NTFS partition");
	return true;
}

/* ----------------------------------------------------------------------- */
/* FreeDOS (Rufus downloads and unpacks a FreeDOS image on demand)         */
/* ----------------------------------------------------------------------- */
bool rfx_boot_freedos_available(void)
{
	return rfx_boot_find_resource("freedos.img") != NULL ||
	       rfx_boot_find_resource("FreeDOS.img") != NULL;
}

/* ----------------------------------------------------------------------- */
/* El-Torito boot catalog / ISO hybridisation helpers                      */
/* ----------------------------------------------------------------------- */
bool rfx_boot_isohybrid(const char *iso_path, const char *out_path);
bool rfx_boot_isohybrid(const char *iso_path, const char *out_path)
{
	GStrv tool = rfx_find_tool("isohybrid", NULL);
	if (tool == NULL)
		return rfx_fail(ERR_MISSING_TOOL, "isohybrid is not installed (syslinux-utils)");

	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(tool[0]));
	g_strfreev(tool);
	g_ptr_array_add(argv, g_strdup("--uefi"));
	g_ptr_array_add(argv, g_strdup("-o"));
	g_ptr_array_add(argv, g_strdup(out_path));
	g_ptr_array_add(argv, g_strdup(iso_path));
	g_ptr_array_add(argv, NULL);

	bool ok = rfx_run_ok((GStrv)argv->pdata, NULL, NULL);
	g_ptr_array_free(argv, TRUE);
	return ok;
}
