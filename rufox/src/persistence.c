/*
 * rufox — Linux persistence partition
 *
 * Port of the persistence path in Rufus' src/format.c (FormatThread) and
 * src/format_ext.c. The recipe is distro-specific and Rufus is explicit
 * about it:
 *
 *   Ubuntu & derivatives (uses_casper): ext3, label "casper-rw", no conf file.
 *   Debian & derivatives:               ext3, label "persistence", plus a
 *                                       "persistence.conf" containing "/ union".
 *
 * Rufus gates this behind HAS_PERSISTENCE(), which is
 *   (HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || HAS_REACTOS(r))
 * i.e. only for Linux live media with a bootloader we understand.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mount.h>
#include <glib/gstdio.h>

bool rfx_persistence_supported(const RfxImgReport *img);
bool rfx_persistence_supported(const RfxImgReport *img)
{
	if (img == NULL) return false;
	if (img->is_windows_img || img->reactos_path[0] != '\0') return false;
	/* syslinux or GRUB present */
	return img->has_grub2 != 0 || img->sl_version != 0 ||
	       img->cfg_path[0] != '\0' || img->uses_casper;
}

const char* rfx_persistence_label(const RfxImgReport *img);
const char* rfx_persistence_label(const RfxImgReport *img)
{
	return (img != NULL && img->uses_casper) ? "casper-rw" : "persistence";
}

bool rfx_persistence_create(const RfxPartTarget *target, uint64_t size, const char *label)
{
	g_return_val_if_fail(target != NULL, false);
	(void)size;

	const char *lbl = (label != NULL && *label != '\0') ? label : "casper-rw";

	/* Rufus uses ext3 by default here (SETTING_USE_EXT_VERSION, clamped 2..4). */
	uprintf("Creating the persistence volume on %s (%s):", target->PartPath, lbl);

	if (!rfx_format_partition(target, RFX_FS_EXT3, 0, lbl, true, false,
	                          NULL, NULL, NULL))
		return rfx_fail(ERR_FORMAT_FAILURE, "Could not format %s as ext3: %s",
		                target->PartPath, rfx_last_error_message());

	/* Debian-family needs the persistence.conf marker file. Ubuntu's casper
	 * does not, and adding one there is harmless but pointless. */
	if (strcmp(lbl, "persistence") != 0)
		return true;

	/* Write it straight into the ext3 root directory. We cannot mount inside
	 * a container without loop support, so prefer a mount when we can get
	 * one and fall back to debugfs, which edits the image in place. */
	RfxPartTarget t = *target;
	char mp[256];
	g_snprintf(mp, sizeof(mp), "/mnt/rufox-persist-%d", (int)getpid());
	bool used_loop = false;
	if (rfx_part_mount(&t, mp, "ext3", &used_loop)) {
		g_autofree char *conf = g_build_filename(mp, "persistence.conf", NULL);
		bool ok = g_file_set_contents(conf, "/ union\n", -1, NULL);
		if (ok) uprintf("Created '%s' with '/ union'", conf);
		else    uprintfw("Could not create '%s': %s", conf, strerror(errno));
		sync();
		rfx_part_unmount(&t, mp);
		g_rmdir(mp);
		return ok;
	}

	GStrv debugfs = rfx_find_tool("debugfs", NULL);
	if (debugfs == NULL) {
		uprintfw("Cannot write persistence.conf: no mount and no debugfs");
		return true;	/* the volume itself is fine */
	}
	g_autofree char *tmp = g_strdup_printf("/tmp/rufox-persistence.conf-%d", (int)getpid());
	if (!g_file_set_contents(tmp, "/ union\n", -1, NULL)) {
		g_strfreev(debugfs);
		return false;
	}
	g_autofree char *cmd = g_strdup_printf("write %s persistence.conf", tmp);
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(debugfs[0]));
	g_strfreev(debugfs);
	g_ptr_array_add(argv, g_strdup("-w"));
	g_ptr_array_add(argv, g_strdup("-R"));
	g_ptr_array_add(argv, g_strdup(cmd));
	g_ptr_array_add(argv, g_strdup(rfx_part_tool_path(&t)));
	g_ptr_array_add(argv, NULL);

	bool ok = rfx_run_ok((GStrv)argv->pdata, NULL, NULL);
	g_ptr_array_free(argv, TRUE);
	unlink(tmp);
	if (ok) uprintf("Created 'persistence.conf' with '/ union' via debugfs");
	return ok;
}
