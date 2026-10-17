#!/usr/bin/env python3
"""Host-assisted X620 installer. Inspection is read-only; no fastboot is used."""
from __future__ import annotations
import argparse
import base64
import hashlib
import io
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import time
from pathlib import Path, PurePosixPath

from bootimage import parse_boot, read_ramdisk, rebuild

BASE = Path(__file__).resolve().parents[1]
BLOCK = '/dev/block/platform/mtk-msdc.0/11230000.msdc0/by-name'
PARTS = {'recovery': ('mmcblk0p1', 16777216), 'boot': ('mmcblk0p31', 16777216),
         'system': ('mmcblk0p36', 4294967296), 'userdata': ('mmcblk0p38', None)}
FEATURES = 'none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,uninit_bg,dir_nlink,extra_isize'
IFLYTEK_SHA = 'ff09ec7c03b2133f07b88de4bb63329be7fc508138be9818c2ab1763f5308108'


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def safe_relative(name):
    if '\\' in name or '\x00' in name:
        raise ValueError('Invalid archive path')
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('Archive path escapes its root')
    return path.as_posix().removeprefix('./')


def validate_public_key(key):
    if len(key.splitlines()) != 1 or not re.fullmatch(r'ssh-ed25519 [A-Za-z0-9+/=]+(?: [^\r\n]*)?', key):
        raise RuntimeError('Provide a single Ed25519 PUBLIC key, never its private key')
    try:
        blob = base64.b64decode(key.split()[1], validate=True)
    except ValueError as error:
        raise RuntimeError('Malformed Ed25519 public key') from error
    if len(blob) != 51 or blob[:19] != struct.pack('>I', 11) + b'ssh-ed25519' + struct.pack('>I', 32):
        raise RuntimeError('Malformed Ed25519 public key')
    return key


class Device:
    def __init__(self, adb, serial):
        self.command = [adb, '-s', serial]
        self.serial_hash = hashlib.sha256(serial.encode()).hexdigest()

    def run(self, args, *, data=None, output=None, timeout=60):
        result = subprocess.run(self.command + args, input=data, stdout=output or subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout)
        if result.returncode:
            # Do not print arbitrary device output: it may contain private settings.
            raise RuntimeError('ADB operation failed: ' + args[0])
        return result.stdout if output is None else b''

    def shell(self, code, timeout=60):
        return self.run(['exec-out', 'sh', '-c', shlex.quote('set -eu\n' + code)], timeout=timeout)

    def pull_verified(self, source, target):
        expected = self.shell('sha256sum ' + shlex.quote(source)).decode().split()[0]
        if not re.fullmatch('[0-9a-f]{64}', expected):
            raise RuntimeError('Invalid device checksum')
        target = Path(target)
        if target.exists():
            if digest(target) != expected:
                raise RuntimeError('Existing private backup differs; select a fresh work directory')
            return expected
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + '.partial')
        with partial.open('xb') as output:
            self.run(['exec-out', 'cat', source], output=output, timeout=1200)
        after = self.shell('sha256sum ' + shlex.quote(source)).decode().split()[0]
        if digest(partial) != expected or after != expected:
            raise RuntimeError('Backup transfer/readback checksum mismatch')
        partial.rename(target)
        return expected


def validate_layout(layout):
    for name, (suffix, expected) in PARTS.items():
        item = layout[name]
        if item['path'] not in ['/dev/block/' + suffix, '/dev/' + suffix]:
            raise RuntimeError('Unsupported partition mapping: ' + name)
        size = item['bytes']
        if expected is not None and size != expected:
            raise RuntimeError('Unsupported partition size: ' + name)
        if name == 'userdata' and not 24 * 1024**3 <= size < 25 * 1024**3:
            raise RuntimeError('Only the tested 32 GB X620 layout is supported')


