/*
 * rufox — unit tests
 *
 * These exercise the parts that Rufus gets right by construction and that a
 * port can silently break: label sanitisation, the partition planner's
 * geometry, the FAT32 cluster-size gate, MBR surgery and image analysis.
 * Every expectation here was checked against a hex dump of a real Rufus-made
 * stick or against Rufus' own source (src/format.c, src/drive.c, src/rufus.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rufox.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int failures = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                                  \
	checks++;                                                       \
	if (!(cond)) {                                                  \
		failures++;                                                 \
		fprintf(stderr, "FAIL %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
	}                                                               \
} while (0)

#define CHECK_EQ(a, b, fmt, ...) CHECK((a) == (b), fmt, ##__VA_ARGS__)

/* ----------------------------------------------------------------------- */
static void test_labels(void)
{
	char out[64];

	/* FAT labels: 11 chars max, upper-cased, no dots/spaces/colons */
	rfx_format_label_valid(RFX_FS_FAT32, "ubuntu 24.04 lts", out, sizeof(out));
	CHECK(strcmp(out, "UBUNTU2404L") == 0, "FAT label sanitised to '%s'", out);

	rfx_format_label_valid(RFX_FS_FAT32, "TOOLONGVOLUMENAME", out, sizeof(out));
	CHECK(strlen(out) == 11, "FAT label truncated to 11 chars, got %zu", strlen(out));

	rfx_format_label_valid(RFX_FS_FAT16, "a.b:c", out, sizeof(out));
	CHECK(strcmp(out, "ABC") == 0, "FAT16 strips separators, got '%s'", out);

	/* ext labels keep case and go up to 16 chars */
	rfx_format_label_valid(RFX_FS_EXT4, "casper-rw", out, sizeof(out));
	CHECK(strcmp(out, "casper-rw") == 0, "ext4 label kept as-is, got '%s'", out);
}

/* ----------------------------------------------------------------------- */
static void test_cluster_gate(void)
{
	/* 8 GB stick, 32 KB clusters: 262144 clusters, a legal FAT32 */
	CHECK(rfx_format_cluster_allowed(RFX_FS_FAT32, 32 * 1024, 8ULL * 1024 * 1024 * 1024),
	      "8 GB / 32 KB clusters must be allowed");

	/* 100 MB with 32 KB clusters gives 3200 clusters: that is FAT16, so the
	 * combo box must not offer it. */
	CHECK(!rfx_format_cluster_allowed(RFX_FS_FAT32, 32 * 1024, 100ULL * 1024 * 1024),
	      "100 MB / 32 KB clusters must be refused");

	/* Non power-of-two clusters never exist on FAT. */
	CHECK(!rfx_format_cluster_allowed(RFX_FS_FAT32, 3000, 8ULL * 1024 * 1024 * 1024),
	      "3000-byte clusters are not a FAT thing");
	CHECK(!rfx_format_cluster_allowed(RFX_FS_FAT32, 64 * 1024, 8ULL * 1024 * 1024 * 1024),
	      "64 KB exceeds the FAT32 cluster ceiling");
}

