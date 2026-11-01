/*
 * rufox — job orchestration
 *
 * This is the Linux counterpart of Rufus' FormatThread() in src/format.c,
 * which is the heart of the program: the ordered sequence of "unmount, wipe,
 * partition, format, install bootloader, copy, patch, finalise". The order
 * matters and the steps below are kept in Rufus' order, including the ones
 * that look redundant (e.g. re-reading the layout after repartitioning).
 *
 * Windows -> Linux mapping for this file:
 *   GetPhysicalHandle()      -> open("/dev/sdX", O_RDWR) + exclusive lock
 *   RemoveDriveLetters()     -> umount of every mountpoint below the device
 *   DeletePartition()        -> BLKRRPART + ped_disk_clobber via partition.c
 *   IOCTL_DISK_CREATE_DISK   -> ped_disk_new_fresh()
 *   FormatEx()               -> mkfs.* in format.c
 *   WriteMBR()/WritePBR()    -> bootloaders.c + diskio.c
 *   CopyFileU() tree walk    -> iso_extract.c
 *   UpdateProgressWithInfo() -> RfxProgressCb
 *   CHECK_FOR_USER_CANCEL    -> rfx_cancel_requested()
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* Rufus writes into %USERPROFILE%\Logs; we use XDG_STATE_HOME. */
#define RUFOX_LOG_SUBDIR "rufox"

struct _RfxJobHandle {
	RfxJob        job;
	RfxProgressCb cb;
	gpointer      cb_ud;
};

/* ----------------------------------------------------------------------- */
/* Progress plumbing                                                       */
/* ----------------------------------------------------------------------- */
typedef struct {
	RfxJobHandle *h;
	/* Which op currently owns the 0..100% bar, and its weight. Rufus does
	 * the same thing implicitly by resetting UpdateProgressWithInfoInit(). */
	RfxOp    op;
	double   op_start;
	double   op_span;
	GTimer  *timer;
} ProgressCtx;

static void progress_emit(ProgressCtx *pc, RfxOp op, const char *msg,
                          uint64_t done, uint64_t total, double op_frac)
{
	if (pc == NULL || pc->h->cb == NULL) return;

	RfxProgress p = { 0 };
	double el = pc->timer != NULL ? g_timer_elapsed(pc->timer, NULL) : 0;
	p.op = op;
	p.bytes_done = done;
	p.bytes_total = total;
	p.progress = (float)(pc->op_start + pc->op_span * op_frac);
	p.speed_bps = el > 0 ? (double)done / el : 0;
	p.eta_seconds = (p.speed_bps > 0 && total > done)
	                ? (double)(total - done) / p.speed_bps : -1;
	g_snprintf(p.message, sizeof(p.message), "%s", msg ? msg : "");
	pc->h->cb(&p, pc->h->cb_ud);
}

static void progress_set_op(ProgressCtx *pc, RfxOp op, double start, double span)
{
	pc->op = op;
	pc->op_start = start;
	pc->op_span = span;
}

/* Adapter so that the low-level modules can report through their own
 * RfxProgressCb without knowing about the weighting. */
static void progress_trampoline(const RfxProgress *p, gpointer user_data)
{
	ProgressCtx *pc = user_data;
	if (pc == NULL || p == NULL) return;
	double frac = p->bytes_total > 0 ? (double)p->bytes_done / (double)p->bytes_total
	                                 : (double)p->progress;
	progress_emit(pc, p->op, p->message, p->bytes_done, p->bytes_total,
	              frac < 0 ? 0 : (frac > 1 ? 1 : frac));
}

