#!/bin/sh
# Bounded standalone GNSS using only this phone's original ARM32 runtime.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
test "$(id -u)" = 0 || { echo 'Use root SSH for GNSS administration.' >&2; exit 77; }
umask 077
runtime=/run/native-gnss
root=$runtime/root
binary=/usr/local/libexec/native-gnss-run
system=/usr/local/lib/native-gnss/system
factory=/nvdata/APCFG/APRDEB/GPS
factory_hash=$(cat /etc/native-gnss-factory.sha256)
check_files() {
    test -f /etc/native-installed
    test -x "$binary"
    test -c /dev/gps
    test "$(cat /sys/class/stpgps/stpgps/dev)" = 191:0
    test "$(stat -c '%t:%T' /dev/gps)" = e8:0
    test -w /sys/class/gpsdrv/gps/pwrctl
    test "$(sha256sum "$factory" | cut -d' ' -f1)" = "$factory_hash"
    (cd / && sha256sum -c /usr/local/share/native-gnss/SHA256SUMS >/dev/null)
}
case "${1:-}" in
    check)
        test "$#" = 1 || exit 64
        check_files
        echo 'GNSS_CHECK_OK original_runtime=arm32 factory=readonly default=off'
        exit 0
        ;;
    scan)
        test "$#" = 2 || exit 64
        case "$2" in ''|*[!0-9]*) exit 64;; esac
        test "${#2}" -le 3 && test "$2" -ge 10 && test "$2" -le 300 || exit 64
        seconds=$2
        ;;
    *) echo 'Usage: native-gnss check | native-gnss scan SECONDS (10-300)' >&2; exit 64;;
esac
exec 9>/run/native-gnss-control.lock
flock -n 9 || { echo 'Another GNSS session is running.' >&2; exit 73; }
check_files
test ! -L "$runtime"
test ! -e "$root" || { echo 'GNSS runtime already exists; inspect it before retrying.' >&2; exit 73; }
test ! -e "$runtime/summary.log"
child=
mounted() { awk -v point="$1" '$2 == point { found=1 } END { exit !found }' /proc/mounts; }
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    cleanup_failed=0
    if test -n "$child"; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
        child=
    fi
    # The helper owns and reaps mnld before returning. This fallback only
    # powers down GPS if its ordinary stop sequence did not do so.
    if test "$(cat /sys/class/gpsdrv/gps/pwrctl)" != 0 || test "$(cat /sys/class/gpsdrv/gps/state)" != 0; then
        echo 'GNSS_POWER_OFF_FALLBACK'
        printf 0 >/sys/class/gpsdrv/gps/pwrctl || cleanup_failed=1
        printf 0 >/sys/class/gpsdrv/gps/state || cleanup_failed=1
    fi
    for point in "$root/sys/class/gpsdrv/gps" "$root/data/nvram/APCFG/APRDEB/GPS" "$root/system"; do
        if mounted "$point"; then umount "$point" || cleanup_failed=1; fi
    done
    # Never traverse a live bind mount while deleting this private /run root.
    if awk -v prefix="$root" '$2 == prefix || index($2,prefix "/") == 1 { found=1 } END { exit !found }' /proc/mounts; then
        echo 'GNSS_CLEANUP_REFUSED: runtime still has a mount.' >&2
        cleanup_failed=1
    elif test -d "$root"; then
        find "$root" -xdev -depth ! -type d -exec rm -f -- {} \; || cleanup_failed=1
        find "$root" -xdev -depth -type d -exec rmdir {} \; || cleanup_failed=1
    fi
    if test -f "$runtime/summary.log"; then
        cp "$runtime/summary.log" /var/log/native-gnss.log.new
        chmod 600 /var/log/native-gnss.log.new
        mv /var/log/native-gnss.log.new /var/log/native-gnss.log
        cat "$runtime/summary.log"
        rm "$runtime/summary.log"
    fi
    if test "$(cat /sys/class/gpsdrv/gps/pwrctl)" != 0 || test "$(cat /sys/class/gpsdrv/gps/state)" != 0 ||
       test "$(sha256sum "$factory" | cut -d' ' -f1)" != "$factory_hash"; then cleanup_failed=1; fi
    if test "$cleanup_failed" = 0; then
        if test -d "$runtime"; then rmdir "$runtime" || cleanup_failed=1; fi
        echo 'GNSS_RUNTIME_CLEANED power=0 state=0 calibration_unchanged=1'
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
mkdir -p "$root/system" "$root/dev/socket" "$root/sys/class/gpsdrv/gps" \
    "$root/data/nvram/APCFG/APRDEB" "$root/data/misc/gps" \
    "$root/data/agps_supl" "$root/data/gps_mnl" "$root/data/mpe_mnl" "$root/nvcfg"
mknod -m 600 "$root/dev/null" c 1 3
mknod -m 600 "$root/dev/zero" c 1 5
mknod -m 600 "$root/dev/urandom" c 1 9
mknod -m 600 "$root/dev/stpgps" c 191 0
mknod -m 600 "$root/dev/gps" c 232 0
mount --bind "$system" "$root/system"
mount -o remount,bind,ro "$root/system"
: >"$root/data/nvram/APCFG/APRDEB/GPS"
mount --bind "$factory" "$root/data/nvram/APCFG/APRDEB/GPS"
mount -o remount,bind,ro "$root/data/nvram/APCFG/APRDEB/GPS"
mount --bind /sys/class/gpsdrv/gps "$root/sys/class/gpsdrv/gps"
test "$(awk -v point="$root/data/nvram/APCFG/APRDEB/GPS" '$2 == point {print $4}' /proc/mounts | cut -d, -f1)" = ro
env -u LD_PRELOAD -u LD_LIBRARY_PATH "$binary" 2 "$seconds" "$root" >"$runtime/summary.log" 2>&1 9>&- &
child=$!
result=0
wait "$child" || result=$?
child=
exit "$result"
