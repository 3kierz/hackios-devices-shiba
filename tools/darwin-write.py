#!/usr/bin/env python3
"""The Pixel 8's darwin writer: the kernel tree's tools/darwin-write.py with this phone's darwin (phase 4).

usage: devices/shiba/tools/darwin-write.py [--table DARWIN.JSON] check|sum|plan|write|zero|selftest ...
       devices/shiba/tools/darwin-write.py [--table DARWIN.JSON] offline-test GPT-HEAD.NEW

The commands, the refusals and the log are that tool's (its usage below). What changes is darwin itself: the POCO's
constants are replaced by DARWIN.JSON, which devices/shiba/tools/carve.py made with the new table (default:
devices/shiba/boot-info/darwin.json, out of git -- copy BACKUPDIR/carve/darwin.json there once that table is on the
phone): entry 35 of LUN 0 (sda35), its unique GUID, its LBAs. Before any write the phone is read again and must say the
same, all of it, as that tool checks; the logical unit is checked to be behind the Pixel's UFS controller (13200000.ufs).

"protected_blocks" in DARWIN.JSON is the Darwin volume at darwin's start, which is never written once it is there. It
is null until phase 4 sets it, and while it is null every write and zero is refused (check, sum and selftest work).

offline-test: the refusals and the GPT check against a table carve.py made, on the computer, with no phone.
"""
import json
import os
import sys

import shiba_install as S

HERE = os.path.dirname(os.path.abspath(__file__))


def load(path):
    try:
        info = json.load(open(path))
    except FileNotFoundError:
        sys.exit('darwin-write: no %s: darwin is made in phase 3 (devices/shiba/tools/carve.py writes it)' % path)
    tool = S.upstream('darwin-write')
    tool.NAME = info['name']
    tool.TYPE = info['type']
    tool.UNIQUE = info['unique']
    tool.LUN = info['lun']
    tool.PARTN = info['entry']
    tool.FIRST = info['first_lba']
    tool.LAST = info['last_lba']
    tool.BLOCKS = tool.LAST - tool.FIRST + 1
    tool.NODE = '/dev/block/sda%d' % tool.PARTN
    if (tool.PARTN, tool.LUN, tool.BLOCKS, info['block_size']) != (S.DARWIN_ENTRY, 0, info['blocks'], S.BS):
        sys.exit('darwin-write: %s does not describe sda%d of LUN 0' % (path, S.DARWIN_ENTRY))
    protected = info.get('protected_blocks')
    # null: nothing is writable (every block is "inside the Darwin volume") until phase 4 says where it ends
    tool.PROTECTED = tool.BLOCKS if protected is None else int(protected)
    upstream_identity = tool.identity
    upstream_phone = tool.phone

    def phone(script, binary=False):
        """that tool's phone(), with the carriage returns Windows' adb.exe puts at the end of each line taken out of
        text (binary output, and exec-out's raw reads, are left as they are)"""
        out = upstream_phone(script, binary)
        return out if binary else out.replace('\r\n', '\n').replace('\r', '')
    tool.phone = phone

    def identity():
        """that tool's identity(), with the Pixel's UFS controller in sysfs in place of the POCO's ufshc"""
        lun = tool.phone('echo "lun=$(readlink -f /sys/class/block/sda/device)"\n').strip()
        if S.UFS_HOST not in lun or not lun.endswith(':%d' % tool.LUN):
            raise tool.Refused('sda is %r, not logical unit %d of the UFS controller %s' % (lun, tool.LUN, S.UFS_HOST))
        try:
            return upstream_identity()
        except tool.Refused as e:
            # its own 'ufshc' check is the POCO's sysfs name; everything else it says stands
            rest = [p for p in str(e).split(': ', 1)[-1].split('; ') if 'not logical unit' not in p]
            if rest:
                raise tool.Refused('darwin is not what it was: ' + '; '.join(rest))
            return 'darwin is %s, entry %d of logical unit %d (%s), LBA %d..%d (%d blocks of %d), %s, %s' % (
                tool.NODE, tool.PARTN, tool.LUN, S.UFS_HOST, tool.FIRST, tool.LAST, tool.BLOCKS, S.BS, tool.TYPE,
                tool.UNIQUE)
    tool.identity = identity
    return tool, protected


def offline_test(tool, protected, head_new):
    """the refusals and the GPT check, with the phone's reads answered from a table carve.py made"""
    raw = open(head_new, 'rb').read()[S.BS:4 * S.BS]
    tool.phone_bytes = lambda command: raw
    tool.LOG = os.path.join(os.path.dirname(os.path.abspath(head_new)), 'darwin-write-offline-test.log')
    if protected is None:
        tool.PROTECTED = 1         # (for the test only: a Darwin volume of one block, so the range rules can be tried)
    ok = tool.cmd_selftest()
    for block in (tool.PROTECTED, tool.BLOCKS - 1):
        try:
            tool.blocks_ok(block, 1)
            tool.log('offline-test: block %d inside darwin, after the Darwin volume: allowed' % block)
        except tool.Refused as e:
            ok = False
            tool.log('offline-test: block %d was refused: %s' % (block, e))
    return ok


def main(argv):
    path = os.path.join(os.path.dirname(HERE), 'boot-info', 'darwin.json')
    if argv[:1] == ['--table']:
        path, argv = argv[1], argv[2:]
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0 if argv else 2
    tool, protected = load(path)
    if argv[0] == 'offline-test' and len(argv) == 2:
        return 0 if offline_test(tool, protected, argv[1]) else 1
    if argv[0] in ('write', 'zero') and protected is None:
        tool.log('REFUSED %s: protected_blocks is null in %s: the Darwin volume is not placed yet (phase 4)' %
                 (' '.join(argv), path))
        return 2
    sys.argv = [sys.argv[0]] + argv
    tool.main()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
