"""What the Pixel 8's install tools share: the facts of this phone's storage, and the kernel tree's own tools.

The tools in the kernel tree (tools/backup-partitions.py, userdata-carve.py, darwin-write.py) were written for the POCO
X5 Pro: their partition lists and darwin's position are that phone's. The Pixel 8's tools in devices/shiba/tools/ load
them and give them this phone's facts instead, so that every check and refusal they make stays the tested one.

The facts below were read from this phone (Android, 2026-11-23) and again by HakiOS's own U-Boot and loader
(INSTALL-PLAN.md, phase 1, runs 7-9): LUN 0 (sda) holds 31,196,160 blocks of 4096; its GPT has 34 entries of 128 bytes,
all used, at LBA 2 (room for 128 before the first usable LBA, 6), the backup table at LBA 31196155; userdata is entry 34,
LBA 2449894..31196154, the last partition, to the last usable LBA.
"""
import importlib.util
import os
import sys

BS = 4096
LUN0_BLOCKS = 31196160
GPT_ENTRIES = 34
GPT_ENTRIES_LBA = 2
GPT_FIRST_USABLE = 6
GPT_LAST_USABLE = 31196154
USERDATA_ENTRY = 34                     # sda34
USERDATA_FIRST = 2449894
DARWIN_ENTRY = 35                       # sda35, the first free entry once the table is made longer
GPT_ENTRIES_AFTER = 64                  # the table made longer: 64 entries, LBA 2..3 (darwin-write.py reads LBA 1..3)
UFS_HOST = '13200000.ufs'               # sysfs: /sys/class/block/sda/device leads through the UFS controller's node


def tree():
    """The kernel tree these tools run in: the first directory up from here (as called, not resolved: devices/shiba is
    a link into the tree) that has tools/userdata-carve.py, or $HAKIOS_TREE."""
    d = os.path.dirname(os.path.abspath(sys.argv[0] if sys.argv and sys.argv[0] else __file__))
    for _ in range(6):
        if os.path.isfile(os.path.join(d, 'tools', 'userdata-carve.py')) and os.path.isdir(os.path.join(d, 'devices')):
            return d
        d = os.path.dirname(d)
    t = os.environ.get('HAKIOS_TREE')
    if t and os.path.isfile(os.path.join(t, 'tools', 'userdata-carve.py')):
        return t
    sys.exit('the kernel tree was not found: run this as devices/shiba/tools/... from it, or set HAKIOS_TREE')


def upstream(name):
    """The kernel tree's tools/NAME.py, loaded as a module (nothing runs: each has a __main__ guard)."""
    path = os.path.join(tree(), 'tools', name + '.py')
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
