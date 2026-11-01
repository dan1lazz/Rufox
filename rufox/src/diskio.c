/*
 * rufox — low-level disk I/O
 *
 * Rufus: drive.c AnalyzeMBR(), format.c ClearMBRGPT(), WriteMBR(),
 * WriteDrive() (the DD path). On Windows all of these go through
 * WriteFileWithRetry() on a locked \\.\PhysicalDriveN handle; here we use
 * pread()/pwrite() on /dev/sdX with an equivalent retry policy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

/* Rufus' MAX_SECTORS_TO_CLEAR is 128 (64 KB): enough to cover the MBR, the
 * GPT header, the primary GPT table and the first VBR. */
#define MAX_SECTORS_TO_CLEAR  128
/* Rufus: WRITE_RETRIES 5 / WRITE_TIMEOUT 5000 ms */
#define WRITE_RETRIES          5
#define WRITE_TIMEOUT_MS    5000
/* Rufus: DD_BUFFER_SIZE (1 MB) */
#define DD_BUFFER_SIZE  (1024 * 1024)

/* ----------------------------------------------------------------------- */
/* Retrying writer (Rufus: WriteFileWithRetry)                             */
/* ----------------------------------------------------------------------- */
static bool write_with_retry(int fd, const void *buf, size_t len, uint64_t offset,
                             RfxCancel *cancel)
{
	/* A short write carries no errno but still means the media failed. */
	int saved_errno = EIO;
	for (int i = 1; i <= WRITE_RETRIES; i++) {
		if (rfx_cancel_requested(cancel)) {
			rfx_set_error(ERR_CANCELLED, 0, "Cancelled during write retry");
			return false;
		}
		ssize_t w = pwrite(fd, buf, len, (off_t)offset);
		if (w == (ssize_t)len)
			return true;
		/* Capture errno *now*: the retry sleep and the logging below clobber
		 * it, which used to make us report e.g. EAGAIN instead of ENOSPC
		 * (docs/REVIEW.md L3). */
		saved_errno = (w < 0) ? errno : EIO;
		if (w >= 0)
			uprintfw("Write error: wrote %zd bytes, expected %zu", w, len);
		else
			uprintfw("Write error at sector %" G_GUINT64_FORMAT ": %s",
			         offset / 512, strerror(saved_errno));
		if (i < WRITE_RETRIES) {
			uprintf("Retrying in %d second(s)...", WRITE_TIMEOUT_MS / 1000);
			for (int ms = 0; ms < WRITE_TIMEOUT_MS; ms += 100) {
				if (rfx_cancel_requested(cancel)) break;
				g_usleep(100 * 1000);
			}
		}
	}
	rfx_set_error(ERR_WRITE_FAULT, saved_errno,
	              "Could not write to the drive after %d attempts", WRITE_RETRIES);
	return false;
}

/* ----------------------------------------------------------------------- */
/* MBR fingerprinting (Rufus: mbr_types.h)                                 */
/* ----------------------------------------------------------------------- */
/* Rufus ships a table of known MBRs (ms-sys, syslinux, grub, …) and reports
 * which one a drive carries. We keep the most useful subset. The offsets and
 * magic bytes below are taken from ms-sys and syslinux' mbr.bin. */

static bool br_has_marker(const uint8_t *s)
{
	return s[510] == 0x55 && s[511] == 0xAA;
}

static bool mbr_is_zeroed(const uint8_t *s)
{
	for (int i = 0; i < 446; i++)
		if (s[i] != 0) return false;
	return true;
}

static bool mbr_is_syslinux(const uint8_t *s)
{
	/* mbr.bin, altmbr.bin and gptmbr.bin all begin 33 C0 FA
	 * (xor ax,ax; cli). Distinguish them by their message strings.
	 * (The old FA 33 C0 test was byte-swapped and never matched; the
	 *  33 C0 FA test that used to live in mbr_is_isohybrid belonged
	 *  here. See docs/REVIEW.md M2.) */
	if (!(s[0] == 0x33 && s[1] == 0xC0 && s[2] == 0xFA))
		return false;
	return memmem(s, 512, "Missing operating system.", 25) != NULL ||
	       memmem(s, 512, "Missing OS", 10) != NULL;
}

