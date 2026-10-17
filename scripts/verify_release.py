#!/usr/bin/env python3
"""Independently validate the public rootfs, manifest and privacy exclusions."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
import sys
import tarfile
from pathlib import Path, PurePosixPath

BASE = Path(__file__).resolve().parents[1]
PATTERNS = [re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]{64,}-----END (?:OPENSSH |RSA |EC )?PRIVATE KEY-----'),
            re.compile(rb'(?:cpa-|ghp_|gho_|github_pat_)[A-Za-z0-9_]{24,}'),
            re.compile(rb'sk-(?:proj-)?[A-Za-z0-9_-]{40,}')]


def state_path(name):
    prefixes = ['root/.ssh/', 'root/.codex/', 'home/desktop/.codex/',
                'home/desktop/.config/native-codex/', 'home/desktop/.cache/',
                'home/desktop/.local/share/recently-used', 'etc/dropbear/dropbear_',
                'var/lib/native-bluetooth/link-keys', 'var/lib/native/failed-configs/',
                'opt/native-compat/iflytek-focal/home/desktop/']
    return name in ['etc/machine-id', 'var/lib/dbus/machine-id', 'root/.ash_history',
                    'home/desktop/.ash_history', 'home/desktop/.ICEauthority',
                    'opt/native-compat/iflytek-focal/etc/machine-id'] or any(name.startswith(p) for p in prefixes)


def validate(root=BASE, extra_secrets=(), protected_secrets=(), fail=True):
    manifest = json.loads((root / 'manifest/release.json').read_text(encoding='utf-8'))
    expected = {x['path']: x for x in json.loads((root / 'manifest/rootfs-files.json').read_text(encoding='utf-8'))}
    if len(expected) != manifest['rootfs_entries']:
        raise RuntimeError('Duplicate rootfs manifest paths')
    failures = []
    upstream_path = root / 'manifest/upstream-scan-references.json'
    upstream = json.loads(upstream_path.read_text(encoding='utf-8')) if upstream_path.exists() else {}
    references_matched = set()
    bytes_scanned = files_scanned = 0

    def scan(name, stream):
        nonlocal bytes_scanned, files_scanned
        h = hashlib.sha256()
        tail = b''
        generic_hit = private_hit = protected_hit = False
        overlap = max([256] + [len(x) for x in extra_secrets])
        while chunk := stream.read(1024 * 1024):
            h.update(chunk)
            bytes_scanned += len(chunk)
            window = tail + chunk
            if any(p.search(window) for p in PATTERNS):
                generic_hit = True
            if any(value in window for value in extra_secrets):
                private_hit = True
            if any(value in window for value in protected_secrets):
                protected_hit = True
            tail = window[-overlap:]
        files_scanned += 1
        result = h.hexdigest()
        if protected_hit:
            failures.append({'path': name, 'reason': 'protected private source value'})
        if generic_hit or private_hit:
            if upstream.get(name, {}).get('file_sha256') == result:
                references_matched.add(name)
            else:
                failures.append({'path': name, 'reason': 'unresolved credential/reference match'})
        return result

    for name, item in manifest['assets'].items():
        path = root / name
        if path.stat().st_size != item['bytes']:
            raise RuntimeError('Asset size differs: ' + name)
        with path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != item['sha256']:
                raise RuntimeError('Asset checksum differs: ' + name)
    found, contents = set(), {}
    with tarfile.open(root / 'payload/rootfs.tar.gz', 'r|gz') as archive:
        for entry in archive:
            name = entry.name
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or '\\' in name:
                raise RuntimeError('Unsafe rootfs path')
            if name in found or name not in expected:
                raise RuntimeError('Unexpected/duplicate rootfs entry: ' + name)
            found.add(name)
            record = expected[name]
            if (entry.mode, entry.uid, entry.gid, entry.size, entry.type.decode()) != (
                    int(record['mode'], 8), record['uid'], record['gid'], record['bytes'], record['type']):
                raise RuntimeError('Rootfs metadata differs: ' + name)
            if entry.isfile():
                if state_path(name):
                    failures.append({'path': name, 'reason': 'personal state path'})
                stream = archive.extractfile(entry)
                if name in ['etc/wpa_supplicant/wpa_supplicant.conf', 'etc/native-mobile/codex-config.toml',
                            'usr/local/sbin/native-network', 'etc/shadow', 'usr/local/sbin/native-early',
                            'usr/local/libexec/native-iflytek-service']:
                    data = stream.read()
                    contents[name] = data
                    import io
                    stream = io.BytesIO(data)
                if scan(name, stream) != record['sha256']:
                    raise RuntimeError('Rootfs content differs: ' + name)
            elif entry.linkname != record.get('link', ''):
                raise RuntimeError('Rootfs link differs: ' + name)
            if name.startswith(('system/', 'usr/local/lib/native-gnss/system/', 'usr/local/lib/native-gpu/system/',
                                'usr/local/lib/native-tfa/lib64/')) and entry.isfile():
                if not name.endswith(('libnativeprops.so', 'libnativegnssprops.so', 'libnativegpuprops.so')):
                    failures.append({'path': name, 'reason': 'redistributed donor blob'})
            if name.startswith('opt/native-compat/iflytek-focal/opt/apps/com.iflytek.iflyime/') and not entry.isdir():
                failures.append({'path': name, 'reason': 'redistributed proprietary input client'})
    if found != set(expected):
        raise RuntimeError('Missing rootfs paths')
    wifi = contents['etc/wpa_supplicant/wpa_supplicant.conf']
    if b'network=' in wifi or b'psk=' in wifi or b'ssid=' in wifi:
        raise RuntimeError('Wi-Fi profile is not blank')
    config = contents['etc/native-mobile/codex-config.toml']
    import tomllib
    if tomllib.loads(config.decode()):
        raise RuntimeError('Codex configuration is not empty')
    if any(row.split(b':')[1] != b'*' for row in contents['etc/shadow'].splitlines()):
        raise RuntimeError('Account passwords not locked')
    if b'/usr/local/sbin/native-release-init\n/usr/local/sbin/native-network' not in contents['usr/local/sbin/native-early']:
        raise RuntimeError('First-boot identity generator not in startup path')
    if b'cut -c1-16 /etc/machine-id' not in contents['usr/local/sbin/native-network']:
        raise RuntimeError('USB identity not regenerated')
    if b'iflytek-installed' not in contents['usr/local/libexec/native-iflytek-service']:
        raise RuntimeError('Optional vendor client not guarded')
    for p in root.rglob('*'):
        if not p.is_file() or '.git' in p.parts or 'payload' in p.relative_to(root).parts or '__pycache__' in p.parts:
            continue
        relative = p.relative_to(root).as_posix()
        if p.stat().st_size > 20 * 1024**2:
            raise RuntimeError('Unexpected large source file: ' + relative)
        with p.open('rb') as stream:
            scan(relative, stream)
    # The ramdisk contains no kernel, boot header, network profile, or donor data.
    sys.path.insert(0, str(root / 'scripts'))
    from bootimage import read_ramdisk
    ramdisk = read_ramdisk((root / 'payload/initramfs.cpio.gz').read_bytes())
    for name, item in ramdisk.items():
        import io
        scan('initramfs/' + name, io.BytesIO(item['data']))
    allowed = {'bin', 'bin/busybox', 'bin/reboot-recovery', 'bin/sh', 'dev', 'dev/console',
               'init', 'newroot', 'proc', 'sbin', 'sys'}
    if set(ramdisk) != allowed:
        raise RuntimeError('Unexpected ramdisk layout')
    import io
    scan('payload/native-tfa-template.bin', io.BytesIO((root / 'payload/native-tfa-template.bin').read_bytes()))
    report = {'passed': not failures, 'files_scanned': files_scanned, 'bytes_scanned': bytes_scanned,
              'rootfs_entries': len(found), 'known_private_values_checked': len(extra_secrets),
              'unmodified_upstream_reference_files': len(references_matched),
              'credential_values_never_printed': True, 'failures': failures,
              'manifest_metadata_hashes_and_asset_hashes_verified': True,
              'recipient_identity_generation_verified': True,
              'no_flash_test_performed': True}
    if failures and fail:
        raise RuntimeError('Privacy scan failed: ' + json.dumps(failures[:20]))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=BASE)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = validate(args.root.resolve())
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
