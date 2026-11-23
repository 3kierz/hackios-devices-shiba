#!/usr/bin/env python3
"""The Pixel 8's install tools tried on the computer, with no phone (INSTALL-PLAN.md, phase 2).

usage: devices/shiba/tests/install-tools-test.py GPT-SDA-HEAD.BIN [GIB]

GPT-SDA-HEAD.BIN: LUN 0's first 8 blocks as read from the phone (backup.py's gpt-sda-head.bin, or
`dd if=/dev/block/sda bs=4096 count=8` under Android). It is the phone's own and stays out of git; nothing of it is
written anywhere but a temporary folder. The backup copy of the table (the unit's last 8 blocks) is made from it here
as a firmware would have it; backup.py copies the real one before phase 3, and carve.py is to be run on that.

  1. backup.py, without --yes, against a stand-in adb that answers with the Pixel 8's partitions: what it would copy
  2. carve.py on the two: the new table, checked by userdata-carve.py and by carve.py
  3. darwin-write.py offline-test on that table: every refusal refuses, the table is darwin's
  4. carve.py refuses a table that is not the planned one (userdata not the last entry)
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), 'tools')
BS = 4096
DISK = 31196160

FAKE_ADB = r'''
import sys
cmd = sys.argv[-1]
parts = {%s}
if cmd.startswith('ls -l /dev/block/by-name'):
    for n, d in parts.items():
        print('lrwxrwxrwx 1 root root 16 2026-11-23 01:00 %%s -> %%s' %% (n, d))
elif cmd.startswith('blockdev --getsize64 '):
    node = cmd.split()[-1]
    print(4096 * (31196160 if node == '/dev/block/sda' else 8192 if node in ('/dev/block/sdb', '/dev/block/sdc') else 1024 if node == '/dev/block/sdd' else 16384))
else:
    sys.exit('the stand-in adb does not answer %%r' %% cmd)
'''
NAMES = {'sda': ['persist', 'klog', 'misc', 'frp', 'efs', 'efs_backup', 'modem_userdata', 'trusty_persist',
                 'trusty_userdata', 'metadata', 'fips', 'ufs_internal'] +
                [p + s for s in ('_a', '_b') for p in ('boot', 'init_boot', 'vendor_boot', 'vendor_kernel_boot', 'dtbo',
                                                      'vbmeta', 'vbmeta_system', 'vbmeta_vendor', 'pvmfw', 'modem')] +
                ['super', 'userdata'],
         'sdb': [p + '_a' for p in ('bl1', 'pbl', 'bl2', 'dram_train', 'abl', 'bl31', 'tzsw', 'gsa', 'gsa_bl1', 'ldfw', 'dpm', 'gcf')],
         'sdc': [p + '_b' for p in ('bl1', 'pbl', 'bl2', 'dram_train', 'abl', 'bl31', 'tzsw', 'gsa', 'gsa_bl1', 'ldfw', 'dpm', 'gcf')],
         'sdd': ['devinfo', 'pinfo', 'blenv', 'mfg_data']}


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def backup_copy(head):
    """the unit's last 8 blocks, as the primary's header and table say the backup is"""
    h = bytearray(head[BS:2 * BS])
    n, es = struct.unpack_from('<II', h, 80)
    table = head[2 * BS:2 * BS + n * es]
    tail = bytearray(8 * BS)
    tail[3 * BS:3 * BS + len(table)] = table               # LBA DISK-5
    struct.pack_into('<QQ', h, 24, DISK - 1, 1)
    struct.pack_into('<Q', h, 72, DISK - 5)
    size = struct.unpack_from('<I', h, 12)[0]
    struct.pack_into('<I', h, 16, 0)
    struct.pack_into('<I', h, 16, crc(bytes(h[:size])))
    tail[7 * BS:] = h
    return bytes(tail)


def run(what, args, expect_ok=True):
    r = subprocess.run([sys.executable] + args, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    good = (r.returncode == 0) == expect_ok
    print('%s %s (exit %d)' % ('PASS' if good else 'FAIL', what, r.returncode))
    if not good or os.environ.get('VERBOSE'):
        print('    ' + out.replace('\n', '\n    '))
    return good, out


def main(argv):
    if not argv:
        sys.exit(__doc__)
    head = open(argv[0], 'rb').read()
    gib = argv[1] if len(argv) > 1 else '32'
    ok = True
    with tempfile.TemporaryDirectory() as t:
        fake = os.path.join(t, 'adb.py')
        parts = ', '.join('%r: %r' % (n, '/dev/block/%s%d' % (disk, i + 1))
                          for disk, names in NAMES.items() for i, n in enumerate(names))
        open(fake, 'w').write(FAKE_ADB % parts)
        good, out = run('backup.py plans (no --yes)', [os.path.join(TOOLS, 'backup.py'), os.path.join(t, 'b'),
                                                       '--with-bootchain', '--adb', sys.executable, '--adb-arg', fake])
        skipped = [l for l in out.splitlines() if 'does not have it' in l]
        print('    %s; %d skipped%s' % (out.splitlines()[0] if out else '?', len(skipped),
                                       (': ' + ', '.join(l.split()[0] for l in skipped)) if skipped else ''))
        ok &= good and not skipped and not os.path.exists(os.path.join(t, 'b'))

        src = os.path.join(t, 'backup')
        os.makedirs(src)
        open(os.path.join(src, 'gpt-sda-head.bin'), 'wb').write(head)
        open(os.path.join(src, 'gpt-sda-tail.bin'), 'wb').write(backup_copy(head))
        good, out = run('carve.py %s GiB' % gib, [os.path.join(TOOLS, 'carve.py'), src, gib])
        print('    ' + '\n    '.join(l for l in out.splitlines() if l.startswith(('carve: darwin', 'userdata:', 'darwin:',
                                                                                  'blocks that differ', '     dd'))))
        ok &= good
        table = os.path.join(src, 'carve', 'darwin.json')
        good, out = run('darwin-write.py offline-test', [os.path.join(TOOLS, 'darwin-write.py'), '--table', table,
                                                          'offline-test', os.path.join(src, 'carve', 'gpt-head.new')])
        print('    ' + out.splitlines()[-1] if out else '')
        ok &= good
        good, out = run('darwin-write.py refuses a write while protected_blocks is null',
                        [os.path.join(TOOLS, 'darwin-write.py'), '--table', table, 'zero', '1000000', '1'], expect_ok=False)
        ok &= good and 'protected_blocks is null' in out

        # a table where userdata is not the last entry: carve.py must refuse and make nothing
        bad = bytearray(head)
        n, es = struct.unpack_from('<II', bad, BS + 80)
        e33, e34 = 2 * BS + 32 * es, 2 * BS + 33 * es
        bad[e33:e33 + es], bad[e34:e34 + es] = head[e34:e34 + es], head[e33:e33 + es]
        struct.pack_into('<I', bad, BS + 88, crc(bytes(bad[2 * BS:2 * BS + n * es])))
        size = struct.unpack_from('<I', bad, BS + 12)[0]
        struct.pack_into('<I', bad, BS + 16, 0)
        struct.pack_into('<I', bad, BS + 16, crc(bytes(bad[BS:BS + size])))
        src2 = os.path.join(t, 'backup2')
        os.makedirs(src2)
        open(os.path.join(src2, 'gpt-sda-head.bin'), 'wb').write(bytes(bad))
        open(os.path.join(src2, 'gpt-sda-tail.bin'), 'wb').write(backup_copy(bytes(bad)))
        good, out = run('carve.py refuses a table with userdata not entry 34', [os.path.join(TOOLS, 'carve.py'), src2, gib],
                        expect_ok=False)
        ok &= good and not os.path.exists(os.path.join(src2, 'carve', 'gpt-head.new'))
    print('install-tools-test: %s' % ('all passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
