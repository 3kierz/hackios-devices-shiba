# Full iOS on the Pixel 8: the install plan (for review, nothing done yet)

Written 2026-11-23, after every part that runs without iOS was seen working (STATUS.md). **Nothing in this document
has been done to the phone.** Each phase ends at a point where you decide whether to go on.

## What changes, in one paragraph

iOS's system is too big for `fastboot boot` (2.5 GB against a 64 MB boot image), so it has to live on the phone's own
storage, in a partition of its own (`darwin`) cut from the end of Android's `userdata`. **Cutting it erases everything
in Android's data partition.** Android itself (system, vendor, the bootloader, both slots) is not touched, and the phone
keeps booting Android exactly as now; HakiOS still starts only when you `fastboot boot` its image from the computer.
For HakiOS to read the partition, our U-Boot needs a UFS (storage) driver, which does not exist yet.

## The facts this rests on (read from your phone, 2026-11-23)

| | |
|---|---|
| Storage | UFS, LUN 0 is `sda`: 249,569,280 sectors of 512 bytes (128 GB) |
| `userdata` | `sda34`, **the last partition of LUN 0**: starts at sector 19,599,152, 229,970,088 sectors (117.7 GB) |
| Your data in it | **81 GB used of 110 GB** |
| Identity/radio partitions | `persist`, `efs`, `efs_backup`, `modem_userdata`, `trusty_persist`, `trusty_userdata` (LUN 0, before `userdata`): never written by this plan |
| Boot chain | Google's bootloader on other LUNs (`sdb`, `sdc`, `sdd`): never written by this plan |
| iOS images built | `userland/root.apfs` (small image, 33 MB), from your 16.6.1 IPSW. `system.apfs` (about 2.5 GB) **not yet**: its build stalled in `tools/apfs/mkapfs.py` three minutes in (eight workers idle, its spool unchanged for 7.5 hours) and was stopped, 2026-11-23; to be made again before phase 4 (one worker, or the stall found) |

## How the 8a did it, and what is different here

The Pixel 8a port (same SoC) runs iOS from a `darwin` partition holding Apple's root volume as it comes from the IPSW,
the two cryptexes, and an HFS+ data volume for `/private/var`; its boot image was flashed to **slot b**, Android kept
slot a. The kernel never drives the storage itself: it asks the loader, which reads through the firmware's block device
(`drivers/storage/hakios.ufs.c`), and that firmware is **U-Boot's UFS driver, which the 8a's author never published**.
The kernel repository's install tools (`tools/backup-partitions.py`, `userdata-carve.py`, `darwin-write.py`) were
written for the POCO X5 Pro: the backup list uses Qualcomm partition names and `darwin-write.py` has that phone's exact
partition positions built in. They need Pixel 8 versions before they touch anything.

## Phase 0: back up (you, then the tools; nothing written to the phone)

1. **Your own data.** Photos, messages, app data, 2FA apps, anything in `/sdcard`: Google backup, or copy off by hand.
   After phase 3 it is gone from the phone. With 81 GB used, plan the time for this.
