#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mkdir -p /run/native-audio
chown root:audio /run/native-audio
chmod 775 /run/native-audio
touch /run/native-audio/stream.lock
chown root:audio /run/native-audio/stream.lock
chmod 660 /run/native-audio/stream.lock
# Built-in sound devices are present before mdev -s returns. Preserve the
# restrictive permissions and give only the audio group local access.
if test -d /dev/snd; then
    for node in /dev/snd/*; do
        test -c "$node" || continue
        chown root:audio "$node"
        chmod 660 "$node"
    done
fi
# The smart amplifier has its own dedicated I2C character device.
if test -c /dev/i2c_smartpa; then
    chown root:audio /dev/i2c_smartpa
    chmod 660 /dev/i2c_smartpa
fi
# This vendor class omits DEVNAME, so mdev does not create /dev/stpbt.
# Expose the actual registered character device for root-only diagnosis.
bt_dev=$(cat /sys/class/stpbt/stpbt/dev 2>/dev/null) || bt_dev=
case "$bt_dev" in
    ''|*[!0-9:]*|:*|*:) ;;
    *)
        major=${bt_dev%:*}
        minor=${bt_dev#*:}
        if test ! -e /dev/stpbt; then
            mknod -m 600 /dev/stpbt c "$major" "$minor"
        fi
        ;;
esac
