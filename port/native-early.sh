#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
hostname le620
mkdir -p /run /tmp /var/log /dev/pts /dev/shm /dev/usb-ffs/adb /dev/socket
mount -t tmpfs -o mode=0755 tmpfs /run
mount -t tmpfs -o mode=1777 tmpfs /tmp
mount -t devpts -o mode=0620,ptmxmode=0666 devpts /dev/pts
mount -t tmpfs -o mode=1777 tmpfs /dev/shm
ln -sf /proc/self/fd /dev/fd
ln -sf /proc/self/fd/0 /dev/stdin
ln -sf /proc/self/fd/1 /dev/stdout
ln -sf /proc/self/fd/2 /dev/stderr
ln -sf pts/ptmx /dev/ptmx
ln -sf /proc/mounts /etc/mtab
mdev -s
if test -x /usr/local/sbin/native-hardware-init; then
    /usr/local/sbin/native-hardware-init >>/var/log/native-hardware-init.log 2>&1
fi
# mdev defaults to root:root 0660; normal desktop users need the standard
# public character devices. This vendor kernel also uses legacy devpts
# without a pts/ptmx node, so it needs a real /dev/ptmx character device.
chmod 666 /dev/null /dev/zero /dev/random /dev/urandom /dev/tty
if test ! -e /dev/pts/ptmx; then
    rm -f /dev/ptmx
    mknod -m 666 /dev/ptmx c 5 2
fi
ifconfig lo 127.0.0.1 up
# This 3.18 kernel has no exposed hardware RNG. SSH otherwise blocks until
# enough input/interrupt entropy arrives. Seed the kernel before listeners.
/usr/sbin/haveged -w 1024 >>/var/log/haveged.log 2>&1
for attempt in $(seq 1 10); do
    test "$(cat /proc/sys/kernel/random/entropy_avail)" -ge 256 && break
    sleep 1
done
# On old kernels, pull input-pool entropy into the urandom pool before a
# getrandom() caller sleeps waiting for its initialization flag.
dd if=/dev/urandom of=/dev/null bs=64 count=1 2>/dev/null
if test -f /etc/native-installed; then
    mkdir -p /srv
    mount -t ext4 -o rw,noatime /dev/mmcblk0p38 /srv
fi
{
    echo 'Native Alpine Linux boot'
    date -u
    uname -a
    echo 'PID 1 executable:'
    readlink /proc/1/exe
    echo 'PID 1 command:'
    tr '\000' ' ' </proc/1/cmdline
    echo
    cat /etc/alpine-release
} >>/var/log/native-boot.log 2>&1
if test -x /usr/local/sbin/native-boot-mode; then
    /usr/local/sbin/native-boot-mode >>/var/log/native-boot-mode.log 2>&1
fi
/usr/local/sbin/native-release-init
/usr/local/sbin/native-network >>/var/log/native-network.log 2>&1 &
if test -f /etc/native-probe; then
    /usr/local/sbin/native-rollback-watchdog >>/var/log/native-watchdog.log 2>&1 &
fi
if test -f /etc/native-install-probe; then
    /usr/local/sbin/native-install-watchdog >>/var/log/native-watchdog.log 2>&1 &
fi
exit 0
