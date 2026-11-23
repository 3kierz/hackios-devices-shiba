#!/usr/bin/env python3
"""The Pixel 8's backup before anything is written to it (INSTALL-PLAN.md, phase 0): READ ONLY.

usage: devices/shiba/tools/backup.py OUTDIR [--yes] [--only NAME,NAME] [--with-bootchain]

The kernel tree's tools/backup-partitions.py with the Pixel 8's partition names (its own are the POCO's Qualcomm ones).
Everything else is that tool's: it only reads from the phone (ls, blockdev, dd if=, sha256sum, as root over adb), each
copy is proven by three SHA-256s (the phone's before, the computer's, the phone's after), manifest.json and restore.txt
are written, and without --yes nothing is copied: it says what would be and how much room that takes.

What is copied (names from /dev/block/by-name; one the phone does not have is skipped and said):
  what decides whether it boots   boot, init_boot, vendor_boot, vendor_kernel_boot, dtbo, vbmeta, vbmeta_system,
                                  vbmeta_vendor, pvmfw (both slots); misc, frp; devinfo, pinfo, blenv (LUN 3)
  THIS phone's identity and radio persist, efs, efs_backup, modem_userdata, trusty_persist, trusty_userdata, mfg_data
                                  (LUN 3), and the small ones nothing documents: fips, ufs_internal
  the partition tables            the first and last 8 blocks of each LUN (sda ... sdd): what the carve changes on sda
  with --with-bootchain           Google's boot chain on LUNs 1 and 2, both slots: bl1 pbl bl2 dram_train abl bl31
                                  tzsw gsa gsa_bl1 ldfw dpm gcf (Google's factory image has them too)
Never copied: userdata, super, metadata, modem_a/b (Android's, made again by fastboot -w or in the factory image).

restore.txt is that tool's text: it names fastboot commands for a person to run, and warns about the boot chain in the
POCO's words (EDL); on the Pixel 8 the way back for the boot chain is Google's factory image or Android Flash Tool.
The files hold this phone's identity: keep them off the network and out of git (backups/ is ignored).
Needs root on the phone (`adb shell su -c id`).
"""
import sys

import shiba_install

BOOT = [p + s for p in ('boot', 'init_boot', 'vendor_boot', 'vendor_kernel_boot', 'dtbo', 'vbmeta', 'vbmeta_system',
                        'vbmeta_vendor', 'pvmfw') for s in ('_a', '_b')] + ['misc', 'frp', 'devinfo', 'pinfo', 'blenv']
IDENTITY = ['persist', 'efs', 'efs_backup', 'modem_userdata', 'trusty_persist', 'trusty_userdata', 'mfg_data', 'fips',
            'ufs_internal']
BOOTCHAIN = [p + s for s in ('_a', '_b') for p in ('bl1', 'pbl', 'bl2', 'dram_train', 'abl', 'bl31', 'tzsw', 'gsa',
                                                    'gsa_bl1', 'ldfw', 'dpm', 'gcf')]


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0 if argv else 2
    tool = shiba_install.upstream('backup-partitions')
    tool.BOOT, tool.IDENTITY, tool.BOOTCHAIN = BOOT, IDENTITY, BOOTCHAIN
    return tool.main(argv)


if __name__ == '__main__':
    sys.exit(main())
