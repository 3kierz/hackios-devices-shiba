# Device: Google Pixel 8 (shiba)

A HakiOS device repository for the **Google Pixel 8** (`shiba`, board "ZUMA SHIBA MP based on ZUMA"), ported from the
Pixel 8a port by Martin Valba ([Martinvlba/hakios-devices-akita](https://github.com/Martinvlba/hakios-devices-akita),
on [hakios-developers.com](https://hakios-developers.com/#/port/akita)). Both phones use the same SoC, so most of the
8a's work carries over. This repository changes what is different on the Pixel 8.

> **Status: not yet booted on a Pixel 8.** Everything here is written from Google's public device trees and panel
> drivers for the Pixel 8 (`kernel/devices/google/shusky`, `device/google/shusky`), compared with the Pixel 8a's
> (`kernel/devices/google/akita`). See [STATUS.md](STATUS.md) for what to expect and [device.conf](device.conf) for each part.

| What | Value |
|---|---|
| Board (the tree's `model`) | ZUMA SHIBA MP based on ZUMA |
| SoC | Google Tensor G3 (zuma): 4 x Cortex-A510, 4 x Cortex-A715, 1 x Cortex-X3; Mali-G715 |
| Compatible | `google,ZUMA SHIBA google,ZUMA` |
| Base kernel | [Martinvlba/hakios-xnu-kernel](https://github.com/Martinvlba/hakios-xnu-kernel) (from [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)) |

## Pixel 8 vs Pixel 8a: what was changed

The two boards were compared in Google's device trees, area by area:

| Part | Pixel 8a (akita) | Pixel 8 (shiba) | In this port |
|---|---|---|---|
| SoC blocks, GIC, timer, UFS, USB, PCIe, ACPM, AoC, Mali | zuma | zuma, same addresses | unchanged |
| RAM, fixed carve-outs | 8 GiB, zuma common | 8 GiB, zuma common (only the sized dma heaps differ) | unchanged; check against your phone's tree |
| Panel | google-ak3a / ak3b | **google-shoreline** (bigsurf on early boards) | `hakios.akita_display.c`: the 60 Hz TE select (`B9 11`), the TE width (`B0 00 10 B9`, `B9 ..`), brightness 209-3514 = 2-1000 nits, HBM 3515-4095 to 1400 nits |
| Touch | Goodix GT9895 (`goodix,nottingham`), AVDD 3.0 V | **Goodix Berlin-D** (`goodix,brl-d`), **AVDD 3.3 V**, the same SPI and pins | U-Boot env: LDO4M `0x3c` (3.3 V); the driver reads the chip's own layout |
| Buttons | gpio keys | same | unchanged |
| Speaker amps | 2 x CS35L41 on spi_19 | same, the same pins | unchanged |
| Haptics | CS40L26A, 10 V boost, f0 170 Hz | same chip and pins, **11 V boost, f0 145 Hz** | boost 11 V; f0/ReDC from `haptic_f0=`/`haptic_redc=`, else the tree's 0x244000/0x68000 |
| Flashlight | LM3644 on hsi2c_14, gpp8-2 | same | unchanged |
| Battery / charger | MAX77759 | MAX77759 + PCA9468 charge pump | unchanged (read only) |
| Thermistors | modem, quiet, rffe, usb_conn, soc, charger, display, gnss_tcxo | **neutral, quiet, charge, usb_pwr, cam, disp, north, soc** | `hakios.akita_thermal.c`: new names, soc_therm on channel 7, the Pixel 8's VIRTUAL-SKIN and VIRTUAL-SKIN-CHARGE (9 and 7 weighted sums, `thermal_info_config_shiba.json`) |
| Wi-Fi | Broadcom BCM4383 on PCIe 1 (Gen2) | **Broadcom BCM4398** on PCIe 1 | the link and the chip's id (`pcie=5`) expect 0x4398; the BCM4383 firmware steps refuse to run without `wifi_4398=1` |
| Bluetooth | BCM4383 over the APM UART | BCM4398, the same UART and pins | built, not in the defaults (`bt=1` to try) |
| Cameras | IMX787 main, IMX712 front and UW | **boitata, dokkaebi, sandworm** + laser AF | driver left out (`# CONFIG_HAKIOS_AKITA_CAMERA is not set`) |
| GNSS | Samsung Kepler, through the modem | **Broadcom BCM4775** on spi@111E0000 | driver left out |
| Modem | Exynos 5300 on PCIe 0 | Exynos 5300, the same PCIe setup | unchanged (off unless `modem=N`) |

### Why the drivers are still called `akita_*`

The base kernel refers to the zuma drivers by name: `HAKIOS_AKITA_*` in `drivers/core/hakios.stubs.c`, the
`hakios.akita_*.c` lines in `xnu-12377/hakios.patch`, and `platform akita` in the loader (`xnuloader/loader.c`, found
by `google,zuma`). A Pixel 8 build therefore uses the same driver names and the same loader platform. Treat `akita_*`
as the name of the zuma driver family. Everything that belongs to the device itself is named `shiba`:
`CONFIG_DEVICE="shiba"`, `configs/shiba*`, `devices/shiba`, the U-Boot files, `mkubootimg.py -d shiba`.

The base kernel's `userland/build.sh` also hard-codes a few paths named after akita (`../fw-modem-akita`,
`../fw-camera-akita`, `../devices/akita/tools/mkcarrierpack.py`). Those are only used for the modem's carrier pack and
the 8a's camera tuning. Point them at `shiba`, or leave them unused.

## Before the first boot

1. **Unlock the bootloader** (Settings > Developer options > OEM unlocking, then `fastboot flashing unlock`). This wipes the phone.
2. **Pull this phone's own facts** (Android, `adb root` on a userdebug build, or Magisk):
   ```sh
   devices/shiba/pull-boot-info.sh       # into boot-info/: fdt.dtb, the boot headers, the bootloader's log, summary.txt
   cp devices/shiba/boot-info/fdt.dtb devices/shiba/prebuilt/device-tree.dtb   # the tree soc/ and u-boot/ are checked against
   ```
   Then check against them:
   - `u-boot/files/dts/upstream/src/arm64/exynos/google/zuma-shiba.dts`: the `memory@` banks and the fixed `/reserved-memory`
     regions (carried over from the 8a);
   - `u-boot/boot-image`: header version 4, LZ4 kernel, and `avb_footer` = `fastboot getvar partition-size:boot_a`;
   - `soc/zuma.addrs`: regenerate with `soc/zuma-addrs.py boot-info/fdt.dtb`.
3. **Haptics calibration** (optional): as root, `cat /sys/bus/i2c/devices/*-0043/calibration/{f0_stored,redc_stored}` and add
   `haptic_f0=0x... haptic_redc=0x...` to the command line.
4. **Firmware** (never committed): the Mali CSF firmware, the AoC image, the amps' and haptics' tunings from `/vendor/firmware`,
   as for the 8a (see the base kernel's `userland/build.sh`).

## First boot

### HakiOS's U-Boot on the Pixel 8

The Pixel 8a port booted through a U-Boot of its own that was never published (hakios-uefi commit `1c65228`). This
repository brings its own, built from public sources:

- upstream U-Boot (v2026.07) and its generic Exynos phone board (`board/samsung/exynos-mobile`);
- HakiOS's `hakios` command from [oosexgang/hakios-uefi pull request 2](https://github.com/oosexgang/hakios-uefi/pull/2),
  which finds the loader and payload the boot image carries behind U-Boot;
- `u-boot/zuma.patch` and `u-boot/files/board/samsung/exynos-mobile/hakios_zuma.c` (`CONFIG_HAKIOS_ZUMA`): U-Boot's load
  address noted for `hakios load`, U-Boot run in the highest RAM bank (clear of every carve-out), `zuma wdt` (both
  cluster watchdogs stopped, cl0 re-armed as a slow net the kernel feeds), and a `pstore` console in Android's ramoops
  console zone, after the bootloader's own log, which the next bootloader keeps (read it back with `devices/shiba/pull-log.sh`).

Not in it yet: USB, the touch screen's power, UFS and the slot marking the 8a's U-Boot had. None is needed for the
first boot, which runs `hello` from RAM.

### Building and running

Once, on the build machine (WSL is fine for building):

```sh
sudo apt install clang lld lz4 7zip                     # the loader (clang, lld-link), the boot image (lz4)
git -C uefi fetch origin pull/2/head:pr2 && git -C uefi checkout pr2   # after tools/hakios-device.py sync hakios-uefi
```

An AArch64 GCC for U-Boot: `sudo apt install gcc-aarch64-linux-gnu`, or Arm's toolchain unpacked anywhere with its
`bin/` on `PATH` (`aarch64-none-elf-`).

The kernel from source (`make kernel`) needs theos in `~/theos` with its Linux toolchain in `toolchain/linux/iphone/`
(L1ghtmann's `iOSToolchain-x86_64.tar.xz`, which unpacks as `linux/iphone`) and `sdks/iPhoneOS16.5.sdk` (from
github.com/theos/sdks). On Ubuntu two links were also needed in the build's own tools (built here 2026-11-22):

```sh
ln -s ~/theos/toolchain/linux/iphone/bin/dsymutil xnu-deps/host/bin/dsymutil   # the build's xcrun finds none otherwise
ln -s /usr/bin/python3 xnu-deps/host/bin/python                                # the lldb macros run `python`
```

Exit status 2 with `make-kernel: built: hakios-kernel` is the kext symbol-set checks, which fail on this configuration.

`hello` from source (`make hello SHELL=/bin/bash`: `xnu-env.sh` wants bash) is what `mk-first-boot.sh` takes when it is
there; the prebuilt one predates `hello_wait=N`, which keeps a test run up N seconds. The kernel: `make kernel` (theos with `sdks/iPhoneOS16.5.sdk`), or, for this
first run only, `cp prebuilt/hakios-kernel hakios-kernel` (older, and without the zuma drivers, which this run does not use).

```sh
FIRST_ARGS="cpus=1" devices/shiba/mk-first-boot.sh   # build/shiba/hakios-shiba.img: U-Boot, the loader, the kernel, hello
adb reboot bootloader
fastboot boot build/shiba/hakios-shiba.img           # from Windows' platform-tools: WSL2 does not see the phone
```

`fastboot boot` runs the image from RAM only. Nothing is flashed, and a restart returns the phone to Android.

| Seen | Not seen: look at |
|---|---|
| U-Boot's banner | nothing at all: the framebuffer's address (`zuma-shiba.dts`; `pull-boot-info.sh` reads the bootloader's log for it), the boot image's kind (`u-boot/boot-image`) |
| `[oosexgang] watchdog 0x10060000: ... counting, stopped`, then `kept as a net` | a restart about two minutes in: the watchdogs (`zuma wdt`, preboot) |
| `[oosexgang] display: DECON0 trigger unmasked` | `TRIG_CON is not 0x3070`: the Pixel 8's bootloader leaves DECON0 otherwise (`hakios-shiba.env`, preboot) |
| `[oosexgang] hakios: payload of ...`, `loader of ...` | `the boot image has no ramdisk` or `nothing known at`: cmd/hakios.c's container search |
| `== XNULOADER v3 ==`, `CurrentEL=2`, `platform akita` | `platform qemu-virt`: U-Boot's tree did not reach the loader |
| `jumping to ...`, then the kernel's lines | a reset or a hang right after `jumping`: the drop to EL1 |
| `Hello from user space on Darwin!`, then a restart into Android | the run's log is in pstore: `devices/shiba/pull-log.sh` (root in Android) |

`platform akita` is the loader's name for every zuma phone, so it is the expected line on a Pixel 8 too.

Then, one part at a time, as the 8a's port did: `touchscreen=1`, `buttons=1`, `display_probe=1`, `pcie=5` (the Wi-Fi
link: it should say `chip 0x4398`), `haptic=1`, `amp=1`, `thermal=1`. Each says in the log what it found.

## Configuration

```
configs/shiba.config          CONFIG_DEVICE="shiba", the zuma drivers built in, CONFIG_DEFAULTS
configs/shiba_defconfig       #include common_defconfig / zuma.config / shiba.config
```

`make DEVICE=shiba shiba_defconfig` builds the config. `tools/hakios-device.py check shiba` checks this repository
against the base kernel; it passes.

## The repository

| File | What |
|---|---|
| `device.conf` | what the device is, and how far each part works |
| `configs/` | `shiba_defconfig`, `shiba.config` |
| `soc/` | the zuma family's layer (`soc.conf`, `configs/zuma.config`, `zuma.addrs`), the same as the 8a's |
| `drivers/` | the zuma phone drivers (`hakios.akita_*.c`), with the Pixel 8 changes above. Their dated notes ("11-09", "seen on the phone", measured currents) are the Pixel 8a's |
| `u-boot/` | HakiOS's U-Boot for the Pixel 8: `hakios-shiba.env`, `hakios_shiba_defconfig`, `zuma-shiba.dts`, `boot-image` |
| `tools/`, `tests/` | the 8a port's helpers (usbmux, screenshots, logs, bootrun), with paths renamed to `shiba` |
| `mk-first-boot.sh`, `pull-boot-info.sh` | the first boot and the phone's facts |

Credits: the zuma drivers, U-Boot setup and tools are Martin Valba's Pixel 8a work. The HakiOS kernel, loader and
userland are oosexgang's.
