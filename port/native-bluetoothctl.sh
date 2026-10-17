#!/bin/sh
# Root-only control of the native STP stack; no Android or BlueZ dependency.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
test "$(id -u)" = 0 || { echo 'Use root SSH for Bluetooth administration.' >&2; exit 77; }
umask 077
runtime=/run/native-bluetooth
binary=/usr/local/libexec/native-bluetooth
log=/var/log/native-bluetooth.log
mkdir -p "$runtime"
exec 9>/run/native-bluetooth-control.lock
flock -n 9 || { echo 'Another Bluetooth control command is running.' >&2; exit 73; }
get_pid() {
    test -r "$runtime/pid" || return 1
    task_pid=$(cat "$runtime/pid")
    case "$task_pid" in ''|*[!0-9]*) return 1;; esac
    test "$(readlink "/proc/$task_pid/exe" 2>/dev/null)" = "$binary" && kill -0 "$task_pid" 2>/dev/null
}
case "${1:-}" in
    status)
        if get_pid; then echo "RUNNING pid=$task_pid"; else echo STOPPED; fi
        if test -f "$runtime/request"; then
            printf 'PENDING_NUMERIC_COMPARISON '; cat "$runtime/request"
        fi
        if test -f "$log"; then tail -n 16 "$log"; fi
        ;;
    pair|serve)
        test "$#" = 1 || exit 64
        if get_pid; then echo 'Bluetooth is already running; stop it before changing mode.' >&2; exit 73; fi
        if test -f "$log"; then cp "$log" "$log.previous"; fi
        nohup env -u LD_PRELOAD "$binary" "--$1" >"$log" 2>&1 9>&- </dev/null &
        for attempt in 1 2 3 4 5 6 7 8 9 10; do
            if get_pid && grep -q CONTROLLER_READY "$log"; then
                tail -n 8 "$log"
                exit 0
            fi
            sleep 1
        done
        tail -n 20 "$log"
        echo 'Bluetooth did not become ready; inspect the log.' >&2
        exit 1
        ;;
    confirm)
        test "$#" = 2 || exit 64
        case "$2" in *[!0-9]*|'') exit 64;; esac
        test "${#2}" = 6 || exit 64
        get_pid && test -f "$runtime/request" || { echo 'No active numeric request.' >&2; exit 1; }
        test "$(cat "$runtime/request")" = "$2" || { echo 'The digits do not match.' >&2; exit 1; }
        printf '%s\n' "$2" >"$runtime/accept.new"
        mv "$runtime/accept.new" "$runtime/accept"
        echo 'Matching numeric confirmation submitted.'
        ;;
    deny)
        get_pid && test -f "$runtime/request" || exit 1
        : >"$runtime/deny"
        ;;
    stop)
        if get_pid; then
            kill -TERM "$task_pid"
            for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
                if ! get_pid; then echo STOPPED; exit 0; fi
                sleep 1
            done
            echo 'Still stopping; no forced kill was sent.' >&2
            exit 1
        fi
        echo STOPPED
        ;;
    check|reconnect)
        test "$#" = 1 || exit 64
        if get_pid; then echo 'Stop the running service before this check.' >&2; exit 73; fi
        exec env -u LD_PRELOAD "$binary" "--$1"
        ;;
    *) echo 'Usage (root): native-bluetoothctl status|pair|confirm SIX_DIGITS|deny|serve|stop|check|reconnect' >&2; exit 64;;
esac