2. **The partitions that cannot be made again** and the partition table: a Pixel 8 list for `backup-partitions.py`
   (read only: `dd if=`, hashed on both sides): `persist efs efs_backup modem_userdata trusty_persist trusty_userdata
   devinfo misc`, both slots' `boot init_boot vendor_boot vendor_kernel_boot dtbo vbmeta*`, and the first and last 8
   blocks (4 KB) of every LUN. Kept on the computer, never in git (they hold the phone's identity).
3. **A way back that does not need Android:** before anything is written, check that Google's factory image for your
   build (or the Android Flash Tool) can restore the phone **from fastboot alone**, including whether it rewrites LUN 0's
   partition table. *Not yet verified; this plan does not go to phase 3 until it is.*

## Phase 1: UFS in U-Boot, read only (no risk to the phone)

The work that has to come first, and is safe: a U-Boot driver that gives the loader the phone's storage **to read**.

1. A probe (`zuma ufs`), like the earlier `zuma bldrlog`: the UFS host controller's registers as Google's bootloader
   leaves them at hand-over (enabled? link up? which gear?). This decides between two designs:
   **Done 2026-11-23: LEFT UP.** UFSHCI 3.0 (32 slots), HCE 1, HCS 0x10f (device present, both lists and UIC ready,
   power mode switched), both lists running (UTRLRSR 1, the bootloader's list at 0xf8c3a000 in its own reserved memory),
   nothing in flight, the PHY out of isolation. So the first design:
   - **the bootloader leaves it up:** U-Boot adopts the controller as it is (standard UFSHCI: a request list, SCSI
     READ), with no reset or power change -- but the link left in Hibern8 (its last UIC command 0x17), so one
     DME_HIBERNATE_EXIT, as the bootloader sends before it reads, which is what the 8a's device tree describes (`ufs@13200000`, "takes it over
     as it is and resets nothing");
   - **it is powered down:** U-Boot brings it up itself (the device's supply and pins, Samsung's PHY calibration, the
     link to HS gear 4), which is much more work and is what the 8a's environment describes (`ufs-exynos-gs.c`).
   **Found 2026-11-23, in four runs** (`hakios_zuma_ufs.c`): the list adopted (below 4 GB), Samsung's nexus bit set, but
   no request completes, because the link sleeps (Hibern8, the bootloader's last UIC command 0x17) and the standard
   DME_HIBERNATE_EXIT fails without the PHY's own wake-up writes (HCS power-mode status 5, PWR_FATAL_ERROR). Google's
   driver runs Samsung's CAL `pre_h8_exit` before the exit; **zuma's CAL is not public** (only gs101's and gs201's are),
   and zuma's PHY does not hold gs201's values (read: COMN 0x00/0x04/0x20 = 0x11/0/0, not 0x86/0x08/0x60), so gs201's
   are not written. This phone's Android has no kernel tracing, but zuma's CAL is compiled into its own UFS module
   (`vendor_kernel_boot`'s `ufs-exynos-gs.ko`, not stripped): `pre_h8_exit` read out of it (PMA common 0x0 = 0x11, 10 us,
   each lane 0x9f4 = 0, 0xa00 = 0x30, 0xb64 = 0x32 then 0x22), and U-Boot now writes those before the exit.
   **Run 5, the real cause:** with zuma's pre_h8_exit written (the PHY then exactly at its awake values) the exit still
   ends in PWR_FATAL_ERROR, and the probe's pins say why: the UFS device's supply (`ufs-vcc`, gpp0-1) is an output driven
   **low** and its reset (gph5-0) is held low -- **the bootloader powers the storage chip off** before it hands over (the
   8a's U-Boot notes say the same: "the device's supply and pins back on"). So the controller's "device present" is stale
   and nothing answers on the link. Adopting is not enough: U-Boot has to **bring the storage up itself** -- the supply
   and reset, Samsung's link set-up (CAL init_cfg and the HS gear 4 calibration, all in the same Android module), the
   UniPro link start-up, the device's initialisation (NOP, fDeviceInit), the switch to HS gear 4 -- as Google's
   ufs-exynos.c and Linux's ufshcd do at boot. Larger work, still read only towards the storage's contents.
   **Written 2026-11-23, not yet run:** `hakios_zuma_ufs.c` now brings it up from cold -- the PHY's isolation checked and
   the IO coherency set, the device's supply (gpp0-1) on and its pins (gph5-0/1) back on function 2, Samsung's software
   reset and host set-up, the device's reset pulse, zuma's CAL `pre_link`/`post_link` (Samsung's own code and tables from
   `google-modules/soc/gs` `drivers/ufs/zuma`, unchanged, in `zuma-cal/`), DME_LINKSTARTUP, NOP OUT, fDeviceInit -- in
   PWM-G1; `zuma ufs hs` then asks for HS-G4 rate B (the CAL's `pre_pmc`/`post_pmc` around the power mode change). The
   only request that writes anything to the device is SET FLAG fDeviceInit (its own initialisation, which every host
   does at every boot); SCSI stays on the read-only list.
   **Run 7, 2026-11-23: it reads.** The link up on both lanes, the device initialised in 2 ms, bRefClkFreq 38.4 MHz;
   LUN 0 block 0 read in PWM-G1 (10 ms) and, after the switch, in **HS-G4 rate B on 2 lanes** (3 ms), both crc32
   161dd6f8 -- the same 4 KB Android reads (sda-head.bin). `scsi scan`: the Samsung KLUDG4UHGC-B0E1, LUN 0 31,196,160
   blocks of 4 KB, LUNs 1-3 the boot LUNs (32, 32, 4 MB); `part list` reads LUN 0's GPT (persist ... userdata). (Run 6
   had failed only on reads with data: PRD entries larger than the controller's 4 KB.) The log stopped in the partition
   list, the watchdog's net most likely (the screen scrolling before the caches are on): the checks go to the log only
   next.
   **Run 8, 2026-11-23: checked against Android.** With the checks to the log only, all of LUN 0's table listed (34
   partitions, `userdata` sda34 the last, 0x2561e6..0x1dc03fa) and the reads in HS-G4 give Android's own hashes: the
   first 8 blocks crc32 **2fa99268**, the last block (31196159) **4bd66ab1** -- both as read with `dd` in Android. A
   4 MB read (1024 blocks from 196608, four 1 MB requests) also done (crc32 1147406a). Then hakios load, the loader and
   the kernel ran as before. **Phase 1, steps 1-3: done.** Left for the loader to use it: the storage handed on through
   EFI's block I/O (or the loader's own reads), for the kernel's `drivers/storage/hakios.ufs.c`.
   **Run 9, 2026-11-23: the loader and the kernel read it.** U-Boot now brings the storage up (HS-G4) and scans it at
   every boot (`hakios_ufs=1`); the loader, unchanged, finds it through UEFI: 4 raw units (LUN 0 and the three boot
   LUNs), every unit's GPT header and table CRCs right, 62 partitions. With `ufs=2 ufs_check=2` the kernel made
   `/dev/ufs_NAME` for all 62 and read through the loader's page (`ufs_hvc=1`): **402 requests, 420 MB, 0 errors**,
   590 ms in ReadBlocks (about 700 MB/s); abl_a and abl_b hash the same in the kernel as in the loader. (modem_a/b are
   200 MB here, smaller than the 214 MB the check's list asks for: "read failed (22)" at their end, as expected.) The
   loader's log write into `klog` is refused by U-Boot, as meant. **Phase 1: done.**
2. U-Boot's SCSI/block layer on top, so `part list` shows the GPT, and the loader sees the disks through EFI.
3. Read only, checked against Android: the same blocks read in U-Boot and in Android (`dd`), hashes compared.

Writes stay off in U-Boot for good: the only write HakiOS ever makes is the kernel's, into `darwin` alone
(`ufs_write=1`), past the volumes the computer wrote.

## Phase 2: the tools for the Pixel 8 (no risk to the phone)

**Written and tried on the computer, 2026-11-23** (`devices/shiba/tools/`, sharing the facts in `shiba_install.py`; the
kernel tree's own tools loaded and given the Pixel's facts, so their checks and refusals stay the tested ones):
`backup.py` (backup-partitions.py with the Pixel's names: 56 partitions with `--with-bootchain`, the four LUNs' tables;
reads only), `carve.py` (checks the table is the planned one, then userdata-carve.py `--wipe --entries 64`, then checks
the result again and writes `darwin.json`; the computer only), `darwin-write.py` (darwin's identity from `darwin.json`,
the LUN checked to be behind 13200000.ufs; every write refused until phase 4 places the Darwin volume).
`tests/install-tools-test.py GPT-SDA-HEAD.BIN` runs them with no phone: on LUN 0's first blocks read under Android, 32 GiB
gives userdata LBA 2449894..22807039 (77.66 GiB) and darwin sda35 LBA 22807040..31196154 (32.00 GiB), entries 34 and 35
the only ones changed, both copies' CRCs right; every refusal refuses. (The backup copy of the table there is made from
the primary; carve.py is to be run on the real one, which backup.py copies.)

1. `backup-partitions.py`: the Pixel 8 list above.
2. `userdata-carve.py --wipe`: already designed for "userdata is the last partition"; check it against your GPT
   (gpt-head.bin/gpt-tail.bin from phase 0) and run it **on the computer only**, to see the new table: `userdata`
   shortened, `darwin` (APFS type) at the end on a 2 MB boundary, nothing else changed, both copies of the table with
   their CRCs.
3. `darwin-write.py`: the Pixel 8's own constants (partition number 35, its exact LBAs from the table above) instead of
   the POCO's.

## Phase 3: the partition (the one destructive step, your decision)

1. **Size.** iOS needs about 5.6 GB (root volume) + 2.7 GB (cryptexes) + the data volume. The POCO used 20 GiB; the 8a
   77 GiB. **24-32 GiB** is a reasonable start. What is left for Android afterwards: about 85-93 GB, which matters if
   you mean to put your 81 GB back.
2. From Android (root): the two new GPT blocks written with `dd` (head and tail of LUN 0), read back, compared.
3. Reboot to fastboot, `fastboot -w` (Android makes `userdata` again at its new size), Android set up again.
   **This is where your data goes.**

## Phase 4: iOS on the partition

1. `darwin-write.py`: the root volume and the cryptexes into `darwin`, read back and compared; the data volume made.
2. Boot as now, `fastboot boot`, with `root_apfs=ufs_darwin var_hfs=ufs_darwin_data ufs=2` (the shiba configuration's
   defaults already say so), and the small image and boot volume in the payload, as the 8a's `bootrun.sh` does.
3. Whether to flash HakiOS to slot b (as the 8a) is a later, separate decision: it means writing `boot_b`, and U-Boot
   marking the slot good in `devinfo`.

## Risks, and what limits each

| Risk | Limited by |
|---|---|
| Losing your data | Phase 0.1; it is lost by design in phase 3 |
| A broken partition table | Backups of LUN 0's first and last blocks; the carve checked on the computer first; only two GPT entries change; phase 0.3 checked before phase 3 |
| Writing the wrong place | `darwin-write.py`'s refusals: the partition's number, name, GUIDs, place and size all checked at write time; U-Boot never writes |
| The phone not booting Android | Android's slots, bootloader and system partitions are never written; `fastboot -w` is Google's own step |
| Identity (IMEI, calibration) | Those partitions are never written, and are backed up in phase 0.2 |

## Order of work, and where you decide

1. Phase 1 (UFS read-only in U-Boot): code and test boots from RAM, as so far. **Safe.**
2. Phase 2 (tools): on the computer. **Safe.**
3. Phase 0 (backups): yours and the tools'. **Read only.**
4. **Decision:** phase 3 (the partition, Android's data erased).
5. Phase 4 (iOS written, first boot of iOS).
