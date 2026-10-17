# Packaging and validation / 打包与验证

This public release has two parts:

1. Git contains documentation, original port/installer sources, package inventories, required donor hashes and verification tools.
2. The release ZIP also contains `payload/rootfs.tar.gz`, a **kernel-free** `initramfs.cpio.gz`, and a speaker-helper template with vendor firmware regions zeroed. Payload files are release assets, not Git objects.

Host Python uses only its standard library. Run the installer through `python scripts/flash.py`; no pip packages or global environment modifications are required. When developing extra tools, use a project-local virtual environment.

The rootfs contains Alpine userspace and a separate Ubuntu Focal compatibility filesystem for the optional official ARM64 input client. `manifest/alpine-packages.json` lists package/version/license/origin/recipe commit; `manifest/ubuntu-packages.json` lists the isolated environment's package versions. Existing package copyright/license files are retained. Package mirrors, recipes and source locations are documented in [THIRD_PARTY.md](THIRD_PARTY.md).

During `prepare`, the host:

- Reads and checks original partition backups from recovery ADB.
- Requires an exact match of the tested original kernel plus appended DTBs.
- Extracts the reviewed vendor dependency closure from the recipient's mounted **read-only original Android system**, validating each source file against `manifest/vendor-files.json`.
- Restores the TFA helper's embedded settings from the recipient's original `/system/etc/tfa9890/` files. `manifest/tfa-template.json` describes each zeroed region and the final expected helper hash; no factory calibration or vendor DSP patch is distributed in the template.
- Rebuilds a boot image locally using the recipient's boot header and original kernel/DTBs, with the published Linux initramfs. The resulting boot image and vendor overlay stay in the private work directory.

The saved `serial_sha256`, original partition hashes, local backup hashes, upload hashes, boot readback and unchanged recovery bind the later install to that preparation. The template and donor hashes describe one tested configuration; porting to other firmware requires actual hardware testing, not removing the checks.

## Verify a release

After extracting the ZIP:

```sh
python -B scripts/verify_release.py
python -B -m unittest discover -s tests -v
```

Verification reads the compressed rootfs and checks every listed file's hash, type, ownership and mode, package asset hashes, blank API/Wi-Fi settings, locked account passwords, absence of personal files and recipient identity initialization. A separate private check used the actual source phone's sensitive values **in memory only**; those values and the original raw logs are not public.

Validation also includes offline reconstruction of the original tested boot kernel/ramdisk combination and the speaker helper, syntax checks and non-recording runtime checks on the already-working phone. These checks do not constitute a clean-install test. See `manifest/validation.json` for the checks actually completed.

## Rebuilding and limitations

The `port/` tree contains the handset adapters, input bridge, mobile session, panel refresh and screen controller sources used by the port. `port/` preserves original subdirectories; it is not an automated generic distribution build system. Package recipes provide the distribution source/build configuration. The speaker helper requires the recipient's original settings to generate its header, and the boot requires the matching original kernel/DTBs. Private snapshots, firmware backups, network settings and the maintainer's environment are intentionally absent.

这次发布提供可检查的移植源码、安装程序及包清单，但不把个人完整快照或厂商闭源资源当作公开源码。公开版的新安装流程尚未清空实机验收；不能将开发机已有功能验收写成公开版已完成通刷测试。
