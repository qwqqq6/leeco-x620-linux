#!/bin/sh
# The bootloader starts this full Linux rootfs even in KPOC mode. In that
# mode the vendor kernel disables RNDIS and powers off when USB is removed.
# A normal Linux reboot has been verified to return this X620 to mode 0.
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mode_file=/sys/class/BOOT/BOOT/boot/boot_mode
guard=/var/lib/native/charger-reboot.guard
mkdir -p /var/lib/native
mode=$(cat "$mode_file" 2>/dev/null) || mode=unknown
echo "$(date -Is) boot_mode=$mode"
case "$mode" in
    0)
        rm -f "$guard"
        ;;
    8)
        if test -e "$guard"; then
            echo 'Charger reboot already attempted; keeping system accessible and avoiding a reboot loop.'
            exit 0
        fi
        {
            date -Is
            cat /proc/sys/kernel/random/boot_id
            echo 'One normal Linux reboot requested from KPOC mode 8.'
        } >"$guard" || exit 1
        sync
        echo 'Scheduling one normal reboot to leave charger mode.'
        nohup sh -c 'sleep 3; sync; reboot' </dev/null >>/var/log/native-boot-mode.log 2>&1 &
        ;;
    9)
        echo 'Low-power charging mode: automatic reboot deferred; no charging limits changed.'
        ;;
    *)
        echo 'Unrecognized or maintenance boot mode; leaving it unchanged.'
        ;;
esac
