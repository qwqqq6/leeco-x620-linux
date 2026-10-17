#!/bin/sh
# Original TFA9890 DSP with existing calibration, serialized explicit I2S route.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
if test "$#" -ne 1 || test ! -f "$1" || test ! -r "$1"; then
    echo 'Usage: native-speaker-play FILE.wav (48 kHz, stereo, S16_LE, up to 60 seconds)' >&2
    exit 2
fi
file=$(readlink -f -- "$1")
helper=/usr/local/lib/native-tfa/native-tfa
libraries=/usr/local/lib/native-tfa/lib64
test -x "$helper"
test -c /dev/i2c_smartpa
exec 8>/run/native-audio/stream.lock
if ! flock -n 8; then
    echo 'Audio is already in use.' >&2
    exit 1
fi
control() { amixer -c 0 cget "name=$1" | sed -n 's/^  : values=//p'; }
original_side=$(control Audio_i2s0_SideGen_Switch)
original_hd=$(control Audio_i2s0_hd_Switch)
original_dl1hd=$(control Audio_I2S0dl1_hd_Switch)
if test "$original_side" != 0 || test "$(control Voice_Amp_Switch)" != 0; then
    echo 'An existing audio route is active.' >&2
    exit 1
fi
case "$original_hd:$original_dl1hd" in 0:0|0:1|1:0|1:1) ;; *) exit 1 ;; esac
child=
cleanup() {
    if test -n "$child"; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    # Also recover if the helper was killed without running its own cleanup.
    env -u LD_PRELOAD LD_LIBRARY_PATH="$libraries" "$helper" --off >/dev/null 2>&1 || true
    amixer -q -c 0 cset name=Audio_i2s0_SideGen_Switch Off || true
    amixer -q -c 0 cset name=Audio_i2s0_hd_Switch "$original_hd" || true
    amixer -q -c 0 cset name=Audio_I2S0dl1_hd_Switch "$original_dl1hd" || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
amixer -q -c 0 cset name=Audio_i2s0_hd_Switch On
amixer -q -c 0 cset name=Audio_I2S0dl1_hd_Switch On
amixer -q -c 0 cset name=Audio_i2s0_SideGen_Switch On48000
env -u LD_PRELOAD LD_LIBRARY_PATH="$libraries" "$helper" --play "$file" 8>&- &
child=$!
result=0
wait "$child" || result=$?
child=
exit "$result"
