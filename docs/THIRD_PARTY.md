# Third-party components / 第三方组件

The root `LICENSE` applies only to original project glue, documentation and installers. Each included package or pre-existing source file retains its original license. This image is **not entirely MIT-licensed**.

| Component | Source and license information |
| --- | --- |
| Alpine Linux userspace | [Alpine](https://alpinelinux.org/), [v3.24 packages](https://pkgs.alpinelinux.org/packages?arch=aarch64&branch=v3.24), [aports source/build recipes](https://gitlab.alpinelinux.org/alpine/aports). Exact versions, origins, licenses and recipe commits are in `manifest/alpine-packages.json`; recipe `source=` entries point to upstream corresponding source. |
| BusyBox and native initramfs tools | [BusyBox source](https://busybox.net/downloads/), GPL-2.0. The exact BusyBox version is in the Alpine package inventory. `port/native-final-initramfs.sh` contains the Linux startup script. |
| Phosh, Phoc, Squeekboard, Xorg, GTK, IBus, libpinyin and desktop packages | Included via the inventoried Alpine packages; retain their respective upstream licenses and package sources. |
| Isolated Ubuntu Focal ARM64 dependencies | [Ubuntu source archive](https://archive.ubuntu.com/ubuntu/pool/), [Ubuntu Ports binary archive](https://ports.ubuntu.com/ubuntu-ports/), [Launchpad source packages](https://launchpad.net/ubuntu/+source). Exact package/source versions are in `manifest/ubuntu-packages.json`; installed `/usr/share/doc/*/copyright` inside the isolated root is retained. |
| Codex CLI 0.163.0 | [OpenAI Codex source](https://github.com/openai/codex), [upstream release](https://github.com/openai/codex/releases/tag/rust-v0.163.0), Apache-2.0 and upstream notices. No maintainer API configuration or credentials included. |
| BTstack | [BTstack source](https://github.com/bluekitchen/btstack); upstream personal/noncommercial license in [LICENSE.btstack](LICENSE.btstack). Commercial use needs the appropriate upstream licensing. Project-specific transport glue is in `port/bluetooth/`. |
| Android/Bionic startup glue | Existing Android copyright and BSD-style notice remain in `port/bionic-start.c`; that file is not relicensed by the root MIT license. |
| Original kernel, DTBs and Android runtime/firmware | **Not distributed.** Extracted from the recipient's own matching device during private preparation. The project does not grant a license to those components. |
| TFA speaker DSP settings | **Not distributed.** Zeroed out of the public helper template; supplied from the recipient's original system, with exact hash checks. |
| Official iFlytek Linux client | **Not distributed.** Obtain from [the vendor](https://srf.xunfei.cn/); its [agreement](https://srf.xunfei.cn/linux/help/agreement.html) and [privacy policy](https://srf.xunfei.cn/linux/help/private.html) apply. The optional import accepts only official ARM64 2.0.51, SHA256 `ff09ec7c03b2133f07b88de4bb63329be7fc508138be9818c2ab1763f5308108`. |

Original package license/copyright files are preserved in the rootfs where supplied. Consult the exact inventory and package recipe/source version for rebuilding. Naming an upstream project does not imply its endorsement of this experimental port.