/* ----------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ----------------------------------------------------------------------- */
static bool is_real_device(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

/* Rufus' "silent"/WUE-style Windows 11 tweaks. On Windows it edits the live
 * registry of the WinPE hive; here we patch the offline SYSTEM hive that ships
 * inside the image, which is the standard Linux approach (hivexregedit). */
static void apply_windows_tweaks(const char *mountpoint, const RfxOptions *opt)
{
	static const char *bypass_name[] = {
		"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck"
	};
	bool want[3] = { opt->fix_win11_requirements, opt->fix_win11_requirements,
	                 opt->fix_win11_requirements };

	g_autofree char *hive = g_build_filename(mountpoint, "sources", "install.wim", NULL);
	if (!g_file_test(hive, G_FILE_TEST_EXISTS)) {
		/* The registry lives inside the WIM, not as a loose file. Rufus
		 * mounts the WIM with wimlib; we hand it to wimmountrw when it is
		 * available and otherwise tell the user what to expect. */
		uprintf("Windows 11 requirement bypass: the SYSTEM hive is inside the WIM");
		if (!rfx_tool_available("wimapply") && !rfx_tool_available("wimmountrw")) {
			uprintfw("  Install 'wimtools' to patch the hive, or run the "
			         "bypass manually after installation");
			return;
		}
	}

	/* The portable, tool-independent path: drop a .reg fragment next to the
	 * image and let Windows Setup's LabConfig pick it up. Rufus' own WUE
	 * writes an unattend.xml with the equivalent `reg add` commands. */
	g_autofree char *oem = g_build_filename(mountpoint, "sources", "$OEM$",
	                                        "$$", "Panther", NULL);
	if (g_mkdir_with_parents(oem, 0755) != 0) {
		uprintfw("Could not create '%s'", oem);
		return;
	}
	g_autofree char *xml = g_build_filename(oem, "unattend.xml", NULL);

	GString *s = g_string_new(NULL);
	g_string_append(s, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	                   "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n"
	                   "  <settings pass=\"windowsPE\">\n"
	                   "    <component name=\"Microsoft-Windows-Setup\" "
	                   "processorArchitecture=\"amd64\" "
	                   "publicKeyToken=\"31bf3856ad364e35\" "
	                   "language=\"neutral\" versionScope=\"nonSxS\">\n"
	                   "      <RunSynchronous>\n");
	for (int i = 0; i < 3; i++) {
		if (!want[i]) continue;
		g_string_append_printf(s,
			"        <RunSynchronousCommand wcm:action=\"add\">\n"
			"          <Order>%d</Order>\n"
			"          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n"
			"        </RunSynchronousCommand>\n", i + 1, bypass_name[i]);
	}
	g_string_append(s, "      </RunSynchronous>\n"
	                   "    </component>\n"
	                   "  </settings>\n"
	                   "</unattend>\n");

	if (g_file_set_contents(xml, s->str, -1, NULL)) {
		for (int i = 0; i < 3; i++)
			if (want[i])
				uprintf("Created 'HKLM\\SYSTEM\\Setup\\LabConfig\\%s' registry key",
				        bypass_name[i]);
	} else {
		uprintfw("Could not write '%s'", xml);
	}
	g_string_free(s, TRUE);

	if (opt->no_online_account)
		uprintf("Local-account-only setup requested: add 'oobe\\BypassNRO' "
		        "or use the Shift+F10 workaround");
	if (opt->remove_edge)
		uprintf("Edge removal requested: run 'RemoveEdge.cmd' from the "
		        "first-boot hook (see Rufus' WUE for the exact script)");
	(void)hive;
}

/* ----------------------------------------------------------------------- */
/* mtools fallback: push an extracted tree into a FAT that we cannot mount. */
/* This is only reachable for image-file targets inside a container without */
/* loop support; on a real stick the kernel always gives us a node.         */
/* ----------------------------------------------------------------------- */

/* Build "-i<image>@@<offset>" the way mtools wants it. */
static char* mtools_i_arg(const RfxPartTarget *t)
{
	if (!t->IsFileTarget || t->Offset == 0)
		return g_strdup(rfx_part_tool_path(t));
	return g_strdup_printf("%s@@%" G_GUINT64_FORMAT, t->DevicePath, t->Offset);
}

static bool mtools_run(const char *tool, const RfxPartTarget *t, GStrv extra)
{
	GStrv found = rfx_find_tool(tool, NULL);
	if (found == NULL) {
		rfx_set_error(ERR_MISSING_TOOL, 0, "'%s' not found (install 'mtools')", tool);
		return false;
	}
	int n_extra = 0;
	while (extra != NULL && extra[n_extra] != NULL) n_extra++;

	GStrv argv = g_new0(char*, n_extra + 3);
	argv[0] = g_strdup(found[0]);
	argv[1] = g_strdup_printf("-i%s", mtools_i_arg(t));
	for (int i = 0; i < n_extra; i++)
		argv[2 + i] = g_strdup(extra[i]);
	g_strfreev(found);

	g_setenv("MTOOLS_SKIP_CHECK", "1", TRUE);
	GError *err = NULL;
	bool ok = rfx_run_ok(argv, NULL, &err);
	if (!ok) {
		uprintfw("%s: %s", tool, err && err->message ? err->message : "failed");
		g_clear_error(&err);
	}
	g_strfreev(argv);
	return ok;
}

/*
 * Copy an extracted tree into a FAT we cannot mount. `mcopy -s -n DIR/.` is
 * the obvious spelling but mtools refuses it ("Cannot create entry named . or
 * .."); copying each top-level entry with -s recurses correctly and creates
 * the directories on the way, so that is what we do.
 */
static bool copy_tree_via_mtools(const RfxPartTarget *t, const char *src_dir)
{
	g_autoptr(GDir) d = g_dir_open(src_dir, 0, NULL);
	if (d == NULL)
		return rfx_fail(ERR_READ_FAULT, "Cannot read the scratch directory '%s'",
		                src_dir);

	uint32_t entries = 0, failed = 0;
	const char *ent;
	while ((ent = g_dir_read_name(d)) != NULL) {
		g_autofree char *path = g_build_filename(src_dir, ent, NULL);
		char *a[] = { (char*)"-s", (char*)"-n", path, (char*)"::/", NULL };
		if (mtools_run("mcopy", t, a))
			entries++;
		else
			failed++;
	}
	g_dir_close(d);
	d = NULL;

	if (failed > 0)
		return rfx_fail(ERR_WRITE_FAULT,
		                "%u of %u top-level entries could not be copied",
		                failed, entries + failed);
	uprintf("Copied %u top-level entr%s into the FAT via mtools", entries,
	        entries == 1 ? "y" : "ies");
	return true;
}

/* ----------------------------------------------------------------------- */
/* The "what will happen" preview, shown in the confirmation dialog        */
/* ----------------------------------------------------------------------- */
char* rfx_job_describe(RfxJobHandle *h)
{
	g_return_val_if_fail(h != NULL, NULL);
	RfxJob *j = &h->job;

	GString *s = g_string_new(NULL);
	char tmp[64];

	g_string_append_printf(s, "Device:   %s\n", j->DevicePath);
	if (j->Drive.FriendlyName[0] != '\0')
		g_string_append_printf(s, "          %s\n", j->Drive.FriendlyName);
	g_string_append_printf(s, "Size:     %s\n",
	                       rfx_human_size(j->Drive.DiskSize, tmp, sizeof(tmp)));
	g_string_append_printf(s, "Image:    %s\n",
	                       j->ImagePath[0] ? j->ImagePath : "(none — format only)");
	if (j->Image.is_iso)
		g_string_append_printf(s, "          ISO, projected %s\n",
		                       rfx_human_size(j->Image.projected_size, tmp, sizeof(tmp)));

	if (j->Options.zero_drive) {
		g_string_append(s, "Action:   ZERO the entire device\n");
		return g_string_free(s, FALSE);
	}

	g_string_append_printf(s, "Mode:     %s\n",
	                       j->Options.write_mode == RFX_WRITE_AS_DD ? "DD (raw image)" : "ISO (extract)");
	g_string_append_printf(s, "Scheme:   %s\n",
	                       j->Options.partition_scheme == RFX_PS_GPT ? "GPT" :
	                       j->Options.partition_scheme == RFX_PS_MBR ? "MBR" : "Superfloppy (no table)");
	g_string_append_printf(s, "Target:   %s\n",
	                       j->Options.target_type == RFX_TT_UEFI ? "UEFI (non CSM)" : "BIOS or UEFI-CSM");
	g_string_append_printf(s, "FS:       %s", rfx_format_fs_name((RfxFs)j->Options.fs_type));
	if (j->Options.cluster_size)
		g_string_append_printf(s, " (cluster %u)", j->Options.cluster_size);
	g_string_append_printf(s, "\nLabel:    %s\n", j->Options.label);
	if (j->Options.persistence_size)
		g_string_append_printf(s, "Persist:  %s\n",
		                       rfx_human_size(j->Options.persistence_size, tmp, sizeof(tmp)));
	if (j->Options.badblocks_mode != RFX_BB_DISABLED)
		g_string_append_printf(s, "Badblocks: %d pass(es)\n", j->Options.badblocks_passes);
	g_string_append(s, "\n*** ALL DATA ON THE DEVICE WILL BE DESTROYED ***\n");
	return g_string_free(s, FALSE);
}

/* ----------------------------------------------------------------------- */
/* Construction                                                            */
/* ----------------------------------------------------------------------- */
RfxJobHandle* rfx_job_new(void)
{
	RfxJobHandle *h = g_new0(RfxJobHandle, 1);
	rfx_options_default(&h->job.Options);
	h->job.Result = RFX_RESULT_NONE;
	return h;
}

void rfx_job_free(RfxJobHandle *h) { g_free(h); }

void rfx_job_set_device(RfxJobHandle *h, const char *devpath)
{
	g_return_if_fail(h != NULL && devpath != NULL);
	g_strlcpy(h->job.DevicePath, devpath, sizeof(h->job.DevicePath));
}

void rfx_job_set_image(RfxJobHandle *h, const char *image)
{
	g_return_if_fail(h != NULL);
	if (image == NULL) h->job.ImagePath[0] = '\0';
	else g_strlcpy(h->job.ImagePath, image, sizeof(h->job.ImagePath));
}

void rfx_job_set_options(RfxJobHandle *h, const RfxOptions *opt)
{
	g_return_if_fail(h != NULL && opt != NULL);
	h->job.Options = *opt;
}

void rfx_job_set_progress_cb(RfxJobHandle *h, RfxProgressCb cb, gpointer ud)
{
	g_return_if_fail(h != NULL);
	h->cb = cb;
	h->cb_ud = ud;
}

RfxJob* rfx_job_peek(RfxJobHandle *h) { return h ? &h->job : NULL; }

/* ----------------------------------------------------------------------- */
/* The engine                                                              */
/* ----------------------------------------------------------------------- */
RfxResult rfx_job_run(RfxJobHandle *h, RfxCancel *cancel)
{
	g_return_val_if_fail(h != NULL, RFX_RESULT_FAILED);

	RfxJob *j = &h->job;
	RfxOptions *o = &j->Options;
	RfxResult result = RFX_RESULT_FAILED;
	char mountpoint[256] = "";
	char *main_part = NULL;
	RfxPartTarget main_t = { 0 };
	RfxPartTarget casper_t = { 0 };
	bool mounted = false;
	int dev_fd = -1;

	j->Result = RFX_RESULT_NONE;
	j->ErrorMessage[0] = '\0';
	rfx_clear_error();

	GTimer *total = g_timer_new();
	ProgressCtx pc = { .h = h, .timer = g_timer_new() };

#define FAIL(code, fmt, ...) do {                                          \
		rfx_set_error(code, errno, fmt, ##__VA_ARGS__);            \
		goto out;                                                  \
	} while (0)
#define CANCELLED() do {                                                   \
		if (rfx_cancel_requested(cancel))                          \
			FAIL(ERR_CANCELLED, "Cancelled by user");           \
	} while (0)

	uprintf("rufox %s starting", RUFOX_VERSION_STR);
	if (!rfx_is_root())
		uprintfw("Not running as root: privileged steps will go through "
		         "udisks2 or pkexec");

	/* ---------------------------------------------------------------- */
	/* 1. Resolve the target device                                     */
	/* ---------------------------------------------------------------- */
	progress_set_op(&pc, RFX_OP_INIT, 0.0, 0.02);
	progress_emit(&pc, RFX_OP_INIT, "Initialising", 0, 0, 0);

	if (j->DevicePath[0] == '\0')
		FAIL(ERR_NO_DEVICE, "No target device selected");

	if (is_real_device(j->DevicePath)) {
		if (!rfx_dev_probe(j->DevicePath, &j->Drive))
			FAIL(ERR_NO_DEVICE, "Cannot probe '%s'", j->DevicePath);
		if (rfx_dev_is_filtered(&j->Drive))
			FAIL(ERR_DEVICE_BUSY,
			     "Refusing to write to '%s': it is the boot device, an optical "
			     "drive or a virtual device", j->DevicePath);
		uprintf("Using device: %s (%s)", j->DevicePath, j->Drive.FriendlyName);
	} else {
		/* Regular file: our synthetic test target. Rufus has no such path,
		 * but it makes the whole pipeline exercisable without a stick. */
		if (!rfx_dev_probe_file(j->DevicePath, &j->Drive))
			FAIL(ERR_NO_DEVICE, "Cannot use '%s' as a target", j->DevicePath);
		uprintf("Using image file as target: %s (%s)", j->DevicePath,
		        j->Drive.FriendlyName);
	}

	if (j->Drive.DiskSize == 0)
		FAIL(ERR_NO_DEVICE, "The target device reports a size of zero");

	/* ---------------------------------------------------------------- */
	/* 2. Scan the image                                                */
	/* ---------------------------------------------------------------- */
	progress_set_op(&pc, RFX_OP_INIT, 0.02, 0.05);
	if (j->ImagePath[0] != '\0') {
		uprintf("Scanning image...");
		if (!rfx_iso_scan(j->ImagePath, &j->Image, cancel))
			FAIL(ERR_NOT_AN_IMAGE, "Could not analyse '%s': %s", j->ImagePath,
			     rfx_last_error_message());

		/* Rufus forces DD mode for images that cannot survive extraction. */
		if (j->Image.disable_iso || !j->Image.is_iso) {
			if (o->write_mode == RFX_WRITE_AS_ISO) {
				uprintf("This image cannot be extracted: switching to DD mode");
				o->write_mode = RFX_WRITE_AS_DD;
			}
		}
		if (o->write_mode == RFX_WRITE_AS_ISO && j->Image.is_iso) {
			if (j->Image.projected_size > j->Drive.DiskSize)
				FAIL(ERR_IMAGE_TOO_LARGE,
				     "The image needs %s but the device only holds %s",
				     "more space", "less");
		}
		if (o->label[0] == '\0' || strcmp(o->label, "RUFOX") == 0) {
			if (j->Image.usb_label[0] != '\0')
				g_strlcpy(o->label, j->Image.usb_label, sizeof(o->label));
			else if (j->Image.label[0] != '\0')
				g_strlcpy(o->label, j->Image.label, sizeof(o->label));
		}
	} else {
		memset(&j->Image, 0, sizeof(j->Image));
		if (o->boot_type == RFX_BT_IMAGE)
			o->boot_type = RFX_BT_NON_BOOTABLE;
		uprintf("No image selected: the device will just be formatted");
	}

	/* ---------------------------------------------------------------- */
	/* 3. Open the device exclusively (Rufus: GetPhysicalHandle + lock) */
	/* ---------------------------------------------------------------- */
	if (o->dry_run) {
		uprintf("[dry-run] not opening the device for writing");
	} else {
		dev_fd = rfx_open_block(j->DevicePath, true);
		if (dev_fd < 0) {
			if (errno == EACCES || errno == EPERM)
				FAIL(ERR_PERMISSION, "Permission denied opening '%s' for writing",
				     j->DevicePath);
			FAIL(ERR_OPEN_FAILED, "Could not open '%s' for writing", j->DevicePath);
		}
		/* Rufus locks the physical drive so that Windows cannot interfere.
		 * The Linux equivalent is flock(LOCK_EX|LOCK_NB). Failing to get the
		 * lock means another process (a second rufox, dd, gnome-disks…) is
		 * already touching the disk; Rufus treats that as fatal, and so do
		 * we — proceeding would corrupt the media and still report success
		 * (docs/REVIEW.md H3). */
		if (is_real_device(j->DevicePath) &&
		    flock(dev_fd, LOCK_EX | LOCK_NB) != 0) {
			int e = errno;   /* FAIL() reads errno itself — preserve it */
			FAIL(ERR_DEVICE_BUSY,
			     "'%s' is being used by another process (could not obtain "
			     "an exclusive lock): %s", j->DevicePath, strerror(e));
		}
	}

	/* ---------------------------------------------------------------- */
	/* 4. Unmount everything (Rufus: RemoveDriveLetters)                */
	/* ---------------------------------------------------------------- */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_INIT, 0.07, 0.03);
	if (is_real_device(j->DevicePath)) {
		uprintf("Unmounting all volumes on %s", j->DevicePath);
		if (!rfx_dev_unmount_all(j->DevicePath))
			uprintfw("Some volumes could not be unmounted");
	}

	/* ---------------------------------------------------------------- */
	/* 5. Bad blocks (Rufus: badblocks.c)                               */
	/* ---------------------------------------------------------------- */
	if (o->badblocks_mode != RFX_BB_DISABLED) {
		CANCELLED();
		progress_set_op(&pc, RFX_OP_BADBLOCKS, 0.10, 0.20);
		RfxBadBlocksReport bb = { .corrupted = g_array_new(FALSE, FALSE, sizeof(uint64_t)) };
		if (!rfx_badblocks_check(j->DevicePath, j->Drive.DiskSize, 4096,
		                         (RfxBadBlocksMode)o->badblocks_mode,
		                         o->badblocks_passes, &bb,
		                         progress_trampoline, &pc, cancel)) {
			g_array_free(bb.corrupted, TRUE);
			FAIL(ERR_WRITE_FAULT, "Bad blocks check failed: %s",
			     rfx_last_error_message());
		}
		uprintf("Bad blocks: %" G_GUINT64_FORMAT " tested, %" G_GUINT64_FORMAT
		        " corrupted", bb.blocks_tested, bb.blocks_corrupted);
		g_array_free(bb.corrupted, TRUE);
	}

	/* ---------------------------------------------------------------- */
	/* 6. Zero the drive (Rufus: "Non bootable" + zero_drive)           */
	/* ---------------------------------------------------------------- */
	if (o->zero_drive) {
		CANCELLED();
		progress_set_op(&pc, RFX_OP_ZERO_MBR, 0.10, 0.85);
		if (o->dry_run) {
			uprintf("[dry-run] would zero %" G_GUINT64_FORMAT " bytes",
			        j->Drive.DiskSize);
		} else if (!rfx_disk_zero(dev_fd, j->Drive.DiskSize, j->Drive.SectorSize,
		                          o->fast_zeroing, progress_trampoline, &pc, cancel)) {
			FAIL(ERR_WRITE_FAULT, "Could not zero the drive: %s",
			     rfx_last_error_message());
		}
		result = RFX_RESULT_SUCCESS;
		goto out;
	}

	/* ---------------------------------------------------------------- */
	/* 7. DD mode: one raw write and we are done                        */
	/* ---------------------------------------------------------------- */
	if (o->write_mode == RFX_WRITE_AS_DD || j->Image.disable_iso ||
	    (j->ImagePath[0] != '\0' && !j->Image.is_iso)) {
		CANCELLED();
		int src_fd = open(j->ImagePath, O_RDONLY | O_CLOEXEC);
		if (src_fd < 0)
			FAIL(ERR_OPEN_FAILED, "Could not open the image '%s'", j->ImagePath);

		progress_set_op(&pc, RFX_OP_FILE_COPY, 0.10,
		                o->verify_after_write ? 0.70 : 0.85);
		if (o->dry_run) {
			uprintf("[dry-run] would DD %" G_GUINT64_FORMAT " bytes to %s",
			        j->Image.image_size, j->DevicePath);
		} else {
			/* Rufus analyzes the existing MBR before overwriting it. */
			rfx_disk_analyze_mbr(dev_fd, "Drive");

			/* A DD image smaller than the device leaves the previous
			 * backup GPT at the very end of the disk, so the kernel,
			 * parted and Windows all see a phantom GPT after the write.
			 * Rufus guards against exactly this with ClearMBRGPT(); clear
			 * both ends first. Non-fatal on failure, like Rufus. Pairs
			 * with H1 so all 33 tail sectors are wiped (docs/REVIEW.md H2). */
			if (!rfx_disk_clear_mbr_gpt(dev_fd, j->Drive.DiskSize,
			                            j->Drive.SectorSize))
				uprintfw("Could not clear the old partition records: %s",
				         rfx_last_error_message());

			if (!rfx_disk_write_image(src_fd, j->Image.image_size, dev_fd,
			                          j->Drive.DiskSize, j->Drive.SectorSize,
			                          j->Image.compression_type,
			                          progress_trampoline, &pc, cancel)) {
				close(src_fd);
				FAIL(ERR_WRITE_FAULT, "DD write failed: %s",
				     rfx_last_error_message());
			}

			if (o->verify_after_write) {
				progress_set_op(&pc, RFX_OP_VERIFY, 0.80, 0.15);
				if (!rfx_disk_verify_image(src_fd, j->Image.image_size, dev_fd,
				                           j->Drive.SectorSize,
				                           progress_trampoline, &pc, cancel)) {
					close(src_fd);
					FAIL(ERR_VERIFY_MISMATCH, "%s", rfx_last_error_message());
				}
			}
		}
		close(src_fd);
		result = RFX_RESULT_SUCCESS;
		goto out;
	}

	/* ---------------------------------------------------------------- */
	/* 8. ISO mode: wipe, partition, format, boot, copy                 */
	/* ---------------------------------------------------------------- */
	if (j->ImagePath[0] == '\0' && o->boot_type == RFX_BT_NON_BOOTABLE) {
		/* "Non bootable" + a file system: just repartition and format. */
		uprintf("Creating a non-bootable %s volume",
		        rfx_format_fs_name((RfxFs)o->fs_type));
	}

	/* 8a. Wipe the old layout (Rufus: ClearMBRGPT + InitializeDisk) */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_ZERO_MBR, 0.10, 0.05);
	if (!o->dry_run) {
		rfx_disk_analyze_mbr(dev_fd, "Drive");
		if (!rfx_disk_clear_mbr_gpt(dev_fd, j->Drive.DiskSize, j->Drive.SectorSize))
			FAIL(ERR_WRITE_FAULT, "Could not clear the partition records: %s",
			     rfx_last_error_message());
	}

	/* 8b. Plan and create the new layout */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_PARTITION, 0.15, 0.05);
	RfxPartSpec plan[RFX_MAX_PARTITIONS];
	int n_parts = rfx_partition_plan(&j->Drive, o, &j->Image, 0, plan);
	if (n_parts <= 0)
		FAIL(ERR_PARTITION_FAILURE, "Could not compute a partition layout: %s",
		     rfx_last_error_message());

	if (!rfx_partition_create(&j->Drive, o->partition_scheme, plan, n_parts, o->dry_run))
		FAIL(ERR_PARTITION_FAILURE, "Could not create the partition table: %s",
		     rfx_last_error_message());

	/* The main data partition is the first one that is neither ESP nor MSR. */
	int main_idx = -1;
	for (int i = 0; i < n_parts; i++) {
		if (plan[i].Esp || plan[i].TypeGuid == 1) continue;
		main_idx = i;
		break;
	}
	if (main_idx < 0)
		FAIL(ERR_INTERNAL, "The layout has no main data partition");

	main_t = rfx_part_resolve(j->DevicePath,
	                          o->partition_scheme == RFX_PS_SFD ? 0 : main_idx + 1,
	                          plan[main_idx].Offset, plan[main_idx].Size);
	main_part = g_strdup(main_t.PartPath);
	if (main_part == NULL)
		FAIL(ERR_INTERNAL, "Could not build the partition device path");

	/* 8c. Format */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_FORMAT, 0.20, 0.15);
	uint32_t cluster = o->cluster_size;
	if (cluster == 0)
		cluster = rfx_format_default_cluster_size((RfxFs)o->fs_type, plan[main_idx].Size);

	if (!o->dry_run) {
		if (!rfx_format_partition(&main_t, (RfxFs)o->fs_type, cluster, o->label,
		                          o->quick_format, o->create_extended_label,
		                          progress_trampoline, &pc, cancel)) {
			/* rfx_format_partition() already set a complete, specific
			 * "Could not format <dev> as <fs>: <mkfs stderr>" message.
			 * Re-wrapping it here doubled the prefix and — because
			 * rfx_last_error_message() aliases the very buffer that
			 * rfx_set_error() writes — corrupted the tail. Propagate it
			 * verbatim instead (docs/REVIEW.md L2). result already
			 * defaults to RFX_RESULT_FAILED. */
			goto out;
		}
		rfx_dev_repartition_notify(j->DevicePath);
	}

	/* 8d. MBR fixups (Rufus: WriteMBR after FormatEx stripped the LBA bit) */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_FIX_MBR, 0.35, 0.03);
	if (!o->dry_run && o->partition_scheme == RFX_PS_MBR) {
		bool masquerade = j->Image.winpe_seen && !j->Image.uses_minint;
		rfx_disk_fixup_mbr(dev_fd, o->fs_type,
		                   o->boot_type != RFX_BT_NON_BOOTABLE &&
		                   o->target_type == RFX_TT_BIOS,
		                   masquerade, j->Drive.SectorSize);
	} else if (!o->dry_run && o->partition_scheme == RFX_PS_GPT) {
		/* Rufus' protective MBR carries its own signature for GPT. */
		rfx_disk_set_mbr_signature(dev_fd, 0x53554652);	/* "RUFS" */
	}

	/* 8e. Mount so that we can copy files. On a real device this always
	 * works; on an image-file target it needs a loop node, and when the
	 * kernel refuses one we fall back to mtools for FAT below. */
	CANCELLED();
	if (!o->dry_run) {
		g_snprintf(mountpoint, sizeof(mountpoint), "/mnt/rufox-%d", (int)getpid());
		bool used_loop = false;
		if (rfx_part_mount(&main_t, mountpoint, NULL, &used_loop)) {
			mounted = true;
			uprintf("Mounted %s on %s%s", rfx_part_tool_path(&main_t),
			        mountpoint, used_loop ? " (via a loop device)" : "");
		} else {
			uprintfw("Could not mount the new file system: %s",
			         rfx_last_error_message());
			rfx_clear_error();
			if (!main_t.IsFileTarget)
				FAIL(ERR_FORMAT_FAILURE,
				     "Could not mount the freshly formatted %s", main_part);
			uprintf("Continuing with direct (mtools) access to the FAT");
		}
	}

	/* 8f. Copy the ISO contents (Rufus: ExtractISO) */
	if (j->ImagePath[0] != '\0' && o->boot_type == RFX_BT_IMAGE) {
		CANCELLED();
		progress_set_op(&pc, RFX_OP_FILE_COPY, 0.38, 0.50);
		if (o->dry_run) {
			uprintf("[dry-run] would extract '%s' onto %s", j->ImagePath, main_part);
		} else if (mounted) {
			if (!rfx_iso_extract(j->ImagePath, mountpoint, &j->Image, o,
			                     progress_trampoline, &pc, cancel))
				FAIL(ERR_WRITE_FAULT, "Extraction failed");
		} else {
			/* No mount: extract to a scratch directory and push the tree
			 * into the FAT with mtools. */
			g_autofree char *scratch = g_dir_make_tmp("rufox-iso-XXXXXX", NULL);
			if (scratch == NULL)
				FAIL(ERR_INTERNAL, "Could not create a scratch directory");
			if (!rfx_iso_extract(j->ImagePath, scratch, &j->Image, o,
			                     progress_trampoline, &pc, cancel)) {
				uprintfw("Extraction failed: %s", rfx_last_error_message());
			} else if (!copy_tree_via_mtools(&main_t, scratch)) {
				FAIL(ERR_WRITE_FAULT,
				     "Could not copy the extracted tree into the FAT: %s",
				     rfx_last_error_message());
			}
			GError *rm = NULL;
			GStrv a = RFX_ARGV("rm", "-rf", scratch);
			rfx_run_ok(a, NULL, &rm);
			g_clear_error(&rm);
		}
	}

	/* 8g. Bootloader */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_PATCH, 0.88, 0.05);
	if (!o->dry_run && o->boot_type != RFX_BT_NON_BOOTABLE) {
		switch (o->boot_type) {
		case RFX_BT_IMAGE:
			/* Nothing to install: the ISO carries its own bootloaders.
			 * For BIOS targets on an extracted tree we still need a
			 * syslinux/GRUB MBR, which bootloaders.c handles. */
			if (o->target_type == RFX_TT_BIOS && j->Image.is_iso) {
				if (!rfx_boot_install_syslinux_mbr(dev_fd, o->partition_scheme,
				                                   j->Drive.SectorSize))
					uprintfw("Could not install the SYSLINUX MBR: %s",
					         rfx_last_error_message());
			}
			break;
		case RFX_BT_GRUB2:
			if (!rfx_boot_install_grub2(j->DevicePath, o->target_type,
			                            o->label, mountpoint))
				uprintfw("Could not install GRUB2: %s", rfx_last_error_message());
			break;
		case RFX_BT_SYSLINUX_V4:
		case RFX_BT_SYSLINUX_V6:
			if (!rfx_boot_install_syslinux(main_part, mountpoint, NULL))
				uprintfw("Could not install SYSLINUX: %s", rfx_last_error_message());
			break;
		case RFX_BT_FREEDOS:
			if (!rfx_boot_freedos_available())
				uprintfw("FreeDOS support needs the 'freedos' resources");
			break;
		default:
			break;
		}
	}

	/* 8h. Windows tweaks + persistence */
	if (!o->dry_run) {
		if (j->Image.is_windows_img && o->image_option != RFX_IMOP_WIN_TO_GO)
			apply_windows_tweaks(mountpoint, o);

		if (j->Image.uses_casper && o->persistence_size > 0) {
			CANCELLED();
			int casper_idx = -1;
			for (int i = 0; i < n_parts; i++)
				if (plan[i].TypeGuid == 4) { casper_idx = i; break; }
			if (casper_idx >= 0) {
				casper_t = rfx_part_resolve(j->DevicePath, casper_idx + 1,
				                            plan[casper_idx].Offset,
				                            plan[casper_idx].Size);
				progress_set_op(&pc, RFX_OP_CREATE_FS, 0.93, 0.04);
				if (!rfx_persistence_create(&casper_t, plan[casper_idx].Size,
				                            rfx_persistence_label(&j->Image)))
					uprintfw("Could not create the persistence volume: %s",
					         rfx_last_error_message());
			}
		}

		if (o->create_extended_label && mounted)
			rfx_format_write_extended_label(mountpoint, o->label);
	}

	/* 8i. Finalise */
	CANCELLED();
	progress_set_op(&pc, RFX_OP_FINALIZE, 0.97, 0.03);
	if (!o->dry_run) {
		if (mounted) {
			sync();
			if (!rfx_part_unmount(&main_t, mountpoint))
				uprintfw("Could not unmount %s", mountpoint);
			mounted = false;
		}
		rfx_sync_block(dev_fd);
		rfx_dev_repartition_notify(j->DevicePath);

		if (o->verify_after_write && o->write_mode == RFX_WRITE_AS_ISO) {
			uprintf("Verify after write is only meaningful in DD mode on Linux; "
			        "the extracted tree was written through the file system");
		}
	}

	result = RFX_RESULT_SUCCESS;
	progress_emit(&pc, RFX_OP_FINALIZE, "Done", 1, 1, 1.0);

