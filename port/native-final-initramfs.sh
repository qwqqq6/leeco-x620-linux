#!/bin/busybox sh
export PATH=/bin:/sbin
/bin/busybox --install -s /bin
fail_recovery() {
    echo 'Linux root unavailable; returning to recovery in 10 seconds'
    sleep 10
    /bin/reboot-recovery
    while :; do sleep 60; done
}
mkdir -p /proc /sys /dev /newroot
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs -o mode=0755 tmpfs /dev
mknod -m 600 /dev/console c 5 1
mknod -m 666 /dev/null c 1 3
exec </dev/console >/dev/console 2>&1
echo /bin/mdev >/proc/sys/kernel/hotplug
mdev -s
for attempt in $(seq 1 15); do
    test -b /dev/mmcblk0p36 && break
    sleep 1
    mdev -s
done
mount -t ext4 -o rw,noatime /dev/mmcblk0p36 /newroot || fail_recovery
test -f /newroot/etc/native-installed || fail_recovery
mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/var/log
echo 'Mount system partition as native Linux root' >>/newroot/var/log/native-initramfs.log
mount --move /dev /newroot/dev || fail_recovery
mount --move /proc /newroot/proc || fail_recovery
mount --move /sys /newroot/sys || fail_recovery
exec /bin/busybox switch_root -c /dev/console /newroot /sbin/init
