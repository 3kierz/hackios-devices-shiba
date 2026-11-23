#!/bin/bash
# The Pixel 8's first HakiOS boot (docs/porting/index.md, step 2): one boot image for `fastboot boot`, nothing flashed.
#
#   devices/shiba/mk-first-boot.sh            -> build/shiba/hakios-shiba.img
#
# In it: HakiOS's U-Boot for shiba (devices/shiba/u-boot), and in the ramdisk the HakiOS loader with a payload of the
# kernel as built (make kernel) and the Apple-free `hello` (prebuilt/userland/hello) as user space, with this
# configuration's boot-arguments (configs/shiba.config: one processor, no storage).
#
# What to expect on the phone, in order, all on the screen (there is no USB or log partition under U-Boot on zuma yet):
#   U-Boot's banner; "[oosexgang] hakios: the container is N bytes into the ramdisk", "payload of ...", "loader of ...";
#   the loader's "== XNULOADER v3 ==", "CNTFRQ=...", "platform akita"; the kernel's lines; "Hello from user space on
#   Darwin!"; then the phone restarts by itself (into Android). A step that is not reached is the one to look at
#   (devices/shiba/README.md, "First boot").
#
# Needs: make kernel (hakios-kernel; prebuilt/hakios-kernel copied there will do for this first run); an AArch64 GCC for
# U-Boot (uefi/u-boot/build.sh, CROSS_COMPILE=...; LLVM's linker does not link a working U-Boot); clang and lld-link for
# the loader; uefi/ (tools/hakios-device.py sync hakios-uefi) with pull request 2 of oosexgang/hakios-uefi (hakios load
# finds the container behind U-Boot): git -C uefi fetch origin pull/2/head:pr2 && git -C uefi checkout pr2. u-boot/ is made
# by uefi/u-boot/apply.sh; this device's files and devices/shiba/u-boot/zuma.patch are put in by this script.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../.."
out=build/shiba
mkdir -p "$out"

[ -f hakios-kernel ] || { echo "mk-first-boot.sh: no hakios-kernel: make kernel first" >&2; exit 1; }
[ -f xnuloader/identity.h ] || PLACEHOLDER=1 xnuloader/mkidentity.sh
xnuloader/build.sh >/dev/null
python3 tools/hakios-config.py check >/dev/null

# the configuration's arguments, or FIRST_ARGS=... instead (FIRST_ARGS="cpus=1": one core, as the 8a's first run)
# hello built from source (make hello: hello_wait= and the drivers' sysctls) when there is one, else the prebuilt one
# (userland/hello.arm64 first: userland/build.sh rebuilds userland/hello for the unpacked user space -- arm64e for 16.6.1
# d16ap -- and the kernel refuses that static program as process 1 even with hakios_arm64e=1, EBADARCH, 2026-11-23;
# make hello builds it arm64: cp userland/hello userland/hello.arm64 after it)
hello=userland/hello.arm64; [ -f "$hello" ] || hello=userland/hello; [ -f "$hello" ] || hello=prebuilt/userland/hello
echo "mk-first-boot.sh: user space: $hello"
# FW_IMAGE=userland/root.apfs: the small image carried too, as the second RAM disk (md1), hello staying the root (md0,
# the loader's resident image): the drivers read the phone's firmware from its fw/ without a root mounted
# (drivers/storage/hakios.image.c, hakios_image_file) -- haptic=3, amp=3, aoc=3, the Mali
# FLASH_ROOT=1: iOS from the phone's flash (INSTALL-PLAN.md, phase 4): the boot volume (userland/flashboot.apfs, md0,
# resident) and the small image (userland/root.apfs, md1), both made by
#   NO_SYSTEM=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh
# and the kernel's built-in root_apfs=ufs_darwin: init mounts Apple's root volume from darwin (ufs=2 among the
# arguments), the two cryptexes behind it. Without ufs_write=1 nothing is written: /private/var is in memory.
if [ -n "${FLASH_ROOT:-}" ]; then
	[ -f userland/flashboot.apfs ] || { echo "mk-first-boot.sh: no userland/flashboot.apfs (FLASHBOOT=1 userland/build.sh)" >&2; exit 1; }
	# Darwin's launchd asks for the iPhone it runs on ("failed to query hardware target: 22" with the placeholder,
	# 2026-11-23): the loader has to carry that iPhone's identity, xnuloader/mkidentity.sh (from sysimage's device tree)
	if grep -q hakios-placeholder xnuloader/identity.h; then
		echo "mk-first-boot.sh: xnuloader/identity.h is the placeholder: run xnuloader/mkidentity.sh first" >&2; exit 1
	fi
	disks="resident=userland/flashboot.apfs ramdisk=userland/root.apfs"
	echo "mk-first-boot.sh: flash root: flashboot.apfs $(( $(stat -c %s userland/flashboot.apfs) / 1048576 )) MB (md0), root.apfs $(( $(stat -c %s userland/root.apfs) / 1048576 )) MB (md1)"
