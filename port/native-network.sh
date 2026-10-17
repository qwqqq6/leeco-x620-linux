#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB=/sys/class/android_usb/android0
if test -d "$USB"; then
    echo 0 >"$USB/enable"
    echo 18d1 >"$USB/idVendor"
    echo 4ee3 >"$USB/idProduct"
    cut -c1-16 /etc/machine-id >"$USB/iSerial"
    echo Linux >"$USB/iManufacturer"
    echo X620-Linux >"$USB/iProduct"
    # Bring up RNDIS independently: a missing FunctionFS descriptor must not
    # prevent the management network from enumerating.
    test ! -f "$USB/f_rndis/wceis" || echo 1 >"$USB/f_rndis/wceis"
    echo rndis >"$USB/functions"
    echo 1 >"$USB/enable"
    sleep 2
    if ifconfig rndis0 192.168.92.1 netmask 255.255.255.0 up; then
        udhcpd /etc/udhcpd.conf
    fi
fi
/usr/local/sbin/native-wifi
date -u
ip addr
ip route
dmesg >/var/log/native-kernel.log
