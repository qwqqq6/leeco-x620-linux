#!/bin/sh
# Explicit, serialized PMIC earpiece playback. Tested with 48 kHz stereo
# S16_LE WAV. Do not enable the separate NXP loudspeaker or automatic hooks.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
if test "$#" -ne 1 || test ! -f "$1" || test ! -r "$1"; then
    echo 'Usage: native-receiver-play FILE.wav (48 kHz, stereo, S16_LE)' >&2
    exit 2
fi
exec 8>/run/native-audio/stream.lock
if ! flock -n 8; then
    echo 'Receiver is already in use.' >&2
    exit 1
fi
original_gain=$(amixer -c 0 cget name=Handset_PGA_GAIN | sed -n 's/^  : values=//p')
original_amp=$(amixer -c 0 cget name=Voice_Amp_Switch | sed -n 's/^  : values=//p')
case "$original_gain" in
    [0-9]|1[0-9]) ;;
    *) echo 'Cannot read receiver gain.' >&2; exit 1 ;;
esac
if test "$original_amp" != 0; then
    echo 'Receiver is already enabled by another audio route.' >&2
    exit 1
fi
cleanup() {
    amixer -q -c 0 cset name=Voice_Amp_Switch Off || true
    amixer -q -c 0 cset name=Handset_PGA_GAIN "$original_gain" || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
amixer -q -c 0 cset name=Handset_PGA_GAIN 16
amixer -q -c 0 cset name=Voice_Amp_Switch On
aplay -D hw:0,0 "$1" 8>&-
