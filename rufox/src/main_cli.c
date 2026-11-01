/*
 * rufox — command-line front-end
 *
 * Rufus itself is GUI-only with a handful of startup switches (-i, -f, -x, -g).
 * A Linux port needs a real CLI: it is what makes the engine testable without
 * a display, and it is what the "boring but correct" use case wants. The flag
 * set mirrors Rufus' dialog one-to-one, and the switches Rufus already has
 * (-i, -f, -x) keep their meaning.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"
#include "rufox-config.h"
#include "cliopts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <unistd.h>

/* ----------------------------------------------------------------------- */
static void print_usage(const char *app)
{
	printf("\nrufox %s — Rufus, ported to Linux\n", RUFOX_VERSION_STR);
	printf("A reliable USB formatting utility. Derived from Rufus by Pete Batard (GPL-3.0+).\n\n");
	printf("Usage: %s <command> [options]\n\n", app);
	printf("Commands:\n");
	printf("  list                     List the devices that can be written to\n");
	printf("  info  IMAGE              Analyse an ISO/IMG/VHD and print what Rufus would\n");
	printf("  plan  -d DEVICE [-i IMG] Show the partition layout that would be created\n");
	printf("  write -d DEVICE [-i IMG] Wipe, partition, format and write. DESTROYS DATA.\n");
	printf("  hash  -i FILE [-H ALGO]  Print a checksum\n");
	printf("  zero  -d DEVICE          Zero a device (Rufus' \"Non bootable\" + fast zeroing)\n");
	printf("  badblocks -d DEVICE [-b MODE]\n");
	printf("                           Check a device for bad blocks\n\n");
	printf("Write options (Rufus' dialog, as flags):\n");
	printf("  -d, --device PATH        Target device (/dev/sdb) or an image file (test mode)\n");
	printf("  -i, --image PATH         The ISO/IMG to write (also --iso)\n");
	printf("  -S, --scheme MBR|GPT|SFD Partition scheme               (default GPT)\n");
	printf("  -T, --target BIOS|UEFI   Target system                  (default UEFI)\n");
	printf("  -F, --filesystem NAME    FAT16|FAT32|NTFS|exFAT|ext2|ext3|ext4|UDF\n");
	printf("  -c, --cluster BYTES      Cluster size                   (default auto)\n");
	printf("  -L, --label LABEL        Volume label\n");
	printf("  -m, --mode iso|dd        ISO extraction or raw DD write\n");
	printf("  -p, --persistence SIZE   Persistence partition size, e.g. 4G\n");
	printf("  -b, --badblocks MODE     quick|slow|nondestructive\n");
	printf("      --passes N           Number of bad-blocks passes      (default 1)\n");
	printf("  -H, --hash ALGO          md5|sha1|sha256|sha512\n");
	printf("  -V, --verify             Re-read and verify after a DD write\n");
	printf("      --boot TYPE          none|image|syslinux|grub2|freedos|uefi-ntfs\n");
	printf("      --image-option TYPE  standard|extended|wintogo\n");
	printf("      --no-win11-bypass    Keep the Windows 11 TPM/SecureBoot/RAM checks\n");
	printf("      --no-online-account  Set up a local account without a Microsoft login\n");
	printf("      --remove-edge        Do not install Microsoft Edge\n");
	printf("      --quick / --no-quick Quick format                   (default quick)\n");
	printf("      --old-bios-fixes     Extra CHS-aligned gap + a compatibility partition\n");
	printf("      --extended-label     Write autorun.inf/drive.ico\n");
	printf("      --dry-run            Do everything except touching the device\n");
	printf("  -v, --verbose            Debug-level logging\n");
	printf("  -q, --quiet              Errors only\n");
	printf("      --log FILE           Also append the log to FILE\n");
	printf("  -y, --yes                Skip the confirmation prompt\n");
	printf("  -x, --extra-devs         List extra devices, such as USB HDDs\n");
	printf("  -h, --help               This usage guide\n");
	printf("\n");
}