def inspect(device):
    if not device.shell('id -u').strip() == b'0':
        raise RuntimeError('Root TWRP recovery ADB is required')
    version = device.shell('getprop ro.twrp.version').decode().strip()
    if version != '3.2.2-0':
        raise RuntimeError('Only verified TWRP 3.2.2-0 is supported')
    layout = {}
    for name in PARTS:
        path = device.shell('readlink -f ' + BLOCK + '/' + name).decode().strip()
        if not re.fullmatch(r'/dev/(?:block/)?mmcblk0p[0-9]+', path):
            raise RuntimeError('Invalid block device')
        layout[name] = {'path': path, 'bytes': int(device.shell('blockdev --getsize64 ' + path))}
    validate_layout(layout)
    for binary in ['mke2fs', 'mount', 'umount', 'tar', 'sha256sum', 'blockdev', 'dd', 'chroot']:
        device.shell('command -v ' + binary + ' >/dev/null')
    return {'twrp': version, 'partitions': layout, 'serial_sha256': device.serial_hash}


def verify_bundle():
    manifest = json.loads((BASE / 'manifest/release.json').read_text(encoding='utf-8'))
    for name, expected in manifest['assets'].items():
        name = safe_relative(name)
        path = BASE / name
        if path.stat().st_size != expected['bytes'] or digest(path) != expected['sha256']:
            raise RuntimeError('Release checksum mismatch: ' + name)
    return manifest