/* ----------------------------------------------------------------------- */
static void test_partition_plan(void)
{
	RfxDrive d;
	memset(&d, 0, sizeof(d));
	g_strlcpy(d.DevicePath, "/dev/test", sizeof(d.DevicePath));
	d.DiskSize       = 8ULL * 1024 * 1024 * 1024;
	d.SectorSize     = 512;
	d.SectorsPerTrack= 63;

	RfxOptions o;
	rfx_options_default(&o);
	o.partition_scheme = RFX_PS_GPT;
	o.fs_type = RFX_FS_FAT32;
	g_strlcpy(o.label, "TEST", sizeof(o.label));

	RfxPartSpec plan[RFX_MAX_PARTITIONS];
	int n = rfx_partition_plan(&d, &o, NULL, 0, plan);
	CHECK_EQ(n, 1, "plain GPT layout must have exactly one partition, got %d", n);
	CHECK_EQ(plan[0].Offset, 1ULL * 1024 * 1024,
	         "GPT first partition must start at 1 MiB, got %" G_GUINT64_FORMAT,
	         plan[0].Offset);
	/* The GPT backup header+table reserve 33 sectors at the tail; Rufus then
	 * floor-aligns the main partition to a track, so it ends at most one track
	 * before that reservation. */
	uint64_t tail = d.DiskSize - 33ULL * 512;
	CHECK(plan[0].Offset + plan[0].Size <= tail &&
	      plan[0].Offset + plan[0].Size >= tail - 63ULL * 512,
	      "GPT main partition must end at the 33-sector reservation "
	      "(ends %" G_GUINT64_FORMAT ", tail %" G_GUINT64_FORMAT ")",
	      plan[0].Offset + plan[0].Size, tail);
	CHECK(plan[0].TypeGuid == 3, "main partition must be Microsoft Basic Data");

	/* MBR + "fixes for old BIOSes": doubled track offset, as Rufus does so
	 * that GRUB2 still fits in the embedding area. */
	o.partition_scheme = RFX_PS_MBR;
	o.old_bios_fixes = true;
	n = rfx_partition_plan(&d, &o, NULL, 0, plan);
	CHECK(n >= 2, "MBR+compat layout needs the extra partition, got %d", n);
	CHECK_EQ(plan[0].Offset, 2ULL * 63 * 512,
	         "old-BIOS layout must start at a doubled track, got %" G_GUINT64_FORMAT,
	         plan[0].Offset);
	CHECK_EQ(plan[n - 1].Size, 63ULL * 512,
	         "the BIOS compatibility partition is exactly one track");

	/* Persistence: Ubuntu live images get casper-rw at the end. */
	RfxImgReport img;
	memset(&img, 0, sizeof(img));
	img.is_iso = true;
	img.uses_casper = true;
	o.partition_scheme = RFX_PS_GPT;
	o.old_bios_fixes = false;
	o.persistence_size = 2ULL * 1024 * 1024 * 1024;
	n = rfx_partition_plan(&d, &o, &img, 0, plan);
	CHECK_EQ(n, 2, "GPT + persistence must produce two partitions, got %d", n);
	CHECK(strcmp(plan[1].Name, "Linux Persistence") == 0,
	      "second partition must be the persistence one, got '%s'", plan[1].Name);
	CHECK_EQ(plan[1].Size, 2ULL * 1024 * 1024 * 1024,
	         "persistence size must be honoured");
	CHECK(plan[0].Offset + plan[0].Size <= plan[1].Offset,
	      "the partitions must not overlap");
}

/* ----------------------------------------------------------------------- */
static void test_size_parsing(void)
{
	CHECK_EQ(rfx_parse_size("4G"), 4ULL * 1024 * 1024 * 1024, "4G");
	CHECK_EQ(rfx_parse_size("512M"), 512ULL * 1024 * 1024, "512M");
	CHECK_EQ(rfx_parse_size("1024"), 1024ULL, "plain bytes");
	CHECK_EQ(rfx_parse_size("1.5G"), 1536ULL * 1024 * 1024, "fractional 1.5G");
}

/* ----------------------------------------------------------------------- */
static void test_human(void)
{
	char buf[32];
	rfx_human_size(1536, buf, sizeof(buf));
	CHECK(strcmp(buf, "1.5 KB") == 0, "1536 -> '%s'", buf);
	rfx_human_size(5ULL * 1024 * 1024 * 1024, buf, sizeof(buf));
	CHECK(strcmp(buf, "5.0 GB") == 0, "5 GiB -> '%s'", buf);
}