/* ----------------------------------------------------------------------- */
/* Progress rendering on a TTY                                             */
/* ----------------------------------------------------------------------- */
#define BAR_WIDTH 34

static void progress_render(const RfxProgress *p, gpointer ud)
{
	bool *is_tty = ud;
	if (is_tty != NULL && !*is_tty) return;

	char rate[32], eta[32];
	rfx_human_rate(p->speed_bps, rate, sizeof(rate));
	rfx_human_eta(p->eta_seconds, eta, sizeof(eta));

	double frac = p->progress;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int filled = (int)(frac * BAR_WIDTH);

	char bar[BAR_WIDTH + 1];
	for (int i = 0; i < BAR_WIDTH; i++)
		bar[i] = (i < filled) ? '=' : (i == filled ? '>' : ' ');
	bar[BAR_WIDTH] = '\0';

	fprintf(stderr, "\r[%s] %5.1f%%  %-11s %s   ", bar, frac * 100.0, rate, eta);
	fflush(stderr);
	if (frac >= 1.0) fprintf(stderr, "\n");
}

/* ----------------------------------------------------------------------- */
/* Commands                                                                */
/* ----------------------------------------------------------------------- */
static int cmd_list(bool extra_devs)
{
	RfxDevList *l = rfx_dev_list_new();
	if (!rfx_dev_list_refresh(l)) {
		fprintf(stderr, "Could not enumerate devices: %s\n", rfx_last_error_message());
		rfx_dev_list_free(l);
		return 1;
	}

	int n = rfx_dev_list_count(l);
	if (n == 0) {
		printf("No writable removable device found.\n");
		if (!extra_devs)
			printf("(Re-run with -x/--extra-devs to also list USB hard drives.)\n");
		rfx_dev_list_free(l);
		return 0;
	}

	printf("%-14s %-9s %-7s %-6s %-26s %s\n",
	       "DEVICE", "SIZE", "BUS", "SECTOR", "MODEL", "STATE");
	printf("%-14s %-9s %-7s %-6s %-26s %s\n",
	       "--------------", "---------", "-------", "------",
	       "--------------------------", "------------------");
	for (int i = 0; i < n; i++) {
		const RfxDrive *d = rfx_dev_list_get(l, i);
		char sz[32];
		rfx_human_size(d->DiskSize, sz, sizeof(sz));
		printf("%-14s %-9s %-7s %-6u %-26.26s %s%s\n",
		       d->DevicePath, sz, d->BusName, d->SectorSize,
		       d->Model[0] ? d->Model : (d->Vendor[0] ? d->Vendor : "-"),
		       d->MountPoint[0] ? "mounted" : "idle",
		       (!extra_devs && !d->IsRemovable) ? " (fixed: use -x)" : "");
		for (int p = 0; p < d->PartitionCount; p++) {
			char psz[32], poff[32];
			rfx_human_size(d->Partition[p].Size, psz, sizeof(psz));
			rfx_human_size(d->Partition[p].Offset, poff, sizeof(poff));
			printf("  %-12s └ %-8s @ %-9s %s%s%s\n", "",
			       psz, poff,
			       d->Partition[p].FsName[0] ? d->Partition[p].FsName : "?",
			       d->Partition[p].Name[0] ? "  \"" : "",
			       d->Partition[p].Name[0] ? d->Partition[p].Name : "");
			if (d->Partition[p].Name[0]) printf("\"");
			printf("\n");
		}
	}
	rfx_dev_list_free(l);
	return 0;
}

