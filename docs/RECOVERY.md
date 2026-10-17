# Recovery / 恢复

Keep TWRP and your own verified backups before installing. This installer never writes recovery, bootloader, partition tables or calibration partitions. It writes only boot, system and userdata after explicit erase confirmation.

安装前保留可以进入的 TWRP 和自己的已校验备份。安装器只写 boot、system、userdata，不写 recovery、bootloader、分区表或校准分区。

If Linux fails to boot, use the phone's hardware keys to enter recovery. On the development handset: hold power + volume up; after the LeEco logo release power and continue holding volume up. If a boot selection menu appears, select Recovery Mode with its indicated keys. Do not experiment with `fastboot boot`, `fastboot reboot` or `fastboot continue` on this device.

Linux 启动失败时，用手机按键回 recovery：按住电源和音量上，出现乐视标志后松开电源、继续按音量上；如出现选择菜单，按菜单提示进入 Recovery Mode。不要使用上述 fastboot 启动命令。

For a stock restore, use only the **same phone's** `work/private-device/backup/boot.img` and `system.img`, with hashes checked against its `manifest.json`. Push them into recovery RAM or use a TWRP-supported external destination; never store the only copy on the userdata partition that will be formatted. In TWRP, ensure the target partition is unmounted, write the matching image to the verified boot/system block device, sync, and read back the **whole partition** SHA256. Format userdata for Android as required by its original ROM, then reboot using the TWRP menu.

恢复原系统只能使用同一台手机自己的 boot/system 备份，先核对备份清单；保证目标未挂载，按本机实际分区映射恢复后读取整个分区 SHA256 复核。按照原 Android ROM 要求格式化 userdata，最后用 TWRP 菜单重启。恢复会再次清空 Linux 服务器数据。

No general-purpose restore script is supplied: choosing the correct original ROM and data formatting requires the matching device backup. Never write somebody else's nvram, nvdata, persist, protect, seccfg or other calibration/security storage.

Do not erase your computer backups after the first successful boot. If preparation or upload fails before formatting, the installer leaves the original system intact. If a failure occurs after formatting starts, remain in TWRP and restore; the installer deliberately does not automatically retry, format again, or reboot a partially installed system.
