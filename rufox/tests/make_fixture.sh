#!/bin/sh
# Builds the ISO fixture used by tests/test_core.c.
#
# The fixture reproduces the shape of a real Ubuntu live ISO closely enough to
# exercise every analysis rule: an isohybrid MBR, ISOLINUX with a version
# banner, a GRUB config, a UEFI bootloader, a /casper tree and a kernel command
# line that our config patcher must rewrite.
set -e

OUT="${1:-/tmp/rufox-fixture.iso}"
SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT

mkdir -p "$SRC/casper" "$SRC/efi/boot" "$SRC/isolinux" "$SRC/boot/grub"

head -c 3145728 /dev/urandom > "$SRC/casper/vmlinuz"
head -c 6291456 /dev/urandom > "$SRC/casper/initrd.lz"
head -c 41943040 /dev/zero     > "$SRC/casper/filesystem.squashfs"
printf 'fixture\n' > "$SRC/README.txt"

cp /usr/lib/ISOLINUX/isolinux.bin "$SRC/isolinux/isolinux.bin" 2>/dev/null || \
    head -c 38912 /dev/urandom > "$SRC/isolinux/isolinux.bin"

cat > "$SRC/isolinux/isolinux.cfg" <<'CFG'
default live
label live
  kernel /casper/vmlinuz
  append boot=casper initrd=/casper/initrd.lz file=/cdrom/preseed/ubuntu.seed quiet splash
CFG

cat > "$SRC/boot/grub/grub.cfg" <<'CFG'
set default=0
menuentry "Try Ubuntu" {
	linux /casper/vmlinuz boot=casper initrd=/casper/initrd.lz quiet splash ---
	initrd /casper/initrd.lz
}
CFG

# A plausible EFI loader; small enough to look like Mint's dangling symlink
# only if we wanted to, so keep it a real size here.
head -c 900000 /dev/urandom > "$SRC/efi/boot/bootx64.efi"

xorriso -as mkisofs -o "$OUT" \
    -isohybrid-mbr /usr/lib/ISOLINUX/isohdpfx.bin \
    -c isolinux/boot.cat -b isolinux/isolinux.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot -e efi/boot/bootx64.efi -no-emul-boot -isohybrid-gpt-basdat \
    -V "UBUNTU 24.04 LTS" -r -J -joliet-long "$SRC" >/dev/null 2>&1

echo "wrote $OUT"
