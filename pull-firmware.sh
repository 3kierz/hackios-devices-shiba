#!/bin/bash
# devices/shiba/pull-firmware.sh (make firmware DEVICE=shiba): the Pixel 8's firmware off its own Android, into the
# folders userland/build.sh packs into the image's fw/ -- never into git (the vendor's files: tools/provenance-check.py).
#
#   ../fw-gpu      mali_csffw-*.bin                    the Mali-G715's CSF firmware (drivers/gpu: fw/mali_csffw-r54p2.bin)
#   ../fw-haptic   cs40l26*.wmfw, cs40l26*.bin          Cirrus's RAM firmware and tunings for the vibration (haptic=3..5)
#   ../fw-audio    [R-]cs35l41-dsp1-spk-*               the speaker amplifiers' protection firmware and tunings (amp=3..4)
#   ../fw-aoc      aoc.bin                              the AoC's signed image (29 MB: audio, sensors; aoc=3)
#   ../fw-wifi     *_4398_* bcmdhd files, BCM*.hcd      the BCM4398's firmware, calibration, CLM and tx caps, and its
#                                                       Bluetooth patch: kept for when the 4398 is ported (wifi=, bt=)
#
# Needs the phone on adb with root (Magisk). Only reads: /vendor/firmware is copied to a staging folder by su and pulled.
set -e -o pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../.."
command -v adb >/dev/null || PATH=$HOME/bin:$PATH
S=/data/local/tmp/hakios-fw
adb get-state >/dev/null

adb shell "su -c 'sh -c \"
rm -rf $S; mkdir -p $S/gpu $S/haptic $S/audio $S/aoc $S/wifi
cd /vendor/firmware
cp mali_csffw-*.bin $S/gpu/
cp cs40l26*.wmfw cs40l26*.bin $S/haptic/
cp cs35l41-dsp1-spk-* R-cs35l41-dsp1-spk-* $S/audio/
cp aoc.bin $S/aoc/
cp fw_bcmdhd.bin_4398_* fw_bcmdhd.map_4398_* bcmdhd.cal_4398_* bcmdhd_clm.blob_4398_* bcmdhd_txcap.blob_4398_* $S/wifi/
cp brcm/BCM*.hcd $S/wifi/ 2>/dev/null || true
chmod -R a+rX $S\"'" || { echo "pull-firmware.sh: su did not answer (root is needed to read /vendor/firmware)" >&2; exit 1; }

for d in gpu haptic audio aoc wifi; do
	mkdir -p fw-$d
	adb pull "$S/$d/." "fw-$d/" >/dev/null
	echo "pull-firmware.sh: fw-$d: $(ls fw-$d | wc -l) files, $(du -sh fw-$d | cut -f1)"
done
adb shell "su -c 'rm -rf $S'" >/dev/null 2>&1 || true
echo "pull-firmware.sh: done; userland/build.sh packs what each driver reads into the image's fw/"