static bool mbr_is_syslinux_gpt(const uint8_t *s)
{
	/* gptmbr.bin is the SYSLINUX MBR for GPT disks: same 33 C0 FA prologue
	 * but it carries "Multiple active partitions". Checked before the plain
	 * SYSLINUX entry so gptmbr.bin is not mislabelled. */
	return memmem(s, 512, "Invalid GPT", 11) != NULL ||
	       memmem(s, 512, "No OS", 5) != NULL ||
	       memmem(s, 512, "Multiple active partitions", 26) != NULL;
}

static bool mbr_is_grub(const uint8_t *s)
{
	/* GRUB stage1 */
	return s[0] == 0xEB && (s[2] == 0x90 || s[2] == 0x00) &&
	       memmem(s, 512, "GRUB", 4) != NULL;
}

static bool mbr_is_ms_sys(const uint8_t *s)
{
	/* Windows/ms-sys style MBRs: JMP + "Invalid partition table" */
	return memmem(s, 512, "Invalid partition table", 23) != NULL ||
	       memmem(s, 512, "Error loading operating system", 30) != NULL ||
	       memmem(s, 512, "Missing operating system", 24) != NULL;
}

static bool mbr_is_isohybrid(const uint8_t *s)
{
	/* isohdpfx.bin begins 33 ED 90 and carries its own error string.
	 * (The previous 33 C0 FA test was syslinux's prologue, not isohybrid's;
	 *  real isohdpfx.bin was reported as "unknown". See docs/REVIEW.md M2.) */
	return (s[0] == 0x33 && s[1] == 0xED && s[2] == 0x90) ||
	       memmem(s, 512, "isolinux.bin missing or corrupt.", 31) != NULL;
}

bool rfx_disk_analyze_mbr(int fd, const char *target_name)
{
	uint8_t sector[4096];
	memset(sector, 0, sizeof(sector));
	if (!rfx_read_exact(fd, sector, 512, 0)) {
		uprintf("%s: could not read the boot sector", target_name);
		return false;
	}
	if (!br_has_marker(sector)) {
		uprintf("%s does not have a Boot Marker", target_name);
		return false;
	}

	static const struct { bool (*fn)(const uint8_t*); const char *str; } known[] = {
		{ mbr_is_zeroed,        "Zeroed"                 },
		/* GPT variant first: gptmbr.bin shares syslinux' 33 C0 FA prologue
		 * and would otherwise be reported as plain "SYSLINUX". The order
		 * syslinux* before ms_sys is meaningful ("Missing operating system"
		 * appears in both). See docs/REVIEW.md M2. */
		{ mbr_is_syslinux_gpt,  "SYSLINUX GPT (protective)" },
		{ mbr_is_syslinux,      "SYSLINUX"               },
		{ mbr_is_isohybrid,     "ISOLINUX/isohybrid"     },
		{ mbr_is_grub,          "GRUB"                   },
		{ mbr_is_ms_sys,        "Windows/ms-sys"         },
	};

	for (guint i = 0; i < G_N_ELEMENTS(known); i++) {
		if (known[i].fn(sector)) {
			uprintf("%s has a %s Master Boot Record", target_name, known[i].str);
			return true;
		}
	}
	uprintf("%s has an unknown Master Boot Record", target_name);
	return true;
}

/* ----------------------------------------------------------------------- */
/* Boot-code installation                                                  */
/* ----------------------------------------------------------------------- */
bool rfx_disk_write_mbr_bootcode(int fd, const uint8_t *code, size_t len,
                                 uint32_t sector_size, uint8_t drive_number)
{
	if (sector_size < 512) sector_size = 512;
	if (len > sector_size) len = sector_size;

	g_autofree uint8_t *sector = g_malloc0(sector_size);
	if (!rfx_read_exact(fd, sector, sector_size, 0)) {
		rfx_set_error(ERR_READ_FAULT, errno, "Could not read the MBR");
		return false;
	}

	/* Preserve the partition table (offset 0x1BE..0x1FD) and the signature. */
	memcpy(sector, code, len > 446 ? 446 : len);
	sector[444] = 0;			/* Rufus/ms-sys disk signature: low byte  */
	sector[445] = drive_number;		/* ... and the boot drive number in 0x1BD */
	sector[510] = 0x55;
	sector[511] = 0xAA;

	if (!write_with_retry(fd, sector, sector_size, 0, NULL)) {
		rfx_set_error(ERR_WRITE_FAULT, errno, "Could not write the MBR");
		return false;
	}
	return true;
}