static void print_report(const RfxImgReport *r)
{
	char sz[32];
	printf("\n── Image report ─────────────────────────────────────────\n");
	printf("  Path              : %s\n", r->ImagePath);
	printf("  Size              : %s (%" G_GUINT64_FORMAT " bytes)\n",
	       rfx_human_size(r->image_size, sz, sizeof(sz)), r->image_size);
	printf("  Projected size    : %s\n",
	       rfx_human_size(r->projected_size, sz, sizeof(sz)));
	printf("  Type              : %s%s\n",
	       r->is_iso ? "ISO9660" : (r->is_vhd ? "VHD" : "raw image"),
	       r->is_bootable_img ? " + hybrid MBR (isohybrid)" : "");
	if (r->compression_type != RFX_BLED_NONE)
		printf("  Compression       : %s\n", rfx_compression_name(r->compression_type));
	if (r->label[0])     printf("  Volume id         : %s\n", r->label);
	if (r->usb_label[0]) printf("  Proposed label    : %s\n", r->usb_label);
	printf("  UEFI bootable     : %s (mask 0x%X, %d entries)\n",
	       r->has_efi ? "yes" : "no", r->has_efi, r->efi_boot_entry_count);
	for (int i = 0; i < r->efi_boot_entry_count && i < 8; i++)
		printf("                      %s\n", r->efi_boot_entry[i].path);
	printf("  BIOS bootable     : %s\n",
	       r->has_bootmgr ? "yes (bootmgr)" :
	       r->sl_version != 0 ? "yes (ISOLINUX)" :
	       r->has_grub2 ? "yes (GRUB2)" : "no");
	if (r->sl_version)   printf("  ISOLINUX version  : %s\n", r->sl_version_str);
	if (r->has_grub2)    printf("  GRUB2             : yes\n");
	if (r->has_grub4dos) printf("  Grub4DOS          : yes\n");
	if (r->is_windows_img)
		printf("  Windows install   : yes (%s), %d install image(s)%s\n",
		       r->win_version == RFX_WIN_11 ? "Windows 11" :
		       r->win_version == RFX_WIN_10 ? "Windows 10" :
		       r->win_version == RFX_WIN_81 ? "Windows 8.1" :
		       r->win_version == RFX_WIN_8  ? "Windows 8" :
		       r->win_version == RFX_WIN_7  ? "Windows 7" : "unknown",
		       r->wininst_index, r->has_4GB_file ? ", contains a file >= 4 GB" : "");
	if (r->uses_casper)  printf("  Casper/live       : yes (persistence possible)\n");
	printf("  Windows To Go     : %s\n",
	       rfx_wintogo_supported(r) ? "supported" : "not supported");
	if (r->disable_iso)
		printf("  *** Extraction is disabled for this image: it must be DD-written ***\n");

	printf("\n  Allowed file systems:\n    ");
	RfxFs allowed[RFX_FS_MAX];
	int n = 0;
	rfx_format_allowed_fs(RFX_BT_IMAGE, RFX_TT_UEFI, r, allowed, &n);
	for (int i = 0; i < n; i++)
		printf("%s ", rfx_format_fs_name(allowed[i]));
	printf("\n");

	if (rfx_persistence_supported(r))
		printf("  Persistence label : %s\n", rfx_persistence_label(r));
	printf("─────────────────────────────────────────────────────────\n\n");
}

static int cmd_info(const char *image, int hash_type)
{
	RfxImgReport rep;
	if (!rfx_iso_scan(image, &rep, NULL)) {
		fprintf(stderr, "Could not analyse '%s': %s\n", image, rfx_last_error_message());
		return 1;
	}
	print_report(&rep);

	if (hash_type > RFX_HASH_NONE) {
		char hex[256];
		if (rfx_hash_file(image, (RfxHashType)hash_type, hex, sizeof(hex),
		                  NULL, NULL, NULL))
			printf("%s  %s\n", hex, image);
	}
	return 0;
}