/* ----------------------------------------------------------------------- */
static void test_mbr_surgery(void)
{
	/* Build a scratch image, write a synthetic MBR, then let the fixups run. */
	char path[] = "/tmp/rufox-test-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return;
	if (ftruncate(fd, 64 * 1024 * 1024) != 0) { close(fd); unlink(path); return; }

	uint8_t mbr[512];
	memset(mbr, 0, sizeof(mbr));
	mbr[0x1BE + 4] = 0x0B;	/* FAT32 CHS, what libparted leaves behind */
	mbr[510] = 0x55; mbr[511] = 0xAA;
	CHECK(rfx_write_exact(fd, mbr, sizeof(mbr), 0), "seed MBR");

	CHECK(rfx_disk_fixup_mbr(fd, RFX_FS_FAT32, true, false, 512), "fixup");

	uint8_t back[512];
	CHECK(rfx_read_exact(fd, back, sizeof(back), 0), "read back");
	CHECK_EQ(back[0x1BE + 4], 0x0C, "FAT32 must be marked LBA (0x0C)");
	CHECK_EQ(back[0x1BE], 0x80, "boot indicator must be set");
	CHECK_EQ(back[510], 0x55, "signature low byte");
	CHECK_EQ(back[511], 0xAA, "signature high byte");

	/* A masqueraded WinPE drive gets 0x81, exactly like Rufus' WriteMBR(). */
	memset(mbr, 0, sizeof(mbr));
	mbr[0x1BE + 4] = 0x0C;
	mbr[510] = 0x55; mbr[511] = 0xAA;
	CHECK(rfx_write_exact(fd, mbr, sizeof(mbr), 0), "reseed");
	CHECK(rfx_disk_fixup_mbr(fd, RFX_FS_FAT32, true, true, 512), "masquerade fixup");
	CHECK(rfx_read_exact(fd, back, sizeof(back), 0), "read back 2");
	CHECK_EQ(back[0x1BE], 0x81, "masquerade boot indicator");

	close(fd);
	unlink(path);
}

/* ----------------------------------------------------------------------- */
static void test_iso_scan(void)
{
	/* The fixture is produced by tests/make_fixture.sh; if it is missing the
	 * test is skipped rather than failed. */
	const char *iso = getenv("RUFOX_TEST_ISO");
	if (iso == NULL) iso = "/tmp/rufox-fixture.iso";
	if (!g_file_test(iso, G_FILE_TEST_EXISTS)) {
		fprintf(stderr, "SKIP iso scan: no fixture at %s\n", iso);
		return;
	}

	RfxImgReport rep;
	memset(&rep, 0, sizeof(rep));
	CHECK(rfx_iso_scan(iso, &rep, NULL), "scan %s", iso);
	CHECK(rep.is_iso, "fixture must be recognised as an ISO");
	CHECK(rep.is_bootable_img, "fixture carries an isohybrid MBR");
	CHECK(rep.uses_casper, "fixture has /casper");
	CHECK(rep.has_efi != 0, "fixture has a UEFI bootloader");
	CHECK(rep.sl_version != 0, "fixture's ISOLINUX version must be detected");
	CHECK(rep.usb_label[0] != '\0', "a FAT-safe label must be proposed");
	CHECK(rep.projected_size > 0 && rep.projected_size <= rep.image_size,
	      "projected size must be sane");
}

/* ----------------------------------------------------------------------- */
static void test_compression_sniff(void)
{
	/* A .xz suffix must be reported even before any content check. */
	char path[] = "/tmp/rufox-xz-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return;
	char *renamed = g_strdup_printf("%s.xz", path);
	const uint8_t magic[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	CHECK(rfx_write_exact(fd, magic, sizeof(magic), 0), "write xz magic");
	close(fd);
	rename(path, renamed);

	RfxImgReport rep;
	memset(&rep, 0, sizeof(rep));
	CHECK(rfx_iso_scan(renamed, &rep, NULL), "scan the xz-named file");
	CHECK_EQ(rep.compression_type, RFX_BLED_XZ, "xz must be sniffed");
	CHECK(rep.disable_iso, "compressed images must not be extracted");

	unlink(renamed);
	g_free(renamed);
	unlink(path);
}

/* ----------------------------------------------------------------------- */
int main(void)
{
	rfx_log_set_level(RFX_LOG_ERROR);	/* keep the test output readable */

	test_labels();
	test_cluster_gate();
	test_partition_plan();
	test_size_parsing();
	test_human();
	test_mbr_surgery();
	test_iso_scan();
	test_compression_sniff();

	printf("%d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