/* Rufus: ClearMBRGPT() */
bool rfx_disk_clear_mbr_gpt(int fd, uint64_t disk_size, uint32_t sector_size)
{
	if (sector_size < 512) sector_size = 512;

	size_t buf_len = (size_t)sector_size * MAX_SECTORS_TO_CLEAR;
	g_autofree uint8_t *buf = g_malloc0(buf_len);

	uprintfd("Zeroing the first and last %d sectors", MAX_SECTORS_TO_CLEAR);

	if (!write_with_retry(fd, buf, buf_len, 0, NULL)) {
		rfx_set_error(ERR_WRITE_FAULT, errno, "Could not zero the top of the drive");
		return false;
	}
	char tmp[32];
	uprintf("Zeroed %s at the top of the drive", rfx_human_size(buf_len, tmp, sizeof(tmp)));

	/* The backup GPT lives at the very end. Rufus treats a failure here as
	 * non-fatal (Windows holds a lock on it); we do the same. */
	/* Backup GPT = 1 header + 32 table sectors = 33 sectors. Rufus clears
	 * MAX_SECTORS_TO_CLEAR at *both* ends; clearing only buf_len/8 (16
	 * sectors) left the backup partition table alive and the kernel/parted
	 * then saw a phantom GPT (docs/REVIEW.md H1). */
	size_t tail_len = buf_len;
	if (disk_size > tail_len) {
		if (write_with_retry(fd, buf, tail_len, disk_size - tail_len, NULL)) {
			uprintf("Zeroed %s at the end of the drive",
			        rfx_human_size(tail_len, tmp, sizeof(tmp)));
		} else {
			uprintfw("Could not clear the backup GPT area: %s", strerror(errno));
			rfx_clear_error();
		}
	}
	rfx_sync_block(fd);
	return true;
}

/* ----------------------------------------------------------------------- */
/* Drive zeroing / DD image writing (Rufus: WriteDrive)                    */
/* ----------------------------------------------------------------------- */
static bool buffer_is_uniform(const uint8_t *buf, size_t len)
{
	uint32_t first;
	if (len < sizeof(first)) return false;
	memcpy(&first, buf, sizeof(first));
	if (first != 0 && first != 0xFFFFFFFF) return false;
	for (size_t i = sizeof(first); i + sizeof(first) <= len; i += sizeof(first)) {
		uint32_t v;
		memcpy(&v, buf + i, sizeof(v));
		if (v != first) return false;
	}
	return true;
}

