/*
 * rufox — shared command-line parsing
 *
 * The GUI, the CLI and the privileged helper all describe a job with the same
 * set of switches, so the parser lives in one place. Rufus' own executable
 * switches (-i, -f, -x) remain accepted for familiarity.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "cliopts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_fs(const char *s)
{
	static const struct { const char *n; int v; } t[] = {
		{ "fat16", RFX_FS_FAT16 }, { "fat32", RFX_FS_FAT32 },
		{ "ntfs", RFX_FS_NTFS },   { "udf", RFX_FS_UDF },
		{ "exfat", RFX_FS_EXFAT }, { "refs", RFX_FS_REFS },
		{ "ext2", RFX_FS_EXT2 },   { "ext3", RFX_FS_EXT3 },
		{ "ext4", RFX_FS_EXT4 },
	};
	for (guint i = 0; i < G_N_ELEMENTS(t); i++)
		if (g_ascii_strcasecmp(s, t[i].n) == 0) return t[i].v;
	return RFX_FS_UNKNOWN;
}

static int parse_scheme(const char *s)
{
	if (g_ascii_strcasecmp(s, "mbr") == 0) return RFX_PS_MBR;
	if (g_ascii_strcasecmp(s, "gpt") == 0) return RFX_PS_GPT;
	if (g_ascii_strcasecmp(s, "sfd") == 0 || g_ascii_strcasecmp(s, "superfloppy") == 0)
		return RFX_PS_SFD;
	return -1;
}

static int parse_target(const char *s)
{
	if (g_ascii_strcasecmp(s, "bios") == 0 || g_ascii_strcasecmp(s, "csm") == 0)
		return RFX_TT_BIOS;
	if (g_ascii_strcasecmp(s, "uefi") == 0) return RFX_TT_UEFI;
	return -1;
}

static int parse_boot(const char *s)
{
	static const struct { const char *n; int v; } t[] = {
		{ "none", RFX_BT_NON_BOOTABLE }, { "image", RFX_BT_IMAGE },
		{ "syslinux", RFX_BT_SYSLINUX_V6 }, { "syslinux4", RFX_BT_SYSLINUX_V4 },
		{ "grub2", RFX_BT_GRUB2 }, { "grub", RFX_BT_GRUB2 },
		{ "freedos", RFX_BT_FREEDOS }, { "msdos", RFX_BT_MSDOS },
		{ "uefi-ntfs", RFX_BT_UEFI_NTFS }, { "grub4dos", RFX_BT_GRUB4DOS },
	};
	for (guint i = 0; i < G_N_ELEMENTS(t); i++)
		if (g_ascii_strcasecmp(s, t[i].n) == 0) return t[i].v;
	return -1;
}

static int parse_hash(const char *s)
{
	if (g_ascii_strcasecmp(s, "md5") == 0)    return RFX_HASH_MD5;
	if (g_ascii_strcasecmp(s, "sha1") == 0)   return RFX_HASH_SHA1;
	if (g_ascii_strcasecmp(s, "sha256") == 0) return RFX_HASH_SHA256;
	if (g_ascii_strcasecmp(s, "sha512") == 0) return RFX_HASH_SHA512;
	return RFX_HASH_NONE;
}

static int parse_badblocks(const char *s)
{
	if (g_ascii_strcasecmp(s, "quick") == 0) return RFX_BB_QUICK;
	if (g_ascii_strcasecmp(s, "slow") == 0)  return RFX_BB_SLOW;
	if (g_ascii_strcasecmp(s, "nondestructive") == 0 ||
	    g_ascii_strcasecmp(s, "readonly") == 0) return RFX_BB_NONDESTRUCTIVE;
	return -1;
}

int rfx_cli_parse(int argc, char **argv, RfxCliOpts *o)
{
	g_return_val_if_fail(o != NULL, 2);
	memset(o, 0, sizeof(*o));
	rfx_options_default(&o->opt);
	o->command = NULL;
	o->extra_devs = false;
	o->assume_yes = false;
	o->bb_mode = RFX_BB_DISABLED;
	o->bb_passes = 1;
	o->hash_type = RFX_HASH_NONE;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' && o->command == NULL) { o->command = a; continue; }

		if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 1;
		else if (strcmp(a, "-x") == 0 || strcmp(a, "--extra-devs") == 0) o->extra_devs = true;
		else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) rfx_log_set_level(RFX_LOG_DEBUG);
		else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) rfx_log_set_level(RFX_LOG_ERROR);
		else if (strcmp(a, "-y") == 0 || strcmp(a, "--yes") == 0) o->assume_yes = true;
		else if (strcmp(a, "--dry-run") == 0) o->opt.dry_run = true;
		else if (strcmp(a, "-V") == 0 || strcmp(a, "--verify") == 0) o->opt.verify_after_write = true;
		else if (strcmp(a, "--quick") == 0) o->opt.quick_format = true;
		else if (strcmp(a, "--no-quick") == 0) o->opt.quick_format = false;
		else if (strcmp(a, "--old-bios-fixes") == 0) o->opt.old_bios_fixes = true;
		else if (strcmp(a, "--extended-label") == 0) o->opt.create_extended_label = true;
		else if (strcmp(a, "--no-win11-bypass") == 0) o->opt.fix_win11_requirements = false;
		else if (strcmp(a, "--no-online-account") == 0) o->opt.no_online_account = true;
		else if (strcmp(a, "--remove-edge") == 0) o->opt.remove_edge = true;
		else if (i + 1 < argc) {
			const char *v = argv[++i];
			if (strcmp(a, "-d") == 0 || strcmp(a, "--device") == 0) o->device = v;
			else if (strcmp(a, "-i") == 0 || strcmp(a, "--image") == 0 ||
			         strcmp(a, "--iso") == 0) o->image = v;
			else if (strcmp(a, "-S") == 0 || strcmp(a, "--scheme") == 0) {
				int s = parse_scheme(v);
				if (s < 0) { fprintf(stderr, "Bad scheme '%s'\n", v); return 2; }
				o->opt.partition_scheme = s;
			} else if (strcmp(a, "-T") == 0 || strcmp(a, "--target") == 0) {
				int t = parse_target(v);
				if (t < 0) { fprintf(stderr, "Bad target '%s'\n", v); return 2; }
				o->opt.target_type = t;
			} else if (strcmp(a, "-F") == 0 || strcmp(a, "-f") == 0 ||
			           strcmp(a, "--filesystem") == 0) {
				int f = parse_fs(v);
				if (f < 0) { fprintf(stderr, "Bad file system '%s'\n", v); return 2; }
				o->opt.fs_type = f;
			} else if (strcmp(a, "-c") == 0 || strcmp(a, "--cluster") == 0)
				o->opt.cluster_size = (uint32_t)rfx_parse_size(v);
			else if (strcmp(a, "-L") == 0 || strcmp(a, "--label") == 0)
				g_strlcpy(o->opt.label, v, sizeof(o->opt.label));
			else if (strcmp(a, "-m") == 0 || strcmp(a, "--mode") == 0) {
				if (g_ascii_strcasecmp(v, "dd") == 0) o->opt.write_mode = RFX_WRITE_AS_DD;
				else if (g_ascii_strcasecmp(v, "iso") == 0) o->opt.write_mode = RFX_WRITE_AS_ISO;
				else { fprintf(stderr, "Bad mode '%s'\n", v); return 2; }
			} else if (strcmp(a, "-p") == 0 || strcmp(a, "--persistence") == 0)
				o->opt.persistence_size = rfx_parse_size(v);
			else if (strcmp(a, "-b") == 0 || strcmp(a, "--badblocks") == 0) {
				o->bb_mode = parse_badblocks(v);
				if (o->bb_mode < 0) { fprintf(stderr, "Bad bad-blocks mode '%s'\n", v); return 2; }
			} else if (strcmp(a, "--passes") == 0)
				o->bb_passes = atoi(v);
			else if (strcmp(a, "-H") == 0 || strcmp(a, "--hash") == 0)
				o->hash_type = parse_hash(v);
			else if (strcmp(a, "--boot") == 0) {
				int b = parse_boot(v);
				if (b < 0) { fprintf(stderr, "Bad boot type '%s'\n", v); return 2; }
				o->opt.boot_type = b;
			} else if (strcmp(a, "--image-option") == 0) {
				if (g_ascii_strcasecmp(v, "standard") == 0) o->opt.image_option = RFX_IMOP_WIN_STANDARD;
				else if (g_ascii_strcasecmp(v, "extended") == 0) o->opt.image_option = RFX_IMOP_WIN_EXTENDED;
				else if (g_ascii_strcasecmp(v, "wintogo") == 0) o->opt.image_option = RFX_IMOP_WIN_TO_GO;
				else { fprintf(stderr, "Bad image option '%s'\n", v); return 2; }
			} else if (strcmp(a, "--log") == 0) o->log_file = v;
			else { fprintf(stderr, "Unknown option '%s'\n", a); return 2; }
		} else {
			fprintf(stderr, "Option '%s' needs a value\n", a);
			return 2;
		}
	}

	o->opt.badblocks_mode = o->bb_mode;
	o->opt.badblocks_passes = o->bb_passes;
	o->opt.hash_type = o->hash_type;
	return 0;
}

/*
 * Serialise a job back into argv so that the privileged helper can be handed
 * the exact same description. Every switch round-trips through the parser
 * above, which is what keeps the two processes in agreement.
 */