static int cmd_plan(const char *device, const char *image, const RfxOptions *opt)
{
	RfxDrive drive;
	RfxImgReport rep;
	memset(&rep, 0, sizeof(rep));

	if (!rfx_dev_probe(device, &drive)) {
		if (!rfx_dev_probe_file(device, &drive)) {
			fprintf(stderr, "Cannot probe '%s': %s\n", device, rfx_last_error_message());
			return 1;
		}
	}
	if (image != NULL && !rfx_iso_scan(image, &rep, NULL)) {
		fprintf(stderr, "Cannot analyse '%s': %s\n", image, rfx_last_error_message());
		return 1;
	}

	RfxPartSpec plan[RFX_MAX_PARTITIONS];
	int n = rfx_partition_plan(&drive, opt, image != NULL ? &rep : NULL, 0, plan);
	if (n <= 0) {
		fprintf(stderr, "Cannot plan a layout: %s\n", rfx_last_error_message());
		return 1;
	}

	char sz[32];
	printf("Layout for %s (%s), scheme %s:\n", device,
	       rfx_human_size(drive.DiskSize, sz, sizeof(sz)),
	       opt->partition_scheme == RFX_PS_GPT ? "GPT" :
	       opt->partition_scheme == RFX_PS_MBR ? "MBR" : "SFD");
	printf("  %-4s %-26s %14s %14s  %s\n", "#", "NAME", "OFFSET", "SIZE", "FLAGS");
	for (int i = 0; i < n; i++) {
		char off[32], size[32], flags[24] = "";
		rfx_human_size(plan[i].Offset, off, sizeof(off));
		rfx_human_size(plan[i].Size, size, sizeof(size));
		if (plan[i].Esp)    g_strlcat(flags, "esp ", sizeof(flags));
		if (plan[i].Boot)   g_strlcat(flags, "boot ", sizeof(flags));
		if (plan[i].Hidden) g_strlcat(flags, "hidden ", sizeof(flags));
		printf("  %-4d %-26s %14s %14s  %s\n", i + 1, plan[i].Name, off, size, flags);
		printf("       raw: offset=%" G_GUINT64_FORMAT " size=%" G_GUINT64_FORMAT "\n",
		       plan[i].Offset, plan[i].Size);
	}
	return 0;
}

static bool confirm(const char *device, RfxJobHandle *h, bool assume_yes)
{
	if (assume_yes) return true;
	if (!isatty(fileno(stdin))) {
		fprintf(stderr, "Not a terminal: refusing to write without -y/--yes\n");
		return false;
	}
	g_autofree char *desc = rfx_job_describe(h);
	printf("\n%s\n", desc);
	printf("Type the device path to confirm: ");
	fflush(stdout);

	char line[256] = "";
	if (fgets(line, sizeof(line), stdin) == NULL) return false;
	g_strchomp(line);
	if (strcmp(line, device) != 0) {
		fprintf(stderr, "Aborted: you typed '%s', expected '%s'\n", line, device);
		return false;
	}
	return true;
}

static int cmd_write(const char *device, const char *image, RfxOptions *opt,
                     bool assume_yes, bool *is_tty)
{
	RfxJobHandle *h = rfx_job_new();
	rfx_job_set_device(h, device);
	rfx_job_set_image(h, image);
	rfx_job_set_options(h, opt);
	rfx_job_set_progress_cb(h, progress_render, is_tty);

	if (!confirm(device, h, assume_yes)) {
		rfx_job_free(h);
		return 2;
	}

	RfxResult r = rfx_job_run(h, NULL);
	RfxJob *j = rfx_job_peek(h);

	printf("\n");
	switch (r) {
	case RFX_RESULT_SUCCESS:
		printf("✔ Done in %.1f s\n", j->ElapsedSeconds);
		break;
	case RFX_RESULT_CANCELLED:
		printf("✖ Cancelled\n");
		break;
	default:
		printf("✖ Failed: %s\n", j->ErrorMessage);
		break;
	}
	int code = r == RFX_RESULT_SUCCESS ? 0 : 1;
	rfx_job_free(h);
	return code;
}

