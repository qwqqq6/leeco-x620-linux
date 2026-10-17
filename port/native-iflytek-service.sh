#!/bin/sh
# Root supervisor. Backend clients drop to desktop before launching.
set -eu
[ -f /etc/native-mobile/iflytek-installed ] || exit 0
child=
cleanup() {
    trap - EXIT INT TERM
    [ -z "$child" ] || kill "$child" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 0' INT TERM
while :; do
    python3 /usr/local/libexec/native-iflytek-backend.py --service &
    child=$!
    wait "$child" || true
    child=
    sleep 3 &
    child=$!
    wait "$child" || true
    child=
done
