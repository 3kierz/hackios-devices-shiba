# Pixel 8 (shiba): status

HakiOS (iOS 16.6.1 userland of the iPhone 13 mini, arm64e, on XNU) on the Google Pixel 8 (Tensor G3, Mali-G715).
Last update: 2026-11-22.

**First boot on a Pixel 8, 2026-11-22:** Google's bootloader (`fastboot boot`) started this repository's U-Boot, which
started the HakiOS loader at EL2 (`platform akita`, zuma's), which started XNU at EL1 on one core: the GICv3 found, memory
set up, the RAM disk mounted, and `Hello from user space on Darwin!` from process 1, which then exits (the kernel panics
on that, by design for this test, and its log goes back to the loader, which restarts the phone). The whole run's log is
in [boot-info/first-boot-2026-11-22-hello.txt](boot-info/first-boot-2026-11-22-hello.txt), read back in Android with
`pull-log.sh`: U-Boot appends its console to the bootloader's own log buffer, the one place that survives a restart here
(Android's pstore zones do not survive any restart on this phone). Kernel: the prebuilt one of 2026-10-25, `cpus=1`.

Beyond that first boot, this port starts from the Pixel 8a port
([Martinvlba/hakios-devices-akita](https://github.com/Martinvlba/hakios-devices-akita), 2026-11-17). On the 8a it
boots iOS from its own storage, with display, touch, GPU and Metal, sound, Wi-Fi, Bluetooth, battery, thermals and
USB working. The two phones share the SoC and most of the board. The table says what that means for the Pixel 8.

## Expected to work as on the Pixel 8a (same hardware, same addresses and pins)

| | |
|---|---|
| Boot path | Google's bootloader `fastboot boot`s HakiOS's U-Boot (header v4, LZ4, unsigned AVB footer), the loader at EL2 (`platform akita`, zuma's), XNU at EL1, all 9 cores |
| Storage | UFS through U-Boot, Darwin from a `darwin` partition, `/private/var` kept |
| GPU | Mali-G715 with HakiOS's Metal driver (the SoC's block, the phone's own CSF firmware) |
| Buttons | power, volume up/down (the same always-on pins) |
| Speakers | two CS35L41s on spi_19, the same chip selects, resets and interrupts; through the AoC |
| Flashlight | LM3644 on hsi2c_14, the same enables |
| USB | seen as an iPhone (usbmux, PTP) |
| CPU clocks, idle, RTC, power off/restart | ACPM, the SoC's |
| Battery | MAX77759 fuel gauge and charger (read only) |

## Ported: different on the Pixel 8, changed here, not yet run

| | |
|---|---|
| Screen | the Pixel 8's panel is google-shoreline: its 60/120 Hz TE commands and its brightness table (2-1000 nits, HBM to 1400) |
| Touch | Goodix Berlin-D (`goodix,brl-d`) instead of the 8a's GT9895: the same SPI, pins and protocol family; AVDD 3.3 V |
| Vibration | the same CS40L26A, a different motor: boost 11 V; f0 and ReDC from `haptic_f0=` / `haptic_redc=` (this phone's own calibration), else the tree's defaults |
| Thermals | the Pixel 8's eight board thermistors in their order, and Android's VIRTUAL-SKIN for the Pixel 8 (throttling and the skin stop at 55 C) |

## Not ported yet

- **Wi-Fi**: a Broadcom BCM4398 (the 8a's is a BCM4383). The PCIe link and the chip's identity (`pcie=5`) are the
  same code. The firmware download, rings and station are 4383 code, and they do not run unless `wifi_4398=1`.
- **Bluetooth**: the BCM4398's, on the same UART. The 8a's driver is built but not in the defaults (`bt=1` to try).
- **Cameras**: other sensors (boitata main, dokkaebi front, sandworm ultra-wide) and a laser AF. The driver is left out.
- **GNSS**: a Broadcom BCM4775 on spi@111E0000 (the 8a's is Samsung's Kepler). The driver is left out.
- **Mobile network**: the same Exynos 5300 as the 8a, which only reached signal bars there. It is off unless `modem=N`.
- **Fingerprint, sensors, sleep, NFC**: as on the 8a, not yet.

## Checked against a Pixel 8's own tree (2026-11-22, bootloader ripcurrent-17.0-15199481, Android 17)

`pull-boot-info.sh` on a Pixel 8, compared with the Pixel 8a's tree:
- the SoC's block addresses (`soc/zuma.addrs`): identical;
- the RAM (4 x 2 GiB at the same places) and the 41 fixed `/reserved-memory` regions: identical except
  pkvm_guest_firmware (0xd0000, not 0x100000; `zuma-shiba.dts` now has the Pixel 8's);
- the boot image: header version 4, LZ4 kernel, no ramdisk in boot, vendor_boot without a tree, as on the 8a; boot_a is 0x4000000
  (`fastboot getvar partition-size:boot_a`), the size the AVB footer is made for;
- the parts: google-shoreline panel, `goodix,brl-d` touch with LDO4M at 3.3 V, two CS35L41s on spi@111C0000, the
  CS40L26A at 11 V / 2.5 A with f0-default 0x244000 and redc-default 0x68000, MAX77759 + PCA9468, BCM4775 on
  spi@111E0000, Wi-Fi on pcie@13120000 (max-link-speed 3; trained to Gen2 here). All as this port assumes.

## Next

- ~~All nine cores~~ **done, 2026-11-22**: a kernel built from source (`make kernel`, with the zuma drivers in), `cpus=9`
  with every driver's default off: the loader's EL2 trampoline, cores 4-8 (the A715s and the X3) online within 10 ms, `hello`
  on cpu 5 ([boot-info/first-boot-2026-11-22-nine-cores.txt](boot-info/first-boot-2026-11-22-nine-cores.txt)). The four
  A510s (cores 0-3 by MPIDR; the kernel's 1-3) come later by design on zuma (28 s on the 8a); `hello` ended first. The
  prebuilt kernel of 2026-10-25 hangs after the jump with `cpus=9` (older than the 8a's eight-core work).
- **Touch and buttons work, 2026-11-22** ([boot-info/first-boot-2026-11-22-touch-buttons.txt](boot-info/first-boot-2026-11-22-touch-buttons.txt)):
  U-Boot powers the touch chip through ACPM (`zuma touch`: LDO4M at 3.3 V, LDO25M), the 8a's driver finds a GT9916
  (`"BERLIN"`, firmware `9916R`) with no change and tracks a swipe at 240 reports a second; power and both volume keys
  pressed and released.
- **The display, 2026-11-22** ([boot-info/first-boot-2026-11-22-display-bigsurf-120hz.txt](boot-info/first-boot-2026-11-22-display-bigsurf-120hz.txt)):
  the panel is google-bigsurf (BOE), not shoreline; the driver now has both (`panel_name=`, bigsurf by default) and takes
  the bootloader's framebuffer over (3 buffers, flipped by DPP0 between frames); bigsurf's own rate commands move it from
  the bootloader's 60 Hz to 120 Hz (60 frames in 0.5 s). Brightness and display on/off need iOS asking (a later run).
- **The Wi-Fi link, 2026-11-22** ([boot-info/first-boot-2026-11-22-wifi-pcie.txt](boot-info/first-boot-2026-11-22-wifi-pcie.txt)):
  `pcie=5`: the chip powered (WL_REG_ON), the PHY's PLL, CDR and calibration, the link up at Gen2 x1 on the first try, and
  the chip: Broadcom 14e4:4444, `chipcommon: chip 0x4398, revision 3`. Its ID word gives interconnect type 6 and no core
  count, so the 4383's core discovery (the enumeration ROM) will need looking at before the 4398's firmware can go in.
- **Haptics, a look, 2026-11-22** ([boot-info/first-boot-2026-11-22-haptic.txt](boot-info/first-boot-2026-11-22-haptic.txt)):
  `haptic=1`: the bus (PERIC1 USI0) up, the chip out of reset and answering after the wake-up try it is meant to miss:
  CS40L26A, devid 0x40a260, rev A1, its DSP running from ROM. Next, `haptic=2` (one of the ROM's effects, no tuning).
- The other drivers, one at a time: ~~`pcie=5`~~ ~~`haptic=1`~~ (the Wi-Fi link: it should
  say chip 0x4398), `amp=1`, `thermal=1`.
- The panel is **google-bigsurf** on this phone (product id 0x84a050, the bootloader's log), not shoreline: the display
  driver's rate and brightness commands are to be checked against panel-google-bigsurf.c.
- USB, touch power and UFS in U-Boot, which the 8a's unpublished U-Boot had.

## Things to check on the first run

- The framebuffer address `0xfac00000` and DECON0's `TRIG_CON` value `0x3070` (U-Boot's preboot prints which it found;
  the bootloader's log, which says it, needs root to read).
- `touchscreen=1`: the chip's version block should read "BERLIN" and the Berlin-D's id.
- `pcie=5`: `chipcommon: chip 0x4398`.