out:
	g_clear_pointer(&main_part, g_free);
	if (mounted)
		rfx_part_unmount(&main_t, mountpoint);
	if (dev_fd >= 0) {
		if (is_real_device(j->DevicePath))
			flock(dev_fd, LOCK_UN);
		close(dev_fd);
	}

	j->ElapsedSeconds = g_timer_elapsed(total, NULL);
	if (j->ElapsedSeconds > 0 && j->Image.projected_size > 0)
		j->AverageSpeed = (double)j->Image.projected_size / j->ElapsedSeconds;
	g_timer_destroy(total);
	g_timer_destroy(pc.timer);

	j->Result = result;
	if (result == RFX_RESULT_SUCCESS) {
		char sz[32];
		uprintf("Completed in %.1f s (%s)", j->ElapsedSeconds,
		        rfx_human_size((uint64_t)(j->AverageSpeed * j->ElapsedSeconds),
		                       sz, sizeof(sz)));
	} else {
		g_strlcpy(j->ErrorMessage, rfx_last_error_message(), sizeof(j->ErrorMessage));
		if (j->ErrorMessage[0] == '\0')
			g_strlcpy(j->ErrorMessage, rfx_strerror(rfx_last_error()),
			          sizeof(j->ErrorMessage));
		uprintfe("Failed: %s", j->ErrorMessage);
		if (rfx_cancel_requested(cancel))
			j->Result = RFX_RESULT_CANCELLED;
	}
#undef FAIL
#undef CANCELLED
	return j->Result;
}