bool rfx_disk_zero(int fd, uint64_t size, uint32_t sector_size, bool fast,
                   RfxProgressCb cb, gpointer ud, RfxCancel *cancel)
{
	if (sector_size < 512) sector_size = 512;
	size_t buf_len = RFX_CEILING_ALIGN(DD_BUFFER_SIZE, sector_size);

	g_autofree uint8_t *buf = aligned_alloc(sector_size, buf_len);
	if (buf == NULL)
		return rfx_fail(ERR_NOT_ENOUGH_MEMORY, "Could not allocate the zeroing buffer");
	memset(buf, fast ? 0xFF : 0x00, buf_len);

	uprintf(fast ? "Fast-zeroing drive:" : "Zeroing drive:");

	g_autofree uint8_t *cmp = NULL;
	if (fast) {
		cmp = aligned_alloc(sector_size, buf_len);
		if (cmp == NULL)
			return rfx_fail(ERR_NOT_ENOUGH_MEMORY, "Could not allocate the compare buffer");
	}

	GTimer *timer = g_timer_new();
	uint64_t written = 0;
	int throttle = 0;

	RfxProgress p = { 0 };
	p.op = RFX_OP_ZERO_MBR;

	while (written < size) {
		if (rfx_cancel_requested(cancel)) {
			g_timer_destroy(timer);
			return rfx_fail(ERR_CANCELLED, "Cancelled by user");
		}

		size_t chunk = buf_len;
		if (written + chunk > size)
			chunk = (size_t)RFX_CEILING_ALIGN(size - written, sector_size);
		if (written + chunk > size)
			chunk = (size_t)(size - written);
		if (chunk == 0) break;

		/* Rufus' fast-zeroing heuristic: reading flash is often much
		 * faster than writing it, so skip blocks that already hold
		 * 0x00/0xFF. A back-off counter limits how often we read. */
		if (fast && throttle == 0) {
			if (pread(fd, cmp, chunk, (off_t)written) == (ssize_t)chunk &&
			    buffer_is_uniform(cmp, chunk)) {
				written += chunk;
				continue;
			}
			throttle = 15;
		} else if (throttle > 0) {
			throttle--;
		}

		if (!write_with_retry(fd, buf, chunk, written, cancel)) {
			g_timer_destroy(timer);
			return false;
		}
		written += chunk;

		if (cb != NULL) {
			double el = g_timer_elapsed(timer, NULL);
			p.progress    = (float)((double)written / (double)size);
			p.bytes_done  = written;
			p.bytes_total = size;
			p.speed_bps   = el > 0 ? (double)written / el : 0;
			p.eta_seconds = p.speed_bps > 0 ? (double)(size - written) / p.speed_bps : -1;
			g_snprintf(p.message, sizeof(p.message), "Zeroing drive");
			cb(&p, ud);
		}
		uprint_progress(written, size);
	}

	rfx_sync_block(fd);
	g_timer_destroy(timer);
	uprintf("\n");
	return true;
}

/*
 * The DD writer. Rufus implements this inside WriteDrive() with a pair of
 * ping-pong buffers and a dedicated thread; we keep the same structure but
 * use synchronous pread/pwrite because the source is almost always a page-
 * cache resident file.
 */
bool rfx_disk_write_image(int src_fd, uint64_t src_size, int dst_fd,
                          uint64_t dst_size, uint32_t sector_size,
                          int compression, RfxProgressCb cb, gpointer ud,
                          RfxCancel *cancel)
{
	if (sector_size < 512) sector_size = 512;
	if (compression != RFX_BLED_NONE) {
		rfx_set_error(ERR_UNSUPPORTED_FS, 0,
		              "Compressed images must be decompressed first on Linux");
		return false;
	}

	if (src_size > dst_size) {
		rfx_set_error(ERR_IMAGE_TOO_LARGE, 0,
		              "Image is larger than the target device");
		return false;
	}
	/* Rufus: MIN(DiskSize, image_size). After the guard above src_size is
	 * always <= dst_size, so the old `src_size < dst_size ? … : …` ternary
	 * was dead code (docs/REVIEW.md L7). */
	uint64_t target = src_size;

	size_t buf_len = RFX_CEILING_ALIGN(DD_BUFFER_SIZE, sector_size);
	g_autofree uint8_t *buf = aligned_alloc(sector_size, buf_len);
	if (buf == NULL)
		return rfx_fail(ERR_NOT_ENOUGH_MEMORY, "Could not allocate the write buffer");

	uprintf("Writing image in DD mode:");
	uprintfd("  %s -> fd, %" G_GUINT64_FORMAT " bytes", "image", target);

	GTimer *timer = g_timer_new();
	uint64_t done = 0;
	RfxProgress p = { 0 };
	p.op = RFX_OP_FILE_COPY;

	while (done < target) {
		if (rfx_cancel_requested(cancel)) {
			g_timer_destroy(timer);
			return rfx_fail(ERR_CANCELLED, "Cancelled by user");
		}
		size_t chunk = buf_len;
		if (done + chunk > target) chunk = (size_t)(target - done);

		ssize_t r = pread(src_fd, buf, chunk, (off_t)done);
		if (r <= 0) {
			g_timer_destroy(timer);
			if (r < 0)
				return rfx_fail(ERR_READ_FAULT, "Read error: %s", strerror(errno));
			break;	/* clean EOF */
		}
		if (!write_with_retry(dst_fd, buf, (size_t)r, done, cancel)) {
			g_timer_destroy(timer);
			return false;
		}
		done += (uint64_t)r;

		double el = g_timer_elapsed(timer, NULL);
		if (cb != NULL) {
			p.progress    = (float)((double)done / (double)target);
			p.bytes_done  = done;
			p.bytes_total = target;
			p.speed_bps   = el > 0 ? (double)done / el : 0;
			p.eta_seconds = p.speed_bps > 0 ? (double)(target - done) / p.speed_bps : -1;
			g_snprintf(p.message, sizeof(p.message), "Writing image");
			cb(&p, ud);
		}
		uprint_progress(done, target);
	}

	rfx_sync_block(dst_fd);
	double el = g_timer_elapsed(timer, NULL);
	g_timer_destroy(timer);

	char sz[32], rt[32];
	uprintf("\nWrote %s in %.1f s (%s)",
	        rfx_human_size(done, sz, sizeof(sz)), el,
	        rfx_human_rate(el > 0 ? (double)done / el : 0, rt, sizeof(rt)));
	return true;
}

