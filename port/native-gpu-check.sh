#!/bin/sh
# A 4x4 offscreen shader/readback health check, independent of Xorg/fbdev.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
test "$(id -u)" = 0 || { echo 'Use root SSH for the GPU check.' >&2; exit 77; }
test "$#" = 0 || { echo 'Usage: native-gpu-check' >&2; exit 64; }
umask 077
runtime=/run/native-gpu
root=$runtime/root
base=/usr/local/lib/native-gpu
exec 9>/run/native-gpu-control.lock
flock -n 9 || { echo 'Another GPU check is running.' >&2; exit 73; }
test -f /etc/native-installed
device_number() {
    value=$(cat "/sys/class/misc/$1/dev")
    major=${value%:*}
    minor=${value#*:}
    case "$major:$minor" in *[!0-9:]*|:*|*:) return 1;; esac
    test "$major" = 10 && test "$minor" -ge 0 && test "$minor" -le 255
    test -c "/dev/$1"
    test "$(stat -c '%t:%T' "/dev/$1")" = "$(printf '%x:%x' "$major" "$minor")"
    printf '%s\n' "$value"
}
mali_device=$(device_number mali0)
ion_device=$(device_number ion)
sync_device=$(device_number sw_sync)
(cd / && sha256sum -c /usr/local/share/native-gpu/SHA256SUMS >/dev/null)
test ! -L "$runtime"
test ! -e "$root" && test ! -e "$runtime/result.log" || exit 73
child=
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    cleanup_failed=0
    if test -n "$child"; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
        child=
    fi
    if awk -v point="$root/system" '$2==point {found=1} END {exit !found}' /proc/mounts; then
        umount "$root/system" || cleanup_failed=1
    fi
    if awk -v prefix="$root" '$2==prefix || index($2,prefix "/")==1 {found=1} END {exit !found}' /proc/mounts; then
        echo 'GPU_CLEANUP_REFUSED: runtime still has a mount.' >&2
        cleanup_failed=1
    elif test -d "$root"; then
        find "$root" -xdev -depth ! -type d -exec rm -f -- {} \; || cleanup_failed=1
        find "$root" -xdev -depth -type d -exec rmdir {} \; || cleanup_failed=1
    fi
    if test -f "$runtime/result.log"; then
        cp "$runtime/result.log" /var/log/native-gpu-check.log.new
        chmod 600 /var/log/native-gpu-check.log.new
        mv /var/log/native-gpu-check.log.new /var/log/native-gpu-check.log
        cat "$runtime/result.log"
        rm "$runtime/result.log"
    fi
    if test "$cleanup_failed" = 0; then
        if test -d "$runtime"; then rmdir "$runtime" || cleanup_failed=1; fi
        echo GPU_RUNTIME_CLEANED
    fi
    test "$cleanup_failed" = 0 || exit 1
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$runtime"
chmod 700 "$runtime"
mkdir -m 700 "$root"
mkdir -p "$root/system" "$root/dev/socket" "$root/data/local/tmp" "$root/data/misc/gpu" "$root/cache"
for node in null zero urandom mali0 ion sw_sync; do
    case "$node" in
        null) major=1; minor=3;; zero) major=1; minor=5;; urandom) major=1; minor=9;;
        mali0) major=${mali_device%:*}; minor=${mali_device#*:};;
        ion) major=${ion_device%:*}; minor=${ion_device#*:};;
        sw_sync) major=${sync_device%:*}; minor=${sync_device#*:};;
    esac
    mknod -m 600 "$root/dev/$node" c "$major" "$minor"
done
cp "$base/native-gpu-probe" "$root/native-gpu-probe"
chmod 700 "$root/native-gpu-probe"
mount --bind "$base/system" "$root/system"
mount -o remount,bind,ro "$root/system"
test "$(awk -v point="$root/system" '$2==point {print $4}' /proc/mounts | cut -d, -f1)" = ro
ulimit -c 0
printf 'GPU_DEVICES mali=%s ion=%s sw_sync=%s\n' "$mali_device" "$ion_device" "$sync_device"
timeout -k 3 20 env -u LD_PRELOAD -u LD_LIBRARY_PATH /usr/local/libexec/native-gpu-run >"$runtime/result.log" 2>&1 9>&- &
child=$!
result=0
wait "$child" || result=$?
child=
exit "$result"
