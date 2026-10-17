#!/bin/sh
# Capture through this handset's original builtin_Mic_Mic1 route.
# Enable the route before opening PCM; restore controls on every normal exit.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
if test "$#" -ne 2; then
    echo 'Usage: native-mic-record NEW_FILE.wav SECONDS (1-60)' >&2
    exit 2
fi
case "$2" in
    [1-9]|[1-5][0-9]|60) ;;
    *) echo 'Duration must be 1-60 seconds.' >&2; exit 2 ;;
esac
if test -e "$1" || test -L "$1"; then
    echo 'Output already exists; refusing to overwrite it.' >&2
    exit 2
fi
exec 8>/run/native-audio/stream.lock
if ! flock -n 8; then
    echo 'Native audio is already in use.' >&2
    exit 1
fi
get() { amixer -c 0 cget "name=$1" | sed -n 's/^  : values=//p'; }
mode=$(get Audio_MIC1_Mode_Select)
source=$(get Audio_MicSource1_Setting)
gain=$(get Audio_PGA1_Setting)
preamp1=$(get Audio_Preamp1_Switch)
preamp2=$(get Audio_Preamp2_Switch)
adc1=$(get Audio_ADC_1_Switch)
adc2=$(get Audio_ADC_2_Switch)
if test "$adc1" != 0 || test "$adc2" != 0; then
    echo 'A microphone route is already enabled.' >&2
    exit 1
fi
cleanup() {
    amixer -q -c 0 cset name=Audio_Preamp1_Switch OPEN || true
    amixer -q -c 0 cset name=Audio_Preamp2_Switch OPEN || true
    amixer -q -c 0 cset name=Audio_ADC_1_Switch Off || true
    amixer -q -c 0 cset name=Audio_ADC_2_Switch Off || true
    amixer -q -c 0 cset name=Audio_MIC1_Mode_Select "$mode" || true
    amixer -q -c 0 cset name=Audio_MicSource1_Setting "$source" || true
    amixer -q -c 0 cset name=Audio_PGA1_Setting "$gain" || true
    amixer -q -c 0 cset name=Audio_ADC_1_Switch "$adc1" || true
    amixer -q -c 0 cset name=Audio_ADC_2_Switch "$adc2" || true
    amixer -q -c 0 cset name=Audio_Preamp1_Switch "$preamp1" || true
    amixer -q -c 0 cset name=Audio_Preamp2_Switch "$preamp2" || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
amixer -q -c 0 cset name=Audio_MIC1_Mode_Select ACCMODE
amixer -q -c 0 cset name=Audio_MicSource1_Setting ADC1
amixer -q -c 0 cset name=Audio_PGA1_Setting 2
amixer -q -c 0 cset name=Audio_ADC_1_Switch On
amixer -q -c 0 cset name=Audio_ADC_2_Switch On
amixer -q -c 0 cset name=Audio_Preamp1_Switch IN_ADC1
amixer -q -c 0 cset name=Audio_Preamp2_Switch IN_ADC1
timeout "$(($2 + 3))" arecord -D hw:0,1 -f S16_LE -r 48000 -c 2 -d "$2" "$1" 8>&-