static int cmd_hash(const char *path, int type)
{
	if (type <= RFX_HASH_NONE) type = RFX_HASH_SHA256;
	char hex[256];
	if (!rfx_hash_file(path, (RfxHashType)type, hex, sizeof(hex), NULL, NULL, NULL)) {
		fprintf(stderr, "Failed: %s\n", rfx_last_error_message());
		return 1;
	}
	printf("%s  %s\n", hex, path);
	return 0;
}

static int cmd_badblocks(const char *device, int mode, int passes)
{
	RfxBadBlocksReport bb = { .corrupted = g_array_new(FALSE, FALSE, sizeof(uint64_t)) };
	RfxDrive d;
	uint64_t size = 0;
	if (rfx_dev_probe(device, &d))
		size = d.DiskSize;
	else if (rfx_dev_probe_file(device, &d))
		size = d.DiskSize;
	if (size == 0) {
		fprintf(stderr, "Cannot determine the size of '%s'\n", device);
		g_array_free(bb.corrupted, TRUE);
		return 1;
	}
	bool ok = rfx_badblocks_check(device, size, 4096, (RfxBadBlocksMode)mode,
	                              passes, &bb, NULL, NULL, NULL);
	g_array_free(bb.corrupted, TRUE);
	return ok ? 0 : 1;
}

/* ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	rfx_log_set_level(RFX_LOG_INFO);

	bool is_tty = isatty(fileno(stderr));

	RfxCliOpts o;
	int rc = rfx_cli_parse(argc, argv, &o);
	if (rc == 1) { print_usage(argv[0]); return 0; }
	if (rc != 0) return 2;

	if (o.log_file != NULL) rfx_log_set_file(o.log_file);

	if (o.command == NULL) {
		/* Rufus' own behaviour with -i but no command: hand over to the GUI. */
		if (o.image != NULL) {
			printf("Selected image: %s\n", o.image);
			printf("Start rufox-gui for the graphical front-end, or run "
			       "'rufox list' and 'rufox write -d DEVICE -i %s'.\n", o.image);
			return 0;
		}
		print_usage(argv[0]);
		return 0;
	}

	if (strcmp(o.command, "list") == 0)
		return cmd_list(o.extra_devs);
	if (strcmp(o.command, "info") == 0) {
		if (o.image == NULL) { fprintf(stderr, "info needs -i IMAGE\n"); return 2; }
		return cmd_info(o.image, o.hash_type);
	}
	if (strcmp(o.command, "plan") == 0) {
		if (o.device == NULL) { fprintf(stderr, "plan needs -d DEVICE\n"); return 2; }
		return cmd_plan(o.device, o.image, &o.opt);
	}
	if (strcmp(o.command, "write") == 0) {
		if (o.device == NULL) { fprintf(stderr, "write needs -d DEVICE\n"); return 2; }
		return cmd_write(o.device, o.image, &o.opt, o.assume_yes, &is_tty);
	}
	if (strcmp(o.command, "zero") == 0) {
		if (o.device == NULL) { fprintf(stderr, "zero needs -d DEVICE\n"); return 2; }
		o.opt.zero_drive = true;
		return cmd_write(o.device, NULL, &o.opt, o.assume_yes, &is_tty);
	}
	if (strcmp(o.command, "hash") == 0) {
		const char *target = o.image != NULL ? o.image : o.device;
		if (target == NULL) { fprintf(stderr, "hash needs a path (-i FILE)\n"); return 2; }
		return cmd_hash(target, o.hash_type);
	}
	if (strcmp(o.command, "badblocks") == 0) {
		if (o.device == NULL) { fprintf(stderr, "badblocks needs -d DEVICE\n"); return 2; }
		if (o.bb_mode == RFX_BB_DISABLED) o.bb_mode = RFX_BB_QUICK;
		return cmd_badblocks(o.device, o.bb_mode, o.bb_passes);
	}

	fprintf(stderr, "Unknown command '%s'\n", o.command);
	print_usage(argv[0]);
	return 2;
}