elif [ -n "${FW_IMAGE:-}" ]; then
	disks="resident=$hello ramdisk=$FW_IMAGE"
	echo "mk-first-boot.sh: firmware image: $FW_IMAGE ($(( $(stat -c %s "$FW_IMAGE") / 1048576 )) MB, md1)"
else
	disks="ramdisk=$hello"
fi
args=${FIRST_ARGS-$(python3 tools/hakios-config.py args)}
# hakios_arm64e=1: the kernel execs arm64e programs only with it, and userland/build.sh builds hello for the unpacked user
# space's architecture (arm64e for 16.6.1 d16ap); harmless for an arm64 one (the 8a's bootrun.sh always gives it)
case "$args" in *hakios_arm64e=*) ;; *) args="$args hakios_arm64e=1";; esac
echo "mk-first-boot.sh: kernel $(strings hakios-kernel | grep -m1 'Darwin Kernel Version' | cut -c1-90)"
echo "mk-first-boot.sh: arguments: -v $args"
xnuloader/mkpayload.py -o "$out/payload.bin" kernel=hakios-kernel $disks --cmdline "-v $args"

[ -d u-boot ] || uefi/u-boot/apply.sh
cp -r "$here/u-boot/files/." u-boot/          # this device's U-Boot files, as apply.sh does at the start
cp uefi/u-boot/files/cmd/hakios.c u-boot/cmd/hakios.c
# UBOOT_ENV="name=value" lines (one per line) added to the environment for one experiment, after the device's own (the
# later of two settings is the one U-Boot keeps); OUT=name.img for the image's name
if [ -n "${UBOOT_ENV:-}" ]; then
	printf '\n/* added by mk-first-boot.sh for this build (UBOOT_ENV) */\n%s\n' "$UBOOT_ENV" \
		>> u-boot/board/samsung/exynos-mobile/hakios-shiba.env
	echo "mk-first-boot.sh: U-Boot environment added:"; echo "$UBOOT_ENV" | sed 's/^/  /'
fi
# the zuma pieces in the generic Exynos phone board (hakios_zuma.c built, U-Boot's load address noted): once
# (the board's own files back to upstream's first, so that the patch as it is now goes onto them, whatever was applied before)
git -C u-boot checkout -q -- board/samsung/exynos-mobile/exynos-mobile.c board/samsung/exynos-mobile/Makefile \
	board/samsung/exynos-mobile/Kconfig
git -C u-boot apply "$here/u-boot/zuma.patch" || { echo "mk-first-boot.sh: devices/shiba/u-boot/zuma.patch does not apply" >&2; exit 1; }
grep -q hk_load_base u-boot/cmd/hakios.c ||
	{ echo "mk-first-boot.sh: uefi/ has no hakios load for a container behind U-Boot (oosexgang/hakios-uefi pull request 2)" >&2; exit 1; }
uefi/u-boot/build.sh shiba

# mkubootimg.py wants the phone's own tree there (a Pixel's image does not carry it: the bootloader hands over its own);
# pull-boot-info.sh leaves it in boot-info/ (kept out of git: it carries the phone's identifiers)
if [ ! -f "$here/prebuilt/device-tree.dtb" ]; then
	[ -f "$here/boot-info/fdt.dtb" ] || { echo "mk-first-boot.sh: no boot-info/fdt.dtb: run devices/shiba/pull-boot-info.sh first" >&2; exit 1; }
	cp "$here/boot-info/fdt.dtb" "$here/prebuilt/device-tree.dtb"
fi
python3 tools/mkubootimg.py -d shiba -p "$out/payload.bin" -o "$out/${OUT:-hakios-shiba.img}"
echo
echo "mk-first-boot.sh: $out/${OUT:-hakios-shiba.img}. On a machine where fastboot reaches the phone (Windows' platform-tools):"
echo "  adb reboot bootloader"
echo "  fastboot boot $out/${OUT:-hakios-shiba.img}"
echo "Nothing is flashed: a restart gives the phone back to Android."
