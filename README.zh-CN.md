# 乐视 X620 原生 Linux

简体中文 · [English](README.md)

把 **联发科乐视 Le X620（MT6797，已验证 32 GB 分区布局）** 改造成原生 Linux 手机和本地服务器：Alpine Linux 3.24.2 ARM64、Phosh 手机桌面、中文输入支持、USB/Wi-Fi 网络、SSH/SFTP 和网页服务。

系统完全替换 Android，PID 1 是 BusyBox，不运行 Android framework、Zygote 或 Android 应用。硬件仍依赖手机原厂的 **3.18.22+ 内核、DTB 和固件**。这是在一台实机上验证的实验性下游移植。

**禁止用于高通版 Le 2（X520/X522/X526/X527）、X625 或其他 MT6797 手机。** 安装器只接受已验证的分区大小、recovery 版本、内核/DTB 哈希和厂商文件哈希，其他 X620 变种也可能被拒绝。

## 下载与发布状态

从 [Releases](https://github.com/qwqqq6/leeco-x620-linux/releases) 下载 `leeco-x620-linux-…zip` 及 `SHA256SUMS`。这是需要在电脑上用 Python 和 recovery ADB 安装的**电脑辅助刷机包**，**不能**在 TWRP 的“安装 ZIP”界面直接刷入，也不是 Android OTA。

开发机原来的私人安装已经通过启动及功能验收。公开包删除了个人状态，并从待刷手机自己的原 Android 系统重建硬件依赖；这个新的干净安装流程**尚未进行清空实机重刷验收**，因此标为实验性预发布。没有仅为发布而清空开发机。

| 功能 | 开发机已经验证 | 当前限制 |
| --- | --- | --- |
| 显示、触摸、桌面 | 1080×1920 显示、准确触摸、Phosh 手机手势 | Xorg → Phoc/pixman 软件渲染，桌面没有 GPU 加速 |
| 刷新与息屏 | 改善面板刷新；电源键短按息屏、再按唤醒 | 息屏时系统和服务器继续运行，不进入整机休眠 |
| 网络和服务器 | USB RNDIS、Wi-Fi DHCP、SSH/SFTP、8080 网页 | Phosh 网络控制中心尚未完整接入，Wi-Fi 需手工配置 |
| 键盘唤起与收起 | 点输入框弹出；收起后再点同一框可重开 | 已验证 GTK3、GTK4、VTE，其他应用可能不同 |
| 中文与语音输入 | 可选讯飞官方 ARM64 2.0.51；九键/二十六键；空格长按录音、松手填入 | 需另取官方客户端；联网语音会把录音上传厂商 |
| 声音 | 顶部听筒、底部外放和麦克风采集 | 使用专用 WAV 工具，通用桌面音频路由尚未完整接通 |
| 蓝牙 | 扫描、数字比较配对、保存密钥及加密重连 | 使用自定义 BTstack 工具；A2DP/HID/PAN 与标准 BlueZ 界面未验收 |
| 传感器、灯光、振动 | 运动、光线、距离事件，指示灯和振动 | 通过专用工具使用，未完整接入桌面 |
| GPU/GNSS | 独立 GLES 渲染回读、GNSS NMEA 输出 | 桌面无 3D；尚未验收实际卫星定位 |
| 电池、相机、蜂窝 | 基本电池供电与充电 | 电量百分比不可靠；相机和蜂窝未验收；未做长期续航测试 |

## 安装

**安装会清空 system 与 userdata，包括内部存储。** 请先把自己的文件备份到别处。需要已解锁、型号及布局匹配的 X620，以及可以正常进入的 **TWRP 3.2.2-0** 和 recovery ADB。本包不负责解锁或安装 recovery。

本机曾在执行 `fastboot boot`、`fastboot reboot`、`fastboot continue` 后出现 bootloader 重新锁定和循环重启。**不要使用这些命令。** 用手机按键进入 TWRP，安装后使用 TWRP 的重启菜单。

1. 电脑安装 Python 3.11+ 和官方 [Android Platform Tools](https://developer.android.com/tools/releases/platform-tools)。不依赖第三方 Python 包；可在解压后的目录中建立虚拟环境。
2. 把发布 ZIP 解压到专用目录，核对 ZIP 的 SHA256。手机进入 TWRP，关闭 MTP，取消挂载 System 和 Data，保持 USB 连接。
3. 用 `adb devices` 查看自己的序列号。如没有管理密钥，先创建一对；只把 `.pub` 公钥交给安装器：

   ```sh
   ssh-keygen -t ed25519 -f x620-admin
   python scripts/flash.py --serial 你的序列号 inspect
   ```

4. 准备本机专用文件和私人备份。此步骤读取原 boot、system 和校准分区，保存到 `work/private-device/`，不格式化、不刷写。电脑至少预留 6 GB 空间。手机此时必须仍有匹配的原 Android system 和 boot，不能直接对已经安装 Linux 的手机运行。

   ```sh
   python scripts/flash.py --serial 你的序列号 prepare --ssh-public-key x620-admin.pub
   ```

   如需讯飞，先阅读[用户协议](https://srf.xunfei.cn/linux/help/agreement.html)和[隐私政策](https://srf.xunfei.cn/linux/help/private.html)，将[官方 ARM64 2.0.51 包](https://srf.xunfei.cn/sp1/com.iflytek.iflyime_2.0.51_arm64_kylin_sp1.deb)下载到当前目录，再用下面的命令**代替**上一条 prepare 命令：

   ```sh
   python scripts/flash.py --serial 你的序列号 prepare --ssh-public-key x620-admin.pub --iflytek-deb com.iflytek.iflyime_2.0.51_arm64_kylin_sp1.deb --accept-iflytek-terms
   ```

   安装器核对官方包的固定 SHA256，不执行其维护脚本。未选装时保留 libpinyin 回退输入；讯飞语音及讯飞九键转换不可用。

5. 保管好私人备份。检查 prepare 结果，确认清空时执行：

   ```sh
   python scripts/flash.py --serial 你的序列号 install --confirm-erase ERASE-X620
   ```

   程序在格式化前上传并核对全部文件；只写 system、userdata 和 boot，核对 boot 写后哈希和 recovery 未变化，**不会自动重启**。如安装失败，留在 TWRP 中恢复。
6. 成功后在手机选择 **TWRP → Reboot → System**，首次启动等待约 1–2 分钟。SSH 主机密钥和机器标识由手机现场生成。桌面账号没有密码锁，也没有 sudo 授权；root SSH 只接受你在准备步骤中提供的公钥。

## 使用

手机 USB 地址固定为 `192.168.92.1`，网页为 `http://192.168.92.1:8080/`。Windows 如未自动识别，可为新网络接口选择系统自带的 USB RNDIS/网络驱动。手机会向电脑提供 DHCP 地址。

```sh
ssh -i x620-admin root@192.168.92.1
native-screen status
native-screen off
native-screen on
```

把自己的 Wi-Fi 网络写入 `/etc/wpa_supplicant/wpa_supplicant.conf`，权限设为 `0600`，然后从 Linux 正常重启。不要公开该文件。Wi-Fi 地址由自己的路由器分配。网页目录是 `/srv/www`，其他服务器数据放在 `/srv/files`。息屏后 SSH 和网页继续运行。若插着 USB 关机会自动开机，先拔线再正常关机。

Phosh 从底部上滑回主页。点可编辑输入框唤起键盘；点「收起」隐藏，再点同一框重开，也保留长按底部横条的方式。选装讯飞后可切换九键/二十六键；短按空格选词或输入空格，长按录音、松手停止并填入。空格键留白，候选词完整换行显示。真实录音只由用户操作触发。

包内有官方 Codex CLI 0.163.0，**没有预设 API 密钥、API 地址、自定义服务商、账号会话或项目信任配置**。请在手机上配置自己的登录方式。旧内核无法提供通常的 Codex 文件系统沙箱，请使用普通桌面账号在专用项目中运行。

## 恢复、隐私与源码

- [恢复说明](docs/RECOVERY.md)：使用自己的原 boot/system 备份和保留的 TWRP；不能恢复其他手机的校准分区。
- [隐私与打包](docs/PRIVACY.md)：公开包不包含个人 HOME、API 配置、Wi-Fi 网络、SSH 密钥或蓝牙配对库；私人备份和工作目录不进 Git。
- [构建与验证](docs/BUILD.md)：文件清单、源码、许可与验证边界。
- [变更记录](docs/CHANGELOG.md)：整理后的开发和发布过程，不上传原始私人日志。
- [第三方声明](docs/THIRD_PARTY.md)：Alpine、Ubuntu、Codex、BTstack 各自遵循原许可；BTstack 有个人/非商业限制，整套镜像并非 MIT 授权。

本项目不是乐视、Alpine、postmarketOS、Purism 或讯飞官方发行版。部署服务时请考虑旧内核与尚未完成的硬件适配。
