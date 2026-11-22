#!/bin/bash
# devices/shiba/pull-log.sh: after a HakiOS run, back in Android (root), the run's log off the phone.
#
# HakiOS's U-Boot appends its console -- U-Boot, the loader, and the kernel's log the loader shows at the end -- to
# Google's bootloader's own log buffer (bldr_log_reserved@fd800000, hakios_zuma.c, $hakios_bldrlog), which survives the
# restart: the next bootloader reports it, and Android shows it as /sys/fs/pstore/console-ramoops-0.
#
#   devices/shiba/pull-log.sh              -> boot-info/run-YYYYmmdd-HHMMSS.txt, and the HakiOS part on the screen
#
# Needs root (Magisk) and adb (Windows' adb.exe works from WSL).
set -e -o pipefail
here=$(cd "$(dirname "$0")" && pwd)
command -v adb >/dev/null || PATH=$HOME/bin:$PATH
mkdir -p "$here/boot-info"
out=$here/boot-info/run-$(date +%Y%m%d-%H%M%S).txt
adb shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'" | tr -d '\r' > "$out"
echo "pull-log.sh: $out ($(wc -c < "$out") bytes)"
if grep -aq '===== HakiOS U-Boot on zuma' "$out"; then
	# the HakiOS part: from the last mark to the next bootloader start
	awk '/===== HakiOS U-Boot on zuma/ { buf = ""; on = 1 } on { buf = buf $0 "\n" } /welcome to lk\/MP|No kernel logs/ && on && buf != "" { last = buf; on = 0 }
	     END { printf "%s", on ? buf : last }' "$out" | tr -c '[:print:]\n\t' '.'
else
	echo "pull-log.sh: no HakiOS lines in it: U-Boot's pstore did not run, or the run left nothing the bootloader kept"
fi
