#!/usr/bin/env python3
"""The Pixel 8's new partition table, made and checked ON THE COMPUTER ONLY (INSTALL-PLAN.md, phase 2).

usage: devices/shiba/tools/carve.py BACKUPDIR GIB

BACKUPDIR is what devices/shiba/tools/backup.py made (gpt-sda-head.bin and gpt-sda-tail.bin: LUN 0's first and last
8 blocks, proven against the phone). GIB is darwin's size in GiB (INSTALL-PLAN.md: 24-32 is a reasonable start).

Nothing here touches the phone. It checks that LUN 0's table is still exactly the one this port was planned on
(shiba_install.py: the unit's size, 34 entries at LBA 2, userdata entry 34 from LBA 2449894 to the last usable block),
then runs the kernel tree's tools/userdata-carve.py --wipe --entries 64 on a copy of the two files in
BACKUPDIR/carve/: the table made longer (34 entries used of 64; the room is there and zeros, which that tool checks),
userdata shortened, darwin (APFS container type, a new unique GUID) the last GIB of the unit from a 2 MB boundary as
entry 35 -- sda35 under Android. Both copies of the table are made with their CRCs, read back and checked as a firmware
would, and that tool refuses unless exactly those two entries differ.

It then checks the result again itself, and writes BACKUPDIR/carve/darwin.json: darwin's entry, GUIDs and LBAs, what
devices/shiba/tools/darwin-write.py later compares the phone with before it writes anything. Its "protected_blocks"
(the Darwin volume at darwin's start, never written once it is there) stays null until phase 4.

What that tool then prints -- the dd commands that put gpt-head.new and gpt-tail.new on the phone, and fastboot -w --
is phase 3: it ERASES Android's data, and is the user's decision alone. This script does not run any of it.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

import shiba_install as S


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def table(blob, header_at, base):
    """(header facts, entries) of one copy: blob starts at LBA base, its header at blob offset header_at"""
    h = blob[header_at:header_at + S.BS]
    if h[:8] != b'EFI PART':
        sys.exit('carve: no GPT header where one should be')
    size = struct.unpack_from('<I', h, 12)[0]
    hh = bytearray(h[:size])
    want = struct.unpack_from('<I', hh, 16)[0]
    hh[16:20] = b'\0\0\0\0'
    my, alt, first, last = struct.unpack_from('<QQQQ', h, 24)
    lba, n, es = struct.unpack_from('<QII', h, 72)
    tcrc = struct.unpack_from('<I', h, 88)[0]
    raw = blob[(lba - base) * S.BS:(lba - base) * S.BS + n * es]
    ents = []
    for i in range(n):
        e = raw[i * es:(i + 1) * es]
        ents.append({'type': uuid.UUID(bytes_le=e[:16]), 'guid': uuid.UUID(bytes_le=e[16:32]),
                     'first': struct.unpack_from('<Q', e, 32)[0], 'last': struct.unpack_from('<Q', e, 40)[0],
                     'name': e[56:128].decode('utf-16le').split('\0', 1)[0]})
    return {'crc_right': crc(bytes(hh)) == want, 'table_crc_right': crc(raw) == tcrc, 'my': my, 'alt': alt,
            'first_usable': first, 'last_usable': last, 'lba': lba, 'count': n, 'esize': es}, ents


def planned_on(head, tail):
    """the table now is the one this port was planned on, or exit"""
    ph, pe = table(head, S.BS, 0)
    bh, be = table(tail, 7 * S.BS, S.LUN0_BLOCKS - 8)
    bad = []
    for what, got, want in (('primary header CRC', ph['crc_right'], True), ('primary table CRC', ph['table_crc_right'], True),
                            ('backup header CRC', bh['crc_right'], True), ('backup table CRC', bh['table_crc_right'], True),
                            ('the backup header\'s place', bh['my'], S.LUN0_BLOCKS - 1),
                            ('the primary\'s other copy', ph['alt'], S.LUN0_BLOCKS - 1),
                            ('entries', ph['count'], S.GPT_ENTRIES), ('the table\'s LBA', ph['lba'], S.GPT_ENTRIES_LBA),
                            ('entry size', ph['esize'], 128), ('first usable LBA', ph['first_usable'], S.GPT_FIRST_USABLE),
                            ('last usable LBA', ph['last_usable'], S.GPT_LAST_USABLE),
                            ('the two tables', pe == be, True)):
        if got != want:
            bad.append('%s is %r, not %r' % (what, got, want))
    u = pe[S.USERDATA_ENTRY - 1]
    if (u['name'], u['first'], u['last']) != ('userdata', S.USERDATA_FIRST, S.GPT_LAST_USABLE):
        bad.append('entry %d is %s %d..%d, not userdata %d..%d' % (S.USERDATA_ENTRY, u['name'], u['first'], u['last'],
                                                                     S.USERDATA_FIRST, S.GPT_LAST_USABLE))
    if any(e['name'] == 'darwin' for e in pe):
        bad.append('there is a darwin already')
    if bad:
        sys.exit('carve: LUN 0\'s table is not the one this port was planned on: ' + '; '.join(bad) + '. Nothing made.')
    return pe


def main(argv):
    if len(argv) != 2 or not argv[1].isdigit():
        sys.exit(__doc__)
    src, gib = argv[0], int(argv[1])
    if not 8 <= gib <= 96:
        sys.exit('carve: %d GiB: darwin is to be 8 to 96 GiB (iOS needs about 9 before its data; userdata keeps the rest)' % gib)
    head = open(os.path.join(src, 'gpt-sda-head.bin'), 'rb').read()
    tail = open(os.path.join(src, 'gpt-sda-tail.bin'), 'rb').read()
    if len(head) != 8 * S.BS or len(tail) != 8 * S.BS:
        sys.exit('carve: gpt-sda-head.bin and gpt-sda-tail.bin are to be 8 blocks each (backup.py makes them)')
    before = planned_on(head, tail)
    print('carve: LUN 0\'s table is the one this port was planned on (34 entries, userdata sda34 the last)')

    work = os.path.join(src, 'carve')
    os.makedirs(work, exist_ok=True)
    shutil.copyfile(os.path.join(src, 'gpt-sda-head.bin'), os.path.join(work, 'gpt-head.bin'))
    shutil.copyfile(os.path.join(src, 'gpt-sda-tail.bin'), os.path.join(work, 'gpt-tail.bin'))
    for f in ('gpt-head.new', 'gpt-tail.new', 'darwin.json'):
        if os.path.exists(os.path.join(work, f)):
            os.remove(os.path.join(work, f))
    tool = os.path.join(S.tree(), 'tools', 'userdata-carve.py')
    r = subprocess.run([sys.executable, tool, '--wipe', '--entries', str(S.GPT_ENTRIES_AFTER), work, str(gib), 'darwin'])
    if r.returncode != 0 or not os.path.exists(os.path.join(work, 'gpt-head.new')):
        sys.exit('carve: userdata-carve.py made no table (%d)' % r.returncode)

    # the result, checked again here: the same checks as before, with the Pixel's own expectations
    nhead = open(os.path.join(work, 'gpt-head.new'), 'rb').read()
    ntail = open(os.path.join(work, 'gpt-tail.new'), 'rb').read()
    ph, pe = table(nhead, S.BS, 0)
    bh, be = table(ntail, 7 * S.BS, S.LUN0_BLOCKS - 8)
    d, u = pe[S.DARWIN_ENTRY - 1], pe[S.USERDATA_ENTRY - 1]
    want_first = (S.GPT_LAST_USABLE + 1 - gib * 2**30 // S.BS) // 512 * 512
    problems = [what for what, ok in (
        ('CRCs', ph['crc_right'] and ph['table_crc_right'] and bh['crc_right'] and bh['table_crc_right']),
        ('the two copies the same', pe == be), ('64 entries', ph['count'] == bh['count'] == S.GPT_ENTRIES_AFTER),
        ('the other entries as they were', pe[:S.USERDATA_ENTRY - 1] == before[:S.USERDATA_ENTRY - 1]),
        ('userdata from where it was', (u['name'], u['first'], u['guid']) == ('userdata', S.USERDATA_FIRST,
                                                                             before[S.USERDATA_ENTRY - 1]['guid'])),
        ('userdata up to darwin', u['last'] == d['first'] - 1),
        ('darwin entry 35', d['name'] == 'darwin' and str(d['type']).upper() == '7C3457EF-0000-11AA-AA11-00306543ECAC'),
        ('darwin the last GiB asked for', (d['first'], d['last']) == (want_first, S.GPT_LAST_USABLE)),
        ('darwin on a 2 MB boundary', d['first'] % 512 == 0),
        ('nothing after darwin', all(e['type'].int == 0 for e in pe[S.DARWIN_ENTRY:])),
        ('the usable range unchanged', (ph['first_usable'], ph['last_usable']) == (S.GPT_FIRST_USABLE, S.GPT_LAST_USABLE)),
    ) if not ok]
    if problems:
        for f in ('gpt-head.new', 'gpt-tail.new'):
            os.remove(os.path.join(work, f))
        sys.exit('carve: the new table fails: ' + ', '.join(problems) + '. Both files removed.')
    info = {'name': 'darwin', 'lun': 0, 'entry': S.DARWIN_ENTRY, 'node': '/dev/block/sda%d' % S.DARWIN_ENTRY,
            'type': str(d['type']).upper(), 'unique': str(d['guid']).upper(), 'first_lba': d['first'],
            'last_lba': d['last'], 'blocks': d['last'] - d['first'] + 1, 'block_size': S.BS,
            'protected_blocks': None,
            'userdata': {'first_lba': u['first'], 'last_lba': u['last']},
            'gpt_head_new_sha256': __import__('hashlib').sha256(nhead).hexdigest(),
            'gpt_tail_new_sha256': __import__('hashlib').sha256(ntail).hexdigest()}
    with open(os.path.join(work, 'darwin.json'), 'w', newline='\n') as f:
        json.dump(info, f, indent=2)
        f.write('\n')
    print('\ncarve: checked again here: %s' % ', '.join(('CRCs right', 'both copies the same', '64 entries',
                                                       'entries 1-33 unchanged', 'userdata ends where darwin begins')))
    print('carve: darwin sda35, LBA %d..%d (%.2f GiB); userdata %d..%d (%.2f GiB)' % (
        d['first'], d['last'], info['blocks'] * S.BS / 2**30, u['first'], u['last'],
        (u['last'] - u['first'] + 1) * S.BS / 2**30))
    print('carve: %s/darwin.json written (keep it with the backup, out of git: it holds this phone\'s new GUID)' % work)
    print('carve: NOTHING was written to the phone. The commands above are phase 3, which erases Android\'s data.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