def backup(device, work, info):
    report = {}
    # Full Android system backup is needed to recover after the two formats.
    # Calibration partitions are read only, and are NEVER written by this tool.
    for name in ['boot', 'recovery', 'system', 'nvram', 'nvdata', 'nvcfg', 'protect1', 'protect2', 'persist', 'seccfg']:
        path = device.shell('readlink -f ' + BLOCK + '/' + name).decode().strip()
        if not re.fullmatch(r'/dev/(?:block/)?mmcblk0p[0-9]+', path):
            raise RuntimeError('Missing backup partition: ' + name)
        report[name] = {'sha256': device.pull_verified(path, work / 'backup' / (name + '.img')),
                        'bytes': (work / 'backup' / (name + '.img')).stat().st_size}
        print('Verified private backup:', name, flush=True)
    (work / 'backup/manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def original_system(device, info):
    # Mount the source ourselves, read-only, rather than trusting a writable GUI mount.
    mounts = device.shell('cat /proc/mounts').decode().splitlines()
    source = info['partitions']['system']['path']
    for row in mounts:
        fields = row.split()
        if fields[0] in [source, BLOCK + '/system']:
            if 'ro' not in fields[3].split(','):
                raise RuntimeError('Unmount System in TWRP before preparation (read-only source required)')
            mount = fields[1]
            break
    else:
        mount = '/tmp/le620-release-source'
        device.shell('mkdir -p ' + mount + '\nmount -t ext4 -o ro ' + source + ' ' + mount)
    nested = device.shell('if test -f ' + mount + '/etc/native-installed; then echo native; else echo android; fi').strip()
    if nested == b'native':
        raise RuntimeError('Use an original Android system backup as the donor; native-to-native installs are not supported')
    return mount


def ar_payload(path):
    data = path.read_bytes()
    if data[:8] != b'!<arch>\n':
        raise ValueError('Invalid Debian package')
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Truncated Debian package')
        size = int(header[48:58])
        chunk = data[offset + 60:offset + 60 + size]
        if len(chunk) != size:
            raise ValueError('Truncated Debian payload')
        if header[:16].strip().startswith(b'data.tar'):
            return chunk
        offset += 60 + size + size % 2
    raise ValueError('No Debian payload')


def calibration_hash(device):
    source = device.shell('readlink -f ' + BLOCK + '/nvdata').decode().strip()
    if source not in ['/dev/block/mmcblk0p14', '/dev/mmcblk0p14']:
        raise RuntimeError('Unsupported calibration partition mapping')
    owned_mount = None
    mount = None
    for row in device.shell('cat /proc/mounts').decode().splitlines():
        fields = row.split()
        if fields[0] in [source, BLOCK + '/nvdata']:
            if 'ro' not in fields[3].split(','):
                raise RuntimeError('Calibration storage must be unmounted or read-only')
            mount = fields[1]
            break
    if mount is None:
        mount = owned_mount = '/tmp/le620-release-calibration'
        device.shell('mkdir -p ' + mount + '\nmount -t ext4 -o ro ' + source + ' ' + mount)
    try:
        result = device.shell('sha256sum ' + shlex.quote(mount + '/APCFG/APRDEB/GPS')).decode().split()[0]
        if not re.fullmatch('[0-9a-f]{64}', result):
            raise RuntimeError('Invalid recipient GNSS calibration checksum')
        return result
    finally:
        if owned_mount:
            device.shell('umount ' + owned_mount)


def add_bytes(archive, name, data, mode=0o644, uid=0, gid=0):
    item = tarfile.TarInfo(safe_relative(name))
    item.size, item.mode, item.uid, item.gid = len(data), mode, uid, gid
    archive.addfile(item, io.BytesIO(data))


def tfa_reconstruct(device, mount, config):
    template = bytearray((BASE / 'payload/native-tfa-template.bin').read_bytes())
    for region in config['regions']:
        content = device.shell('cat ' + shlex.quote(mount + '/etc/tfa9890/' + region['source']))
        if hashlib.sha256(content).hexdigest() != region['source_sha256']:
            raise RuntimeError('Unsupported speaker settings; no write')
        if region.get('encoding') == 'eq-f32le':
            rows = [row.split() for row in content.decode('ascii').strip('\x00\r\n ').splitlines()]
            content = b''.join(struct.pack('<5f', *(float(x) for x in row[1:])) for row in rows)
        if len(content) != region['bytes']:
            raise RuntimeError('Speaker template length mismatch')
        start = region['offset']
        template[start:start + len(content)] = content
    result = bytes(template)
    if hashlib.sha256(result).hexdigest() != config['reconstructed_sha256']:
        raise RuntimeError('Speaker reconstruction checksum mismatch')
    return result


def prepare(device, work, args):
    bundle = verify_bundle()
    info = inspect(device)
    work.mkdir(parents=True, exist_ok=True)
    if (work / 'prepared.json').exists():
        raise RuntimeError('Work directory already prepared; choose a new one')
    backups = backup(device, work, info)
    original = (work / 'backup/boot.img').read_bytes()
    if hashlib.sha256(parse_boot(original)['kernel']).hexdigest() != bundle['tested_kernel_sha256']:
        raise RuntimeError('Unsupported original kernel/DTB; backups saved, no device write')
    mount = original_system(device, info)
    factory_hash = calibration_hash(device)
    vendor = json.loads((BASE / 'manifest/vendor-files.json').read_text(encoding='utf-8'))
    key = Path(args.ssh_public_key).read_text(encoding='ascii').strip()
    validate_public_key(key)
    optional = None
    if args.iflytek_deb:
        if not args.accept_iflytek_terms:
            raise RuntimeError('Read the vendor terms; use --accept-iflytek-terms only if you agree')
        package = Path(args.iflytek_deb)
        if digest(package) != IFLYTEK_SHA:
            raise RuntimeError('Unsupported official iFlytek package checksum')
        optional = ar_payload(package)
    overlay = work / 'device-overlay.tar.gz'
    with tarfile.open(overlay, 'x:gz') as archive:
        for entry in vendor:
            content = device.shell('cat ' + shlex.quote(mount + '/' + entry['source']))
            if len(content) != entry['bytes'] or hashlib.sha256(content).hexdigest() != entry['sha256']:
                raise RuntimeError('Unsupported donor firmware: ' + entry['source'])
            add_bytes(archive, entry['destination'], content, int(entry['mode'], 8))
        config = json.loads((BASE / 'manifest/tfa-template.json').read_text(encoding='utf-8'))
        add_bytes(archive, 'usr/local/lib/native-tfa/native-tfa', tfa_reconstruct(device, mount, config), 0o755)
        add_bytes(archive, 'root/.ssh/authorized_keys', (key + '\n').encode(), 0o600)
        add_bytes(archive, 'etc/native-gnss-factory.sha256', (factory_hash + '\n').encode(), 0o600)
        if optional:
            with tarfile.open(fileobj=io.BytesIO(optional), mode='r:*') as package:
                for member in package:
                    name = safe_relative(member.name)
                    if name == '.':
                        continue
                    # Keep the package inside a chroot; never execute maintainer scripts.
                    if not (member.isdir() or member.isfile() or member.issym()):
                        raise RuntimeError('Unsupported vendor package file type')
                    if member.issym():
                        resolved = PurePosixPath(member.linkname)
                        if resolved.is_absolute() or '..' in resolved.parts:
                            # Official package links use a chroot-absolute target.
                            # Rewrite to a relative link with the same in-chroot destination.
                            dest = member.linkname.lstrip('/')
                            if '..' in PurePosixPath(dest).parts:
                                raise RuntimeError('Unsafe vendor package link')
                            member.linkname = os.path.relpath(dest, str(PurePosixPath(name).parent)).replace('\\', '/')
                    member.name = 'opt/native-compat/iflytek-focal/' + name
                    member.uid = member.gid = 0
                    member.uname = member.gname = ''
                    archive.addfile(member, package.extractfile(member) if member.isfile() else None)
            add_bytes(archive, 'etc/native-mobile/iflytek-installed', b'official optional package; vendor terms accepted\n')
            add_bytes(archive, 'etc/native-mobile/iflytek-voice-enabled', b'1\n')
    ramdisk = read_ramdisk((BASE / 'payload/initramfs.cpio.gz').read_bytes())
    image = rebuild(original, ramdisk)
    if len(image) > info['partitions']['boot']['bytes']:
        raise RuntimeError('Boot image exceeds its partition')
    image = image.ljust(info['partitions']['boot']['bytes'], b'\0')
    boot = work / 'linux-boot.img'
    boot.write_bytes(image)
    report = {'format': 1, 'prepared_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'device': info, 'release_rootfs_sha256': bundle['assets']['payload/rootfs.tar.gz']['sha256'],
              'recovery_sha256': backups['recovery']['sha256'], 'original_boot_sha256': backups['boot']['sha256'],
              'boot_sha256': digest(boot), 'overlay_sha256': digest(overlay),
              'backups': backups, 'iflytek_requested': bool(optional),
              'private_work_directory_do_not_publish': True}
    (work / 'prepared.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('Prepared and verified. The phone has not been formatted or flashed.')


def install(device, work, confirmation):
    if confirmation != 'ERASE-X620':
        raise RuntimeError('Installation erases system and userdata. Explicit --confirm-erase ERASE-X620 is required')
    bundle = verify_bundle()
    prepared = json.loads((work / 'prepared.json').read_text(encoding='utf-8'))
    info = inspect(device)
    if info != prepared['device'] or bundle['assets']['payload/rootfs.tar.gz']['sha256'] != prepared['release_rootfs_sha256']:
        raise RuntimeError('Device or release differs from preparation')
    for name, entry in prepared['backups'].items():
        path = work / 'backup' / (name + '.img')
        if path.stat().st_size != entry['bytes'] or digest(path) != entry['sha256']:
            raise RuntimeError('Private backup verification failed: ' + name)
    for name, expected in [('linux-boot.img', prepared['boot_sha256']), ('device-overlay.tar.gz', prepared['overlay_sha256'])]:
        if digest(work / name) != expected:
            raise RuntimeError('Prepared asset changed')
    for name, expected in [('boot', prepared['original_boot_sha256']), ('recovery', prepared['recovery_sha256'])]:
        if device.shell('sha256sum ' + BLOCK + '/' + name).decode().split()[0] != expected:
            raise RuntimeError('Device changed after preparation')
    # All payloads live in recovery RAM, so formatting userdata cannot erase them.
    total = (BASE / 'payload/rootfs.tar.gz').stat().st_size + (work / 'device-overlay.tar.gz').stat().st_size + (work / 'linux-boot.img').stat().st_size
    free = int(device.shell("df -Pk /tmp | tail -n 1 | awk '{print $4}'").strip()) * 1024
    if free < total + 96 * 1024**2:
        raise RuntimeError('Not enough recovery RAM; no format performed')
    stage = '/tmp/le620-public-install'
    device.shell('test ! -e ' + stage + '\nmkdir -m 700 ' + stage)
    for local, name, expected in [(BASE / 'payload/rootfs.tar.gz', 'rootfs.tar.gz', prepared['release_rootfs_sha256']),
                                  (work / 'device-overlay.tar.gz', 'overlay.tar.gz', prepared['overlay_sha256']),
                                  (work / 'linux-boot.img', 'boot.img', prepared['boot_sha256'])]:
        device.run(['push', str(local), stage + '/' + name], timeout=900)
        if device.shell('sha256sum ' + stage + '/' + name).decode().split()[0] != expected:
            raise RuntimeError('Uploaded asset checksum mismatch')
    # Do not silently unmount storage the user is using in recovery.
    # A read-only donor mount from our prepare phase is the sole managed mount.
    device.shell('if mountpoint -q /tmp/le620-release-source; then umount /tmp/le620-release-source; fi')
    mounts = device.shell('cat /proc/mounts').decode().splitlines()
    targets = {info['partitions'][name]['path'] for name in ['system', 'userdata']}
    for row in mounts:
        source = row.split()[0]
        resolved = device.shell('readlink -f ' + shlex.quote(source) + ' || true').decode().strip() if source.startswith('/dev/') else source
        if resolved in targets:
            raise RuntimeError('Unmount System and Data, disable MTP in TWRP, then retry; no format performed')
    system = info['partitions']['system']['path']
    userdata = info['partitions']['userdata']['path']
    boot = info['partitions']['boot']['path']
    recovery = info['partitions']['recovery']['path']
    code = f'''set -eu
mkdir -p {stage}/root {stage}/data
mke2fs -F -t ext4 -b 4096 -m 1 -L le620-root -O {FEATURES} -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 {system}
mke2fs -F -t ext4 -b 4096 -m 0 -L le620-data -O {FEATURES} -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 {userdata}
mount -t ext4 {system} {stage}/root
mount -t ext4 {userdata} {stage}/data
tar -xzf {stage}/rootfs.tar.gz -C {stage}/root
tar -xzf {stage}/overlay.tar.gz -C {stage}/root
chmod 700 {stage}/root/root {stage}/root/root/.ssh
chmod 600 {stage}/root/root/.ssh/authorized_keys
mkdir -p {stage}/data/www {stage}/data/files
printf '%s\n' '<!doctype html><meta charset=utf-8><title>X620 Linux</title><h1>X620 Linux server</h1>' >{stage}/data/www/index.html
chmod 755 {stage}/data {stage}/data/www {stage}/data/files
chmod 644 {stage}/data/www/index.html
chroot {stage}/root /bin/busybox true
chroot {stage}/root /bin/sh -n /usr/local/sbin/native-release-init
chroot {stage}/root /bin/sh -n /usr/local/sbin/native-early
test -x {stage}/root/usr/bin/phoc
test -x {stage}/root/usr/libexec/phosh
sync
dd if={stage}/boot.img of={boot} bs=1048576
sync
test "$(sha256sum {boot} | cut -d' ' -f1)" = {prepared['boot_sha256']}
test "$(sha256sum {recovery} | cut -d' ' -f1)" = {prepared['recovery_sha256']}
umount {stage}/data
umount {stage}/root
sync
echo INSTALL_VERIFIED
'''
    result = device.run(['shell', '-T', 'sh'], data=code.encode(), timeout=1800)
    if b'INSTALL_VERIFIED' not in result:
        raise RuntimeError('Installation not verified; stay in TWRP and use private backups')
    (work / 'installed.json').write_text(json.dumps({'verified': True, 'boot_sha256': prepared['boot_sha256'],
                                                    'recovery_unchanged': True, 'automatic_reboot': False}, indent=2) + '\n')
    print('Installation verified. Recovery is unchanged. Use TWRP Reboot > System manually.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default=shutil.which('adb') or 'adb')
    parser.add_argument('--serial', required=True, help='Your device serial from adb devices; never a published donor serial')
    parser.add_argument('--work', type=Path, default=BASE / 'work/private-device')
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('inspect', help='Read-only recovery/layout check')
    prepare_args = sub.add_parser('prepare', help='Back up original partitions; create a private device-specific overlay')
    prepare_args.add_argument('--ssh-public-key', required=True, type=Path)
    prepare_args.add_argument('--iflytek-deb', type=Path, help='Optional official 2.0.51 ARM64 .deb, downloaded from the vendor')
    prepare_args.add_argument('--accept-iflytek-terms', action='store_true')
    install_args = sub.add_parser('install', help='Erase system and userdata and install the prepared release')
    install_args.add_argument('--confirm-erase', required=True)
    args = parser.parse_args()
    device = Device(args.adb, args.serial)
    if args.action == 'inspect':
        print(json.dumps(inspect(device), indent=2))
    elif args.action == 'prepare':
        prepare(device, args.work.resolve(), args)
    else:
        install(device, args.work.resolve(), args.confirm_erase)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print('STOP:', str(error), file=sys.stderr)
        raise SystemExit(1)
