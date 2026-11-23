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

**Phase 0.2 done, 2026-11-23:** `backup.py --with-bootchain --yes`: 56 partitions and the four LUNs' tables (8 copies),
1019 MB, every one proven by its three hashes at the first try, all matching manifest.json again afterwards; kept in
`devices/shiba/backups/20261123/` (ignored by git). `carve.py` on the real tables, 32 GiB: the table is the planned one;
the new one (64 entries; userdata LBA 2449894..22807039, 77.66 GiB; darwin sda35 LBA 22807040..31196154, 32.00 GiB) checks
out both ways, entries 34-35 the only ones changed; `carve/darwin.json` written. Nothing written to the phone.
**Phase 0.1:** your own data backed up (you, 2026-11-23).
**Phase 0.3, the way back, checked on the computer, 2026-11-23:** Google's factory image for this phone's build,
`shiba-cp2a.260805.005-factory-26ca3017.zip` (its SHA-256 begins 26ca3017, as its name says; bootloader
ripcurrent-17.0-15199481, the phone's own). `flash-all.sh` needs fastboot alone: `flash bootloader`, `flash radio`,
`-w update image-...zip`. The bootloader image (FBPK v2, zuma) carries `partition:0`..`partition:3`, the four LUNs'
layouts (Google's FBPT: size, name, type and unique GUID of each entry) from which the bootloader writes their GPTs, and
they are this phone's tables: LUN 0's 34 entries the same in name, type GUID, unique GUID and size, userdata flagged to
fill the unit; LUNs 1-3 the same once sizes are rounded up to whole 4 KB blocks. So `flash-all` puts LUN 0's original
table back (darwin gone, userdata whole again) with Android. Read from the image, not tried on the phone. A second way
back: the proven copies of LUN 0's first and last 8 blocks (backups/20261123), written back with dd from a root shell.

## Phase 3: the partition (the one destructive step, your decision)

1. **Size.** iOS needs about 5.6 GB (root volume) + 2.7 GB (cryptexes) + the data volume. The POCO used 20 GiB; the 8a
   77 GiB. **24-32 GiB** is a reasonable start. What is left for Android afterwards: about 85-93 GB, which matters if
   you mean to put your 81 GB back.
2. From Android (root): the two new GPT blocks written with `dd` (head and tail of LUN 0), read back, compared.
3. Reboot to fastboot, `fastboot -w` (Android makes `userdata` again at its new size), Android set up again.
   **This is where your data goes.**

**Phase 3 done, 2026-11-23 (your decision):** the phone's LUN 0 table read again first: byte for byte the backed-up one at
both ends; the new table's two files pushed and their hashes checked on the phone. Three writes from Android's root shell
(`su -c sh`, the script on standard input -- `su -c '...'` split by adb ran only `cd` as root, copied nothing, and the
table was checked unchanged before going on), each read back from the flash with the page cache dropped: the primary
table (LBA 2..3), its header (LBA 1), the backup (LBA 31196156..31196159); then both copies read fresh and checked on the
computer (CRCs right, 64 entries, the same). The bootloader agrees (`getvar partition-size`: userdata 0x136a01a000,
darwin 0x8001fb000). `fastboot -w` erased userdata and metadata (formatted by Android at its first boot), and the phone
restarted. `darwin.json` copied to devices/shiba/boot-info/ (ignored by git) for darwin-write.py.

**After phase 3, from the new Android (the ColorOS port, set up again), 2026-11-23:** sysfs, without root, says what was
carved: sda34 userdata from LBA 2449894, 20,357,146 blocks; sda35 `darwin` from LBA 22807040, 8,389,115 blocks;
/dev/block/by-name/darwin -> sda35; /data made at the new size (78 GB). With root back (KernelSU): `darwin-write.py check`
says darwin is what was carved (sda35 of LUN 0 behind 13200000.ufs, its LBAs, type and GUID), and `selftest` on the phone
refuses every changed GPT and every range outside darwin. (Windows' adb.exe ends lines with CR: the wrapper strips it.)

## Phase 4: iOS on the partition

**Prepared on the computer, 2026-11-23:** Apple's images out of the IPSW again (iPhone14,4 16.6.1): the root filesystem
is a UDIF image (LZFSE), its Apple_APFS partition taken out whole by 7-Zip (checksums checked) -- 1,497,600 blocks; the
OS cryptex (708,608) and the App cryptex (2,560) are raw APFS as they come. Back to back, as the kernel finds containers
(each where the one before it ends: `,c=1`, `,c=2`): `build/shiba/darwin/darwin-system.img`, blocks 0..2208767 of darwin
(the loader's own note for this IPSW says the same end), SHA-256 3b6721d5...; the data area from 4194304 untouched.
The RAM part: `NO_SYSTEM=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh` (NO_SYSTEM: a local switch, so the 2.5 GB
RAM system image that stalled is not made; theos clang via XNU_CLANG_DIR): flashboot.apfs 4 MB, root.apfs 31 MB;
`FLASH_ROOT=1 FIRST_ARGS="cpus=9 ufs=2" OUT=hakios-shiba-flash.img mk-first-boot.sh`: a 52.9 MB payload. Without
ufs_write=1 nothing is written at boot; /private/var is a memory filesystem (init's fallback when var_hfs does not mount).
The write itself: `fastboot flash darwin` of sparse pieces (tools/sparse-split.py), the bootloader bounding it to darwin;
read back from Android's root shell and compared with the image's SHA-256.
**Written, 2026-11-23 (your go-ahead):** fastboot (bootloader, max-download-size 0xf900000): 35 sparse pieces,
`fastboot flash darwin` each, all OKAY. Back in Android, as root: darwin's first 2,208,768 blocks hash
3b6721d5... -- the image's. `protected_blocks` set to 2208768 in darwin.json: darwin-write.py never writes them again.
**First boot from darwin, 2026-11-23:** U-Boot, the loader (darwin listed, the 64-entry table's CRCs right) and the
kernel (9 cores, the boot volume mounted as md0) ran; then `/sbin/launchd` (HakiOS's stage0) refused, errno 86 EBADARCH:
our static raw-syscall programs (init, child, stage0, var_store) were built arm64e with ptrauth ABI version 0, which the
kernel refuses as process 1 (as hello was). A local `STATIC_ARCH=arm64` in the kernel tree's userland/build.sh builds
those four plain arm64; Apple's programs stay arm64e. Rebuilt with it.
**Second boot from darwin:** HakiOS's init mounted Apple's root volume from darwin and switched to it ("/flash is the
root now"), both cryptexes mounted from behind it (SydneyG20G81.D16SystemCryptex, container 1; AppCryptex, container 2),
and Apple's launchd ran as process 1 -- then exited: "failed to query hardware target: 22". The loader's identity.h was
the placeholder (no IPSW when it was first made); `xnuloader/mkidentity.sh` made it from the IPSW's d16ap device tree
(model, compatible, target-type; kept out of git), and mk-first-boot.sh now refuses FLASH_ROOT=1 with the placeholder.
**Third boot from darwin:** the panic now says Device D16, Hardware Model iPhone14,4; launchd got past the hardware
query, then dyld: "Library not loaded: /usr/lib/libSystem.B.dylib ... (no such file, no dyld cache)" -- no shared cache
mapped, and no `shared_region: ... map` refusal in the log (the kernel's APFS reads LZBITMAP, so not the compression).
Why is earlier in the log than the loader's 16 KB excerpt shows; a local loader option `klog_echo=all` prints all of it.
**Fourth boot (klog_echo=all), 2026-11-23:** init's checks: 45 of 46 pass -- Darwin volume mounted from darwin and made
the root, both cryptexes on /private/preboot/Cryptexes/OS and App, /private/var in memory. The one failure: "run a
program through dyld and the shared cache" (hello_dyn: execve "Unknown error"). launchd's dyld asks twice for a shared
region (check_np) and never asks to map one: its own checks of the arm64e cache fail first. Lead, not yet followed: the
kernel's user pointer authentication (osfmk/arm64/hakios_pac.c, boot-arg hakios_pac, off by default) -- the arm64e
userland and its shared cache are PAC-signed; how the 8a ran 16.6.1 d16ap with it is to be found next.
**Fifth boot (a local kernel trace, `hakios_trace1=1`: process 1's failing open/stat/fcntl with their paths):** dyld
looks for the cache in `/System/Library/Caches/com.apple.dyld/` on the root volume -- ENOENT, Apple's 16.6.1 root has no
such folder (the cache lives only in the OS cryptex) -- and gives up. On iOS 16 Apple's dyld runs libignition for process
1, which points dyld at `/private/preboot/Cryptexes/OS/`; here it did not. libignition's boot-args (read out of
/usr/lib/dyld): ignition_level, ignition_force_dylib_root=cryptex|livefs|rootfs, ignition_live_app_graft,
ignition_halt_after. Next: `ignition_force_dylib_root=cryptex` (the cryptex is already mounted there). PAC is not it.
**Sixth boot:** libignition now printed its state: boot spec **ramdisk** (it reads `rd=md0` from the boot-args), two
stages, hello and goodbye -- the cryptex stages and the fallback are skipped. `rd=md0` is only how the kernel starts (the
boot volume first, init then switches to the flash). A local kernel change: after kern.hakios_switch_root succeeds, the
kernel's boot-args (what kern.bootargs returns) have `rd=` renamed `r_=` (hakios_keep_rd=1 leaves it).
**Seventh boot:** with `rd=` hidden, libignition takes the `local` boot spec (7 stages, ignition level 5, force dylib root
on) and stops in its second, `preboot`: "failed to lookup preboot: 25" -- `fsctl("/", 0xC1044A50)` (APFS group 'J' 0x50,
260 bytes: version 2, the role at bytes 2-3, 0x10 Preboot; the device node back from byte 4), which the kernel's APFS
answered ENOTTY like every fsctl. A local kernel change answers that one request for role 0x10 with the device mounted on
/private/preboot (this port's boot volume, which init moves there).
**Eighth boot:** the role lookup answered ("preboot device: root_device", the boot volume's name as the first root);
then "failed to get preboot mount point: 2": libignition reads the sysctl `vfs.generic.apfs.edt_fstab` (the APFS kext's
volumes by role, 236-byte entries, the mount point at +32 and the role at +232). The kernel now has it, with one entry
(role 0x10 on /private/preboot), there only after the root switch.
**Ninth boot:** the `preboot` stage passes ("preboot already mounted: device = root_device, mount point =
/private/preboot"); `cryptex1 sniff` fails (8): it only checks that the Preboot volume has a `cryptex1` directory. Read
ahead in dyld: the `local` plan's 7 stages are hello, preboot, cryptex1 sniff, graft fetch (tolerates a missing image:
"continuing"), graft select (with ignition_force_dylib_root=cryptex: "overriding os cryptex root", needing the graft point
we mount), dylib cache, goodbye. The boot volume now has `cryptex1` (a local line in the kernel tree's build.sh).
**Tenth boot:** past `cryptex1 sniff` -- and `graft` IS in the plan (the read-ahead was wrong): "os: opening preboot
subdirectory: cryptex1/current ... failed to open object directory: 2". Its objects (from dyld's strings): os.dmg,
os.dmg.root_hash, os.dmg.trustcache, an apticket...im4m, and app.* the same; then graftdmg, whose failure is ignored when
the graft point already has content ("cryptex content already available"), as ours does. Next: an empty
cryptex1/current, so that libignition lists which objects it requires.
**Eleventh boot:** "object absence permitted: apticket.d16ap.im4m"; "object not present: os.dmg ... failed to open object:
os hyper dmg: 2" -- os.dmg is required. Empty stand-ins now (os.dmg, os.dmg.root_hash, app.dmg, app.dmg.root_hash; no
trust caches): the graft itself fails (graftdmg asks the dmg's filesystem, this kernel's APFS: ENOTTY) and Apple's code
ignores that when the graft point already has content, as ours does.
**Twelfth boot:** os.dmg and os.dmg.root_hash open; os.dmg.trustcache is required. The IPSW's own seals and trust caches
of both cryptexes (Firmware/096-57245-083.dmg.* the OS, 096-57613-084.dmg.* the App) now go into the boot volume's
cryptex1/current (from sysimage/cryptex1, out of git).
**Thirteenth boot:** every object opened, the manifest spec selected, then launchd halted: "failed to realpath chosen
im4m: -1: 9" -- the apticket's absence is "permitted" but a later step resolves its path. An empty apticket.d16ap.im4m
stand-in now (a device's signed manifest is not in the IPSW; the graft that would use it fails and is ignored).
**Fourteenth boot:** the graft ran: "grafting: method = syscall, dmg = .../os.dmg ... graft point =
/private/preboot/Cryptexes/OS" -- graftdmg: 18 (EXDEV), not tolerated. Apple's code carries on from one graft error alone,
37 (EALREADY: "cryptex content already available"), then opens the covered graft point. A local kernel change: graftdmg
answers EALREADY when a volume is already mounted on the graft point (init mounts each cryptex there).
**Fifteenth boot:** graftdmg 37 now, but still "graft failed": the os cryptex's failure handler
(_cryptex_graft_failure_exists) accepts 37 only when libignition's "userspace reboot" flag is set (configuration +7034),
which comes from `XPC_USERSPACE_REBOOTED` in launchd's environment (_configuration_init). HakiOS's init (a local change)
now execs Apple's launchd with XPC_USERSPACE_REBOOTED=1 on a flash boot -- the cryptexes are already in place, as after a
user-space reboot. What launchd itself does with the variable is to be seen.
**Sixteenth boot: Apple's launchd runs.** libignition completes: both cryptexes "content already available ... ignored
error = 37", trust caches skipped, "overriding os cryptex root: boot-arg forced cryptex fallback", "opened shared cache
directory: /private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld", "ignition sequence complete"; then
`com.apple.xpc.launchd ... hello`, "Darwin Bootstrapper Version 7.0.0", its boot-args. About a second later (launchd
reading FeatureFlags plists) the kernel entered the debugger with no panic text in the returned log. A local kernel change
writes the reason of every debugger entry (op, message, panic string, caller) into that log; the trace is off again.
**Seventeenth boot:** "debugger entered: op 3, message panic, panic string `userspace panic: %s`" -- launchd itself asked
for the panic (reboot with RB_PANIC|RB_QUICK and its own message). The kernel now writes that message into the log too.
**Eighteenth boot:** launchd's message: "Userspace reboot changed system version: previous  != current 20G81". Read in
launchd (REA could not analyse it here: no Mach-O backend on this Windows machine; llvm-objdump instead): on a normal
start launchd sets kern.osproductversion and kern.osversion from SystemVersion.plist; on a user-space reboot it reads
kern.osversion -- set by the launchd before -- and compares. HakiOS's init (local) now sets both from the Darwin
volume's SystemVersion.plist before it execs launchd, as that launchd would have.
**Nineteenth boot:** no kernel log came back at all -- the record ends at the loader's "jumping to", then the next
start: the kernel never returned to the loader (a hang a watchdog caught, or a restart asked for), so how far launchd got
is unknown. A local kernel change: boot-arg `bldrlog=0xfd800000` copies the kernel's console, as it goes, into the
bootloader's log buffer (the one U-Boot appends to, which survives a reset and Android hands back); on from now.
**Twentieth boot (the console copy works):** launchd gets past its version check and runs its boot tasks, skipping
most (a user-space reboot), then "keybag": MKB_INIT "Could not open /private/var/keybags/systembag.kb ... Can't load the
keybag. tears in rain... Time to die ... Rebooting" (the run about 20 s shorter, as seen on the screen). On a
user-space reboot launchd expects the keybag the first boot made. So that route is dropped (kept behind boot-arg
init_userspace_reboot): the kernel's graftdmg now takes an already-mounted cryptex as grafted (success, not EALREADY),
the stand-in AMFI accepts the trust cache libignition then loads (__mac_syscall AMFI 101), the apticket stand-in is
not empty (it is mapped), and launchd starts as on a normal first boot.
**Twenty-first boot: a normal first boot.** Both cryptexes "loaded trust cache"; launchd ran every boot task --
fsck, mount-phase-1/2 ("data volume missing, but not required"), data-protection ("No SEP present"), finish-obliteration
("No obliteration needed"), detect-installed-roots, MSUEarlyBootTask, fips ("all tests PASSED") -- then keybag:
MKB_INIT in "No Crypto" mode only loads /private/var/keybags/systembag.kb (a restore makes it) and reboots without it.
(The 8a never met this: its rd=md0 stayed visible, so launchd ran no boot tasks.) A systembag.kb stand-in now goes
into /private/var with the var skeleton; the kernel's software keybag answers a load with a handle whatever it holds.
**Twenty-second boot:** the file found, but "Could not create dictionary from stream": MKB reads systembag.kb as a
property list. From MobileKeyBag's strings (dyld_shared_cache_arm64e.15): KeyBagKeys (data, must not be empty),
KeyBagVersion, OpaqueStuff. The stand-in is now such a plist.
**Twenty-third boot (the run visibly longer):** "DONE MKB_INIT"; usermanagerd created the mobile user (uid 501) and
loaded the persona; boot tasks dirs_cleaner, xpcroleaccountd, init_featureflags, auearlyboot, tzinit, finish-restore,
sysstatuscheck, prng_seedctl ran; then launchd_cache_loader aborted ("Failed to create SecStaticCodeRef
errSecCSStaticCodeNotFound": HakiOS hides Apple's xpc/launchd.plist so that launchd runs its curated LaunchDaemons) and
launchd panicked. The loader has its own switch, boot-arg launchd_unsecure_cache; the build now writes a
launchd.plist of HakiOS's own (Apple's, its LaunchDaemons replaced by HakiOS's 421) into the xpc folder init binds.
**Twenty-fourth boot:** "Using default cache paths", then "Failed to verify codedirectory errSecCSUnsigned": the
cache was found, but the switch was not taken. Disassembled (llvm-objdump), the loader reads launchd_unsecure_cache=
with strtoul: it is a number, not a path, and a path reads as 0. It is now launchd_unsecure_cache=1, which loads the
default path, HakiOS's cache, without the signature.
**Twenty-fifth boot:** "Using unsecure cache", "Cache sent to launchd successfully", "Early boot complete. Continuing
system boot." Then launchd's own check: "unexpected exec of non-platform binary: /redwood/sbin/init: com.hakios.demo"
(a job from the cache must exec a platform binary: csops CS_OPS_STATUS, bit CS_PLATFORM_BINARY). Our programs are
ad hoc signed by ldid -S, which leaves the code directory's flags 0, and the kernel's AMFI stand-in marked only
CS_ADHOC ones as platform. It now takes a signature with no CMS signer as ad hoc (kern_minimac.c). The 8a never met
this: with Apple's cache hidden its jobs were not cache jobs. (Also seen, not fatal: com.hakios.thermalstate's
executable missing.)
**Twenty-sixth boot: iOS booted** from darwin (your report, from the screen). No log came back: the run did not end in
a panic, which restarts the phone with the log buffer kept; it was ended from the buttons, a hard reset after which the
bootloader reports no console-ramoops. Next: end a run with a software restart (iOS's own Restart) to keep the log.
**Twenty-seventh boot:** iOS's Restart (AssistiveTouch): no log either, the reset a watchdog one ("APC Watchdog
Early", SYSTEM_SWRESET). The panic runs, whose logs came back, ended through the loader and then a button reset
(PIN_RESET). With loader_return_after=0 (the timer off, only present) AKITA_POWER leaves Restart to that same way back
to the loader: the screen stalls, the power button is held, and the log should be kept.
**Twenty-eighth boot:** Restart went to the loader (the shutdown spinner stayed), but the log was lost again: the
loader arms a 30 s watchdog after the kernel returns (WDT_BITE_MS), and it bit first (CLUSTER0_NONCPU_WDTRESET).
Every run so far: a button reset (PIN_RESET) kept the HakiOS log; a watchdog or software reset (SYSTEM_SWRESET)
did not. So: once the screen stops, hold the power button within about 20 s.
**Twenty-ninth boot:** held at once, still CLUSTER0_NONCPU_WDTRESET: the watchdog bit during the shutdown itself.
The kernel feeds it from a thread call (loader_return.c), which stops as it halts; the loader feeds it while it
waits only for loader_return_after + 60 s. Now loader_return_after=86400: the loader feeds it the whole run (a day).
**Thirtieth boot:** no console-ramoops at all. Restarts are given up as the way to the log: it is now read live over
USB (usbmux=1, the kernel's port 1) by tools/usbmux-log.py, which now also reaches Windows' Apple Mobile Device
Service (127.0.0.1:27015) when run with Windows' Python: no usbipd, no usbmuxd in WSL.
**Thirty-first boot:** Windows saw no device: HakiOS's USB never started on the Pixel 8 ("akita usb: the soft reset
did not finish"): the 8a's U-Boot made the eUSB2 link (`eusb init`) before the kernel, shiba's does not yet. The
driver now makes it itself (eusb_reinit, its replug path) when the first start fails, and starts again; the soft
reset is given up to a second.
**Thirty-second boot: the log live over USB** (usbmux-log.py through Windows' Apple Mobile Device Service, 808 KB):
USB works. SpringBoard up (lock screen, status bar, battery UI), backboardd, wifid, chronod, usermanagerd. Found:
mediaserverd restarts every few seconds (15 pids in 3 minutes), after Metal's failed assertion "computeFunction must
not be nil" (metal=1: it is not shown the GPU); seven of our jobs (ptp, wifi, lsreg, klogsave, locfeed, memsay,
thermalstate) "Missing executable" in /redwood/bin; the stream ended at 16:38:10 with no panic in it while the phone
went on.
The "Missing executable" jobs are our dynamic arm64e programs: theos's clang 11 writes unversioned arm64e (CPU
subtype 2, no ptrauth-ABI bit), which the kernel grades 0 and does not exec (grade_arm64e_binary); var_store, plain
arm64, ran. userland/build.sh now marks them ptrauth ABI version 0 (PAC is off in this kernel: the ABIs are one
here) and signs them again; build with LDID=~/theos/toolchain/linux/iphone/bin/ldid, or the programs that need
entitlements (lsreg, locfeed, ...) are signed without them.
**Thirty-third boot:** no "Missing executable": thermal_state, memsay, klogsave (the kernel's log into
/private/var/mobile/Media/hakios-klog, in memory), ptp_responder, lsreg (MetalTest.app registered), locfeed and the
wifi job all ran. USB came up as Code 10 in Windows at first; a replug (the driver's own re-init) fixed it.
mediaserverd still restarts (the Metal assertion).
mediaserverd: shown no GPU device (metal=1), it still describes compute pipelines; the function is nil and Metal's
-[MTLComputePipelineDescriptorInternal setComputeFunction:] asserts. inject_media.c (libhakios.media, inserted in
mediaserverd) now leaves a nil function out and says who described it (image:symbol, the first three times).
**Thirty-fourth boot:** the guard in ("takes no nil"); no assertion, one mediaserverd for the whole run (it had
been 15 in 3 minutes). It never said "left out": with the method replaced no nil reached it, so who described the
pipeline is still not known.
Sound works (your test, from the phone's speaker).
**Wi-Fi, the 4398's port begun:** the image ran pcie=5 without wifi=, so the chip was never started; the driver's
steps are the 8a's BCM4383's (refused on the 4398 without wifi_4398=1). The first difference: the 4398's chip id says
interconnect type 6 (NCI), whose enumeration ROM the AXI scan cannot read. Step 1 now shows that ROM raw (reads only,
256 words at most) and stops; the next boot has wifi=1 wifi_4398=1. The phone's own firmware is in fw-wifi/ (b0, c0,
d0; out of git).
**Wi-Fi 1:** the 4398's EROM read raw (0x330 bytes at 0x18007118). Its format from Google's 4398 driver
(nciutils.c, read for the facts): interfaces of two words (node, flags with the header's offset) and a slave's address
words; the core headers in the OOBR (0x18006000) and EROM part 1 (0x18007000). Step 1 now walks it (nci_scan) and lists
the cores, ids from their headers; reads only, then stops. usbmux-log.py now reconnects by itself (the phone leaves
the bus at the Apple logo) and only appends.
**Wi-Fi 2:** the 4398's cores (37): ARM CA7 rev 13 (0x18080000, node 0x18504000), sysmem rev 14 (0x18088000; ROM
0x0-0x29ffff, RAM from 0x2a0000), PCIe2 rev 130, three 802.11 rev 88, chipcommon 76, PMU 46, GCI 28. From Google's
4398 driver (a local clone, read for the facts): no firmware signature in its build; firmware d0 for chip rev 3; the
CA7 halted by reset with SICF_CPUHALT and let go by clearing that bit; the vars and random seed below the top of RAM;
the reset vector at address 0. Step 2 now: completion timeout 0x8a061a80, the ARM's domain, the sysmem up and sized
from its banks, the CA7 halted (NCI's way: DMP control at the OOBR header + 8, IDM reset at node + 0x140); then stops.
**Wi-Fi 3:** step 2 ran as Google's driver expects: the CA7 as found up and halted (DMP control 0x21), the sysmem up,
coreinfo 0x0b201000: 11 ROM banks, 32 RAM banks, 4 MB of RAM at 0x2a0000 (to 0x69ffff, the sysmem's slave range);
the ARM's domain on, completion timeout 0x8a061a80. Next, the firmware (d0). The calibration is named by the module's
vendor id in the chip's OTP (bcmdhd.cal_USI_..., then _4398_d0, then bcmdhd.cal): this phone's is not in fw-wifi/ yet.
Android's driver says (info_string): firmware 24.100.671.86.1 d0, Chip 4398 Rev 3, VID 9957: the calibration is
bcmdhd.cal_USI_G5SN_9957_V17 (bcm94398d0GGmodUSIG5SN), now in fw-wifi/; the d0 firmware there is the phone's (the same
SHA-256). userland/build.sh packs the 4398's files (2.2 MB); steps 3 and 4 are shared with the 4383's: the firmware at
0x2a0000 read back, the calibration below the top with its token, the random seed, the reset vector at 0; the CA7 let
go by clearing its halt bit; the shared area's address awaited in the top word. Next boot: wifi=4.
**Wi-Fi 4: the BCM4398's firmware runs.** 2118969 bytes at 0x2a0000, read back with 0 words different; the reset
vector written at 0 (no secure-boot lock); the CA7 let go; pcie_shared's address 0x453b94 in the top word after 44 ms,
flags 0x109 (protocol 9, asserts built, no trap). Its fields were still 0: from PCIe core rev 68 the firmware says it
is up with a boot interrupt, which Google's driver waits for (5 s) before reading pcie_shared; step 4 now polls the
mailbox interrupt status and the ring information's address for it.
**Wi-Fi 5 (quiet image, no -v):** the boot interrupt came after 423 ms (mailbox interrupt status 0x1), but
pcie_shared at the first address then read all zero, its flags too. Google's driver reads the top word again after
the boot interrupt; step 4 now does, and shows pcie_shared's first 16 words.
**Wi-Fi 6:** after the boot interrupt the top word is 0x613800: pcie_shared flags 0x70250109 (protocol 9, DMA indices),
mailboxes 0x613e00/0x613e04, ring information 0x61388c, the firmware's console at 0x52687c; FWID c9e90b08, Android's.
The ring information's layout (Google's ring_info_t) is the one step 5 reads. Next boot: wifi=6 (rings, "ver", the
4398's CLM and txcap), with torch=3 and bt=2 to try (BCM.hcd now packed) and display_hz=120 (the panel ran at 60 Hz:
the driver keeps the bootloader's rate unless asked). Metal: this tree has no hakios-metal Mali build (out/ and
xnu-deps/build/mesa26 absent), so the image carries the plug-in without a GPU behind it; MetalTest sees no device.
**The USB log's breaks:** every 30-90 s Windows' Apple Mobile Device Service reset the phone (bus reset, enumerated
again), each time after its connections to lockdownd (port 62078) were refused -- 52 of 52 in one run: the job list had
no lockdownd. userland/build.sh puts it in only with LOCKDOWND=1, and iOS's bluetoothd only with BLUETOOTH=1; the 8a's
tools/bootrun.sh builds with both. The user space is now built as
  LOCKDOWND=1 BLUETOOTH=1 LDID=~/theos/toolchain/linux/iphone/bin/ldid XNU_CLANG_DIR=~/theos/toolchain/linux/iphone/bin
  STATIC_ARCH=arm64 NO_SYSTEM=1 CRYPTEXES=flash FLASHBOOT=1 REST=1 userland/build.sh
(429 jobs: lockdownd, bluetoothd, bluetoothuserd, the BTServer profiles).
**Run 7:** lockdownd answered (36 of 36 connections); two breaks only (the Apple logo, one later). 120 Hz
(display_hz=120): 120 frames a second while the screen is simple, 10 when widget extensions start (no GPU Metal: all
drawn by the CPU). The flashlight: the LM3644 answers, /dev/hakios-torch. Bluetooth (bt=2): BCM.hcd loaded, the
controller BCM4398D0 G5SN_V17_SB3, opened by bluetoothd. Wi-Fi steps 5 and 6: the rings up, "ver" 24.100.671.86.1 d0,
the MAC read, the 4398's CLM and txcap loaded (status 0). Next: wifi=8 (radio up and a scan, en0, the station).
Tested on that same boot (the wifi=8 image not yet booted): Bluetooth and the flashlight work in iOS; Wi-Fi does
not (the image stopped at step 6 by design); swiping choppy, 2-50 frames a second while touched: no GPU Metal. Metal is
the 8a's Mali build: tools/mesa/pan/mk-pan-host.sh and mk-pan-ios.sh (Mesa 26.2.6's panfrost compiler), then
make -C hakios-metal ios-mali ARCH=arm64e, then userland/build.sh with HAKIOS_METAL_OUT=ios-mali (as bootrun.sh);
this machine lacks meson, ninja, python3-mako and libdrm-dev.
**Run 8 (wifi=8):** en0 and the station up, wifid asking it (Settings turned the radio off and on), but no network
heard: "event_msgs" -23 (unsupported: the 4398 has only "event_msgs_ext", eventmsgs_ext_t: version 1, command 3,
27 bytes of mask), and "escan" -37 (a wrong version: "scan_ver" is a wl_scan_version_t, its major version the
third 16-bit word -- read as one word it was 0x60001). Now: event_msgs_ext when event_msgs is refused; the scan
parameters of the version the firmware gives (2/3: the v2 layout; 4: scan_type_ext after scan_type), for step 7's scan
and the station's alike. The Metal build: Ubuntu's meson is 1.3.2, Mesa 26 wants 1.4 or later.
**Run 9: Wi-Fi joins.** "event_msgs_ext" 0, scan parameters v3, the boot's scan: 14 access points in 4.5 s;
Settings' scans answered; a WPA2-PSK network joined from Settings (link up, keys set by the firmware's supplicant,
5500 MHz, -56 dBm). 5 s later the firmware halted: dozens of "PHYTX error" (TDC_Frmlen and Psdulen disagree),
"TXE shared error", a PSM watchdog (the MAC's microcode), tx FIFO 7 flush timeouts, a trap -- once data was sent.
The frames with odd ethertypes were flagged 0x2, BCMPCIE_PKT_FLAGS_FRAME_802_11: 802.11 management frames, now
kept from en0. The next boot runs without Bluetooth (bt=0), to see whether the shared radio's other half is in the
way of Wi-Fi's transmits.
**Run 10 (bt=0):** the same: joined, then PHYTX errors and the halt 5 s later (1003 frames sent meanwhile, 0 data
received). Bluetooth is not it. Ruled out too: the flow ring's create request and the tx post's layout (Google's
structures, field for field), the DMA memory (write-combined, uncached), Google's preinit iovars (nothing of the
radio's). Open.
**Metal:** built here now -- mk-pan-host.sh, mk-pan-ios.sh (meson 1.12 from pipx; HAKIOS_METAL=the tree's
hakios-metal), make -C hakios-metal ios-mali ARCH=arm64e; the user space with BIG_IN_ROOT=1 HAKIOS_METAL_OUT=ios-mali:
HakiosMetal 11.6 MB with the Mali back end. metal=2 shows it to every process but backboardd: applications draw on
the GPU; iOS's own compositing (backboardd) does not -- that is METAL_PLUGIN=ca, which also needs a CoreAnimation
default.metallib this 16.6.1 image lacks (tools/ca-metallib.py).
**Run 11 (metal=2): stuck at the Apple logo** -- SpringBoard and mediaserverd aborted over and over (432 and 434
times), each just after the Mali made a context for it. Back to metal=1, with userland/build.sh's new METAL_CLIENTS
(HakiosMetal.clients beside the bundle): the device shown to MetalTest and metal_test only.
**Run 12 (hot, per the tester):** SoC 60-70 C, skin 44.6-45.1 C (severity 3: the CPU budgets cut), the battery 22 C,
its charge current held down by the heat; nothing crash-looping. The flashlight's going off was its own 30 s limit
(torch_max_s). From 20:48 Windows reset the bus every 5-30 s with the phone's side configured and no stall before it.
MetalTest's results are on the console (usbmux port 1), not in the kernel's log.
**Console 13: Metal on the Mali.** MetalTest found "HakiOS Mali-G715 (Metal: buffers, blits and compute so far)" and
draws: 19 of its 25 scenes set up and run at 40-100 frames a second (triangles, indexed draws, textures, blending, depth,
stencil, 16384 instances, 4x MSAA, RGBA16F, three targets, ASTC, formats, lines, scissor, discard, blits, 240000
triangles, shadow map, UIKit over Metal). Six do not, each a piece hakios-metal's plug-in lacks: generateMipmapsForTexture
(5), a compute function from the application's library (13), array, cube and 3D textures (15), the blit's
copyFromTexture:...toTexture: with slices and levels (20 drew, its read-back raised), layered rendering (22, 24).
Seen on the phone: every scene but 25 (UIKit over Metal) BLACK. The plain plug-in (hakios-metal/plugin) has no
-newTextureWithDescriptor:iosurface:plane: -- a CAMetalLayer's drawables are not textures over the surfaces the window
server shows. The 8a's GPU=1 (devices/shiba/tools/bootrun.sh) is METAL_PLUGIN=ca (userland/src/HakiosMetalCA.m: the
IOSurface drawables, the surfaces' seeds, CoreAnimation's window server on the GPU) with ca_metallib= and the iPhone
11's CoreAnimation library: QuartzCore's default.metallib, which this 16.6.1 image lacks (only default.pipelinelib), from
the iPhone 11's 16.5 (20F66) through tools/ca-metallib.py (CA_QC_METALLIB=) into ~/ca-share; without it every client
but none aborts. METAL_PLUGIN=ca builds here against the Mali library (11.9 MB).
The 8a's repositories, 2026-11-23: Martinvlba/hakios-xnu-kernel's akita-port has 6 commits past this tree, all iOS 27
(xnu-12377, iOS 27.2.1's stock jobs, Apple's APFS kext, /redwood renamed /akita, the tree renamed hakios-base);
Martinvlba/hakios-devices-akita's drivers match shiba's but for that rename and shiba's own changes, its one new piece
a USB CDC-ACM serial console beside usbmux (hakios.akita_serial.c). Neither carries the CoreAnimation library: the
8a's bootrun.sh still takes it from ~/ca-share, the iPhone 11's.
**Wi-Fi, the stall after a join:** en0 is 1500 bytes of MTU, no offloads (hakios.net.c), so the PHYTX errors' frames
of 1900-2200 bytes are the firmware's own aggregates, and its dump was the aggregation queue manager's (AQM). Next boot:
boot-arg wifi_noagg=1 ("amsdu" 0, "ampdu" 0 before WLC_UP), and en0's sends counted by ethertype with the longest.
**Run 14 (wifi_noagg=1):** no PHYTX error at all, but a halt 2 s after the join -- now a clean trap: TRAP 4 (data
abort), dfar 0, pc 0x3bd468 lr 0x3bd79b. Through fw_bcmdhd.map_4398_d0: txq_hw_fill <- wlc_txq_fill <- wlc_sendpkt_ext
<- wl_send <- pciedev_handle_h2d_msg_txsubmit: a NULL read while queueing a frame from our flow ring, right after the
firmware's supplicant keyed the link. 363 frames posted in those 2 s. The tx post's fields are Google's
(dhd_prot_txdata: 802.3, prio << 5, one segment, data after the Ethernet header, phase 0); its station-mode flow ring
is one per priority with the first frame's destination, as ours. Next boot: each of the first 24 frames said as it is
sent, and the totals in the halt report.
**Run 15:** the same trap (pc 0x3bd468). At the halt: 295 frames sent, the longest 1242 bytes, IPv4 265, IPv6 22, ARP 8
-- ordinary frames (the per-frame lines were overwritten in the kernel's buffer between the logger's reconnects). The
host_cap written is 0x10000000 (EDL ring) with IPC version 0; Google's dhd_set_host_cap() ORs in the active IPC version
(9 here) and, for this firmware's flags, HOSTRDY, DS_NO_OOB_DW, EXTENDED_TRAP_DATA, UR_FW_NO_TRAP: 0x10821409. Next boot:
wifi_hostcap=0x10821409 (aggregation still off).
**Run 16 (host_cap 0x10821409): NO HALT.** Joined (5500 MHz, -63 dBm), frames both ways: the first sends ordinary
(IPv6 ND and MLD, DHCP to port 67), 227 received (152 KB). Then a background scan from wifid at 279-281 s (status 5,
WLC_E_STATUS_NO_ACK) and from there nothing in, and the tx completions stopped at 212 of 340: 128 out, the rest
dropped. Next boot: the same with wifi_console=5 (the firmware's console every 5 s) to see the stall from its side.
**Run 17 (wifi_console=5): not a stall -- the same trap**, pc 0x3bd468 5 s after the join, in the console: with that
host_cap the firmware no longer says it halted through the mailbox. Disassembled (llvm-objdump, thumbv7a, the firmware at
0x2a0000): 0x3bd468 is an ASSERT's store to 0 in txq_hw_fill -- a batch's descriptor bytes, 12 x (byte at +0x3e8 minus
byte at +0x3e9) + 84 x (byte at +0x3e9), in blocks of 960, more than 6. Frames handed at once beyond what it takes: iOS's
hundreds right after a join, each posted by this driver at once (128 in flight at most). Next boot: wifi_txmax=16 (at
most 16 in flight; the host_cap back to its default, so that a halt is said again).
**Run 18 (wifi_txmax=16): WI-FI WORKS.** No halt; joined at the second try; 37 MB received, 9935 frames sent over
minutes, link up throughout. But 8599 frames refused at the limit for iOS to send again. Now the limit is the 4398's
default (16 in flight) and a frame past it waits in its send buffer, posted as the chip gives buffers back (tx_drain
from d2h_tx_poll); only with all 128 buffers full is one refused. Aggregation still off (wifi_noagg=1).
**Run 19 (queued, 16 in flight):** joined; an IPv4 address by DHCP and IPv6 of the network; TCP connections to
port 443 made one after another, no halt in the 18 s the log covers (it ends there).
Then, loading YouTube, the whole system crashed and the phone restarted: the USB log ends at 344 s (WebKit starting),
and the bootloader kept nothing (RST_STAT PIN_RESET, no HakiOS lines). After a run comes back the loader set a 30 s
watchdog and U-Boot's hakios_failed reset after 60 s -- both resets that lose the log. Now the loader stops the cluster-0
watchdog once its log is written (xnuloader/loader.c, wdt_stop_for_log) and hakios_failed waits, saying to hold the
power button: the reset that keeps it.
**Run 20:** YouTube plays music over Wi-Fi (the tester). 43 MB in, ~16600 frames out, no halt; at most 112 frames
waiting, 3768 refused with all 128 buffers full. No crash: the same boot at 599 s, backboardd still pid 39; the screen
went off and on (RequestPowerChange 0/1) -- a lock, not a restart.
**The GPU for iOS (METAL_PLUGIN=ca):** the iPhone 11's 16.5 (20F66) IPSW from Apple's CDN (SHA-1 3bcbdb42..., as
ipsw.me lists it), its root volume (096-04258-076.dmg) read by 7-Zip 23.01, QuartzCore.framework/default.metallib
(1 MB, MTLB) kept in xnu-deps/build/ipsw-20F66/qc (out of git; the IPSW and the dmg deleted); tools/ca-metallib.py with
CA_QC_METALLIB= that -> ~/ca-share/QuartzCore-default.metallib (141 functions, the uber shaders generic). The user
space: METAL_PLUGIN=ca EXTRA_SHARE=~/ca-share BIG_IN_ROOT=1 HAKIOS_METAL_OUT=ios-mali (and the switches above); the
boot: bootrun.sh's GPU=1 arguments -- ca_metallib=/redwood/share/ca-share/QuartzCore-default.metallib display_hz=120
fbwidth=1080 fbheight=2340 fbscale=3 fbsubtype=2340 fb_zc=1 anim_max=1 (metal=1 metal_all=1 the defaults).
**Run 21: swiping much smoother** (the tester), the display 1080 x 2340 centred on the panel's 1080 x 2400, not scaled
-- the iPhone 13 mini's own, for zero copy (fb_zc=1 wants the panel's width, letterboxed). Before: 1125 x 2436 scaled
to the panel. Built beside it, hakios-shiba-flash-full.img: the GPU arguments without fbwidth/fbheight/fbscale/
fbsubtype/fb_zc -- full screen, scaled, a copy a frame.

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
