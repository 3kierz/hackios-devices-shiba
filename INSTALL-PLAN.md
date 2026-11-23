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