GPtrArray* rfx_cli_job_argv(const char *helper_path, const RfxCliOpts *o)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(a, g_strdup(helper_path));
	g_ptr_array_add(a, g_strdup("write"));
	g_ptr_array_add(a, g_strdup("-d"));
	g_ptr_array_add(a, g_strdup(o->device != NULL ? o->device : ""));
	if (o->image != NULL) {
		g_ptr_array_add(a, g_strdup("-i"));
		g_ptr_array_add(a, g_strdup(o->image));
	}
	g_ptr_array_add(a, g_strdup("-S"));
	g_ptr_array_add(a, g_strdup(o->opt.partition_scheme == RFX_PS_GPT ? "GPT" :
	                            o->opt.partition_scheme == RFX_PS_MBR ? "MBR" : "SFD"));
	g_ptr_array_add(a, g_strdup("-T"));
	g_ptr_array_add(a, g_strdup(o->opt.target_type == RFX_TT_UEFI ? "UEFI" : "BIOS"));
	g_ptr_array_add(a, g_strdup("-F"));
	g_ptr_array_add(a, g_strdup(rfx_format_fs_name((RfxFs)o->opt.fs_type)));
	if (o->opt.cluster_size != 0) {
		g_ptr_array_add(a, g_strdup("-c"));
		g_ptr_array_add(a, g_strdup_printf("%u", o->opt.cluster_size));
	}
	g_ptr_array_add(a, g_strdup("-L"));
	g_ptr_array_add(a, g_strdup(o->opt.label));
	g_ptr_array_add(a, g_strdup("-m"));
	g_ptr_array_add(a, g_strdup(o->opt.write_mode == RFX_WRITE_AS_DD ? "dd" : "iso"));
	if (o->opt.persistence_size != 0) {
		g_ptr_array_add(a, g_strdup("-p"));
		g_ptr_array_add(a, g_strdup_printf("%" G_GUINT64_FORMAT, o->opt.persistence_size));
	}
	if (o->bb_mode != RFX_BB_DISABLED) {
		g_ptr_array_add(a, g_strdup("-b"));
		g_ptr_array_add(a, g_strdup(o->bb_mode == RFX_BB_QUICK ? "quick" :
		                            o->bb_mode == RFX_BB_SLOW ? "slow" : "nondestructive"));
		g_ptr_array_add(a, g_strdup("--passes"));
		g_ptr_array_add(a, g_strdup_printf("%d", o->bb_passes));
	}
	if (o->opt.verify_after_write) g_ptr_array_add(a, g_strdup("-V"));
	if (!o->opt.quick_format)      g_ptr_array_add(a, g_strdup("--no-quick"));
	if (o->opt.old_bios_fixes)     g_ptr_array_add(a, g_strdup("--old-bios-fixes"));
	if (o->opt.create_extended_label) g_ptr_array_add(a, g_strdup("--extended-label"));
	if (!o->opt.fix_win11_requirements) g_ptr_array_add(a, g_strdup("--no-win11-bypass"));
	if (o->opt.no_online_account)  g_ptr_array_add(a, g_strdup("--no-online-account"));
	if (o->opt.remove_edge)        g_ptr_array_add(a, g_strdup("--remove-edge"));
	if (o->opt.dry_run)            g_ptr_array_add(a, g_strdup("--dry-run"));
	g_ptr_array_add(a, g_strdup("-y"));
	g_ptr_array_add(a, NULL);
	return a;
}