/* Verify pass: re-read the device and compare against the image. */
bool rfx_disk_verify_image(int src_fd, uint64_t src_size, int dst_fd,
                           uint32_t sector_size, RfxProgressCb cb, gpointer ud,
                           RfxCancel *cancel)
{
	(void)sector_size;
	size_t buf_len = DD_BUFFER_SIZE;
	g_autofree uint8_t *a = g_malloc(buf_len);
	g_autofree uint8_t *b = g_malloc(buf_len);

	uprintf("Verifying:");

	/* dst_fd is the same handle we just wrote through, so the block device's
	 * page cache still holds our own data: pread() below would compare the
	 * cache against itself and always "pass". Drop the cache first so we
	 * actually re-read the media (docs/REVIEW.md H5). On a regular file
	 * target BLKFLSBUF is not supported and returns EINVAL/ENOTTY — ignore. */
#ifdef BLKFLSBUF
	if (ioctl(dst_fd, BLKFLSBUF, 0) != 0 && errno != EINVAL && errno != ENOTTY)
		uprintfw("Could not flush the device buffer cache: %s", strerror(errno));
#endif

	GTimer *t = g_timer_new();
	uint64_t done = 0;
	RfxProgress p = { 0 };
	p.op = RFX_OP_VERIFY;

	while (done < src_size) {
		if (rfx_cancel_requested(cancel)) {
			g_timer_destroy(t);
			return rfx_fail(ERR_CANCELLED, "Cancelled by user");
		}
		size_t chunk = (size_t)MIN(buf_len, src_size - done);
		ssize_t ra = pread(src_fd, a, chunk, (off_t)done);
		ssize_t rb = pread(dst_fd, b, chunk, (off_t)done);
		if (ra <= 0 || rb <= 0 || ra != rb) {
			g_timer_destroy(t);
			return rfx_fail(ERR_READ_FAULT,
			                "Verification read failed at offset %" G_GUINT64_FORMAT, done);
		}
		if (memcmp(a, b, (size_t)ra) != 0) {
			g_timer_destroy(t);
			for (size_t i = 0; i < (size_t)ra; i++) {
				if (a[i] != b[i])
					return rfx_fail(ERR_VERIFY_MISMATCH,
					                "Data mismatch at offset %" G_GUINT64_FORMAT
					                " (expected 0x%02X, got 0x%02X)",
					                done + i, a[i], b[i]);
			}
		}
		done += (uint64_t)ra;
		if (cb != NULL) {
			double el = g_timer_elapsed(t, NULL);
			p.progress = (float)((double)done / (double)src_size);
			p.bytes_done = done;
			p.bytes_total = src_size;
			p.speed_bps = el > 0 ? (double)done / el : 0;
			p.eta_seconds = p.speed_bps > 0 ? (double)(src_size - done) / p.speed_bps : -1;
			g_snprintf(p.message, sizeof(p.message), "Verifying");
			cb(&p, ud);
		}
		uprint_progress(done, src_size);
	}
	g_timer_destroy(t);
	uprintf("\nVerification successful");
	return true;
}
