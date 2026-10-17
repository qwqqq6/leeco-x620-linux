# LeEco X620 native Linux

[简体中文](README.zh-CN.md) · English

Turn the **MediaTek LeEco Le X620 (MT6797, tested 32 GB layout)** into a native Linux phone and local server: Alpine Linux 3.24.2 ARM64, a Phosh mobile desktop, Chinese input support, USB/Wi-Fi networking, SSH/SFTP and a small web server.

Android is replaced. BusyBox is PID 1; Android framework, Zygote and Android applications do not run. Hardware support still depends on the handset's original **3.18.22+ vendor kernel, DTB and firmware**. This is an experimental downstream port, with tests on one handset.

**Do not flash this onto Qualcomm Le 2 models (X520/X522/X526/X527), X625, or another MT6797 phone.** The installer accepts only the tested partition sizes, recovery version, kernel/DTB hash and vendor file hashes. Other X620 variants may be rejected.

## Downloads and status

Download the `leeco-x620-linux-…zip` asset and its `SHA256SUMS` from [Releases](https://github.com/qwqqq6/leeco-x620-linux/releases). This is a **host-assisted flashing package**, used with Python and recovery ADB on a computer. It is **not** an Android OTA or a ZIP that TWRP's Install button can apply.

The original private installation was booted and tested. The redistributed package removes personal state and reconstructs hardware dependencies from the recipient's own Android installation. That new clean-install path has **not** been destructively flash-tested; this release is marked experimental/pre-release. No destructive reflash was performed merely to produce the release.

| Component | Verified on the development handset | Limits |
| --- | --- | --- |
| Display and touch | 1080×1920 display; accurate touch; Phosh mobile gestures | Xorg → Phoc/pixman software rendering; no desktop GPU acceleration |
| Refresh and screen off | Improved panel refresh; short power press turns panel off/on | Screen off keeps Linux and servers running; no system suspend |
| Networking and services | USB RNDIS, Wi-Fi DHCP, SSH/SFTP, HTTP :8080 | Phosh network controls are not fully integrated; configure Wi-Fi manually |
| Keyboard | Tap an input field to show; dismiss; tap the same field to reopen | Verified with GTK3, GTK4 and VTE; other apps may differ |
| Chinese input and dictation | Optional official iFlytek 2.0.51 ARM64; 9-key/26-key; hold space to dictate, release to insert | Obtain the official client separately; online voice sends audio to the vendor |
| Audio | Earpiece and bottom speaker playback; microphone capture | Dedicated WAV helpers; general desktop audio routing is incomplete |
| Bluetooth | Scan, numeric-comparison pairing, saved encrypted reconnection | Custom BTstack tools; A2DP/HID/PAN and normal BlueZ UI not accepted |
| Sensors, LEDs and vibration | Events from motion/light/proximity sensors; LEDs and vibration | Dedicated tools rather than full desktop integration |
| GPU/GNSS | Isolated GLES render/readback; GNSS produces NMEA | No desktop 3D; no satellite fix validated |
| Battery, camera and cellular | Basic battery operation and charging | Battery percentage is unreliable; camera/cellular unverified; no long-term endurance test |

## Install

**Installation erases system and userdata, including internal storage.** Back up your files elsewhere. Use an unlocked, matching X620 with a working **TWRP 3.2.2-0** and recovery ADB. The package does not unlock a bootloader or install recovery.

On this handset, `fastboot boot`, `fastboot reboot` and `fastboot continue` triggered bootloader relocking and boot loops. **Do not use those commands.** Enter TWRP with the handset keys; use TWRP's Reboot menu when instructed.

1. Install Python 3.11+ and the official [Android Platform Tools](https://developer.android.com/tools/releases/platform-tools) on your computer. No third-party Python packages are required; you may create a virtual environment in the extracted package directory.
2. Extract the release ZIP into a dedicated directory. Check the published ZIP SHA256. In TWRP, disable MTP and unmount System and Data. Leave USB connected.
3. Find your serial with `adb devices`. Create an SSH key for **your** phone if needed:

   ```sh
   ssh-keygen -t ed25519 -f x620-admin
   python scripts/flash.py --serial YOUR_SERIAL inspect
   ```

4. Prepare a device-specific package and private backups. This reads original boot/system/calibration partitions and saves them under `work/private-device/`; it does not format or flash. Allow at least 6 GB of computer disk space. The phone must still contain the matching original Android system and boot. Do not point this at an already-installed Linux system.

   ```sh
   python scripts/flash.py --serial YOUR_SERIAL prepare --ssh-public-key x620-admin.pub
   ```

   Optional iFlytek: read the [vendor agreement](https://srf.xunfei.cn/linux/help/agreement.html) and [privacy policy](https://srf.xunfei.cn/linux/help/private.html), download the [official ARM64 2.0.51 package](https://srf.xunfei.cn/sp1/com.iflytek.iflyime_2.0.51_arm64_kylin_sp1.deb) into this directory, and use this **instead** of the preceding prepare command:

   ```sh
   python scripts/flash.py --serial YOUR_SERIAL prepare --ssh-public-key x620-admin.pub --iflytek-deb com.iflytek.iflyime_2.0.51_arm64_kylin_sp1.deb --accept-iflytek-terms
   ```

   The exact package SHA256 is checked. Its maintainer scripts are not executed. Without this optional package, the existing libpinyin fallback remains available; vendor dictation and vendor 9-key conversion are unavailable.

5. Keep the private backups. Read the preparation output, then explicitly request the erase/install:

   ```sh
   python scripts/flash.py --serial YOUR_SERIAL install --confirm-erase ERASE-X620
   ```

   The installer uploads and checks all assets before formatting. It writes only system, userdata and boot, checks boot readback and that recovery is unchanged, and **does not reboot automatically**. A failed install must remain in TWRP for recovery.
6. On success, use **TWRP → Reboot → System**. Give the first boot 1–2 minutes. SSH host keys and the machine identity are generated on the phone. The desktop account has no password lock and no sudo grant; root SSH accepts only the public key you supplied.

## Use

USB phone address: `192.168.92.1`; HTTP: `http://192.168.92.1:8080/`. On Windows, the RNDIS interface may require selecting the system's USB RNDIS/network driver. The phone offers a DHCP lease to the computer.

```sh
ssh -i x620-admin root@192.168.92.1
native-screen status
native-screen off
native-screen on
```

Configure your own Wi-Fi network in `/etc/wpa_supplicant/wpa_supplicant.conf` with mode `0600`, then reboot from Linux. Do not publish that file. The assigned Wi-Fi address depends on your router. Put web files in `/srv/www` and other server data in `/srv/files`. Screen off does not stop SSH or HTTP. Unplug USB before normal shutdown if external power makes the phone boot again.

Phosh: swipe up from the bottom to return home. Tap an editable field to show the keyboard; **收起** hides it. Tap the same field again to reopen, or hold the bottom bar. With the optional iFlytek client, choose 9/26 keys, tap space to commit/space, or hold space for voice and release to stop and insert. The space key intentionally has no label; candidates wrap to show their text in full. Microphone recording starts only from a user gesture.

The official Codex CLI 0.163.0 is included. **No API key, API endpoint, custom provider, account/session or trusted project is configured.** Configure your own login on the handset. The old kernel does not provide the normal Codex filesystem sandbox; run it as the unprivileged desktop user in a dedicated project.

## Recovery, privacy and source

- [Recovery instructions](docs/RECOVERY.md): use your own verified original boot/system backup and retained TWRP. Never restore somebody else's calibration partitions.
- [Privacy and packaging](docs/PRIVACY.md): no personal home, API settings, Wi-Fi profile, SSH keys or Bluetooth pairing database is distributed. Private backups/work directories are excluded from Git.
- [Build and verification](docs/BUILD.md): package manifests, source inventory, licensing and validation limits.
- [Change log](docs/CHANGELOG.md): development and release milestones without raw private logs.
- [Third-party notices](docs/THIRD_PARTY.md): Alpine/Ubuntu/Codex and BTstack retain their own licenses. BTstack has personal/noncommercial terms; do not assume the whole image is MIT-licensed.

This project is not an official LeEco, Alpine, postmarketOS, Purism or iFlytek release. Keep the old kernel and incomplete drivers in mind when deciding which services to expose.
