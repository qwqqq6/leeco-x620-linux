# Privacy / 隐私

The public tree is built from an explicit source allowlist, not by uploading the working directory. The rootfs is rewritten from a verified private baseline plus the accepted keyboard patch.

公开文件来自明确选取的源码和重新生成的系统包，不直接上传开发目录或私人快照。

Removed / 已排除：

- All personal home directories, shell history, recent-file records, caches, dconf state, PulseAudio cookies and input learning/history.
- Codex API key, custom API endpoint/provider, account/session history and trusted-project configuration.
- Wi-Fi SSID/password profiles, SSH private/public authorization and host keys, Bluetooth pairing/link keys.
- Machine IDs, donor USB serial, runtime logs, server data, original partition dumps and calibration storage.
- The proprietary iFlytek client and Android hardware blobs. These are supplied separately by the recipient, not taken from the development handset.

First boot creates unique SSH host keys and machine identity after entropy initialization. The USB serial derives from the new machine identity. SSH authorization is injected from the recipient's public key during private preparation. There is no shared password or shared administrative private key.

首次启动在熵池初始化后生成独立的 SSH 主机密钥和机器标识；USB 序列号来自新标识。管理公钥由刷机者自己提供，没有通用密码或共享私钥。

`work/` contains original partition backups, recipient firmware and prepared device files. Keep it private and out of Git. Do not upload raw diagnostic logs or system dumps to issues. The repository's ignore file is a convenience, not a substitute for checking what you commit.

Optional voice dictation uses the vendor cloud. No real microphone recording was performed while creating or validating this release. A user starts and ends voice capture by holding/releasing space.

发布前检查了压缩包内部的每个普通文件、initramfs、二进制模板和公开源码；还在内存中用开发机的实际私有值比对，包括 API 密钥。扫描报告不保存这些值。自动扫描有局限，因此同时采用了目录排除、配置重建和明确的发布文件清单。
