# Change log / 变更记录

## v0.1.0-experimental — 2026-10-17

First public experimental package. The development handset's private build had the following milestones:

- 2026-10-15: researched model-specific support; booted native Alpine using the original MT6797 kernel; installed Linux into system/userdata, preserved TWRP and verified network services after reboot.
- 2026-10-16: added Chinese Xfce desktop, accurate touch, USB/network startup recovery, earpiece/microphone helpers, indicators/vibration, sensor readers and bottom speaker adaptation.
- 2026-10-17: verified Bluetooth pairing/reconnection, isolated GPU probe and GNSS software path; improved panel refresh and power-button screen off/on while retaining server access.
- 2026-10-17: switched to Phosh/Phoc mobile desktop, added Codex CLI and the optional official iFlytek input bridge, 9/26-key layouts and hold-space/release dictation; removed the desktop input-method status bar, expanded candidate text and fixed keyboard dismiss/reopen on input taps.
- Release packaging: stripped personal accounts' files and all API/Wi-Fi/SSH/Bluetooth state; reset API configuration; removed original kernel/vendor runtime and proprietary input client from distribution. Added device-bound backup/preparation, hash-checked reconstruction, bilingual READMEs and public verification evidence.

已验证的开发机功能与尚未完成的硬件见 README。公开包的干净安装流程未进行清空重刷验收；相机、蜂窝、通用音频、桌面 3D、卫星定位和长期续航不能标为已完成。
