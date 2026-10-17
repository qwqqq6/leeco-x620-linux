"""Exercise erase guards and archive boundaries without connecting a phone."""
import hashlib
import base64
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import flash
import bootimage


class FlashSafety(unittest.TestCase):
    def setUp(self):
        self.layout = {name: {'path': '/dev/block/' + suffix,
                             'bytes': size or 24 * 1024**3 + 100 * 1024**2}
                       for name, (suffix, size) in flash.PARTS.items()}

    def test_tested_layout_and_dev_alias(self):
        flash.validate_layout(self.layout)
        for row in self.layout.values():
            row['path'] = row['path'].replace('/dev/block/', '/dev/')
        flash.validate_layout(self.layout)

    def test_wrong_partition_never_accepted(self):
        for name in flash.PARTS:
            with self.subTest(partition=name):
                changed = json.loads(json.dumps(self.layout))
                changed[name]['path'] = '/dev/block/mmcblk0p2'
                with self.assertRaises(RuntimeError):
                    flash.validate_layout(changed)

    def test_wrong_size_or_other_storage_variant(self):
        for name in flash.PARTS:
            with self.subTest(partition=name):
                changed = json.loads(json.dumps(self.layout))
                changed[name]['bytes'] += 8 * 1024**3
                with self.assertRaises(RuntimeError):
                    flash.validate_layout(changed)

    def test_no_erase_or_contact_without_explicit_phrase(self):
        device = mock.Mock()
        with self.assertRaises(RuntimeError):
            flash.install(device, Path('unused'), 'yes')
        device.assert_not_called()
        self.assertEqual(device.method_calls, [])

    def test_corrupt_backup_stops_before_push_or_format(self):
        device = mock.Mock()
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'backup').mkdir()
            (work / 'backup/boot.img').write_bytes(b'changed')
            prepared = {'device': {'same': True}, 'release_rootfs_sha256': 'r',
                        'backups': {'boot': {'bytes': 7, 'sha256': 'not-a-match'}}}
            (work / 'prepared.json').write_text(json.dumps(prepared))
            with mock.patch.object(flash, 'verify_bundle', return_value={'assets': {'payload/rootfs.tar.gz': {'sha256': 'r'}}}), \
                 mock.patch.object(flash, 'inspect', return_value={'same': True}):
                with self.assertRaisesRegex(RuntimeError, 'backup verification'):
                    flash.install(device, work, 'ERASE-X620')
            self.assertEqual(device.method_calls, [])

    def test_device_switch_stops_before_mutation(self):
        device = mock.Mock()
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'prepared.json').write_text(json.dumps({'device': {'serial_sha256': 'a'},
                                                           'release_rootfs_sha256': 'r'}))
            with mock.patch.object(flash, 'verify_bundle', return_value={'assets': {'payload/rootfs.tar.gz': {'sha256': 'r'}}}), \
                 mock.patch.object(flash, 'inspect', return_value={'serial_sha256': 'b'}):
                with self.assertRaisesRegex(RuntimeError, 'Device or release differs'):
                    flash.install(device, work, 'ERASE-X620')
            self.assertEqual(device.method_calls, [])

    def test_archive_traversal_rejected(self):
        for name in ['../boot', 'a/../../boot', '/etc/shadow', 'a\\..\\boot', 'a\x00b']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                flash.safe_relative(name)
        self.assertEqual(flash.safe_relative('./system/lib64/file.so'), 'system/lib64/file.so')

    def test_malformed_debian_package_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'input.deb'
            for data in [b'not an archive', b'!<arch>\nshort header']:
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    flash.ar_payload(path)

    def test_public_key_structure_and_private_key_rejection(self):
        blob = struct.pack('>I', 11) + b'ssh-ed25519' + struct.pack('>I', 32) + b'x' * 32
        key = 'ssh-ed25519 ' + base64.b64encode(blob).decode() + ' recipient'
        self.assertEqual(flash.validate_public_key(key), key)
        for invalid in ['-----BEGIN OPENSSH PRIVATE KEY-----', 'ssh-ed25519 eA==', key + '\n' + key]:
            with self.assertRaises(RuntimeError):
                flash.validate_public_key(invalid)

    def test_calibration_read_refuses_writable_mount(self):
        device = mock.Mock()
        device.shell.side_effect = [b'/dev/block/mmcblk0p14\n',
                                    b'/dev/block/mmcblk0p14 /nvdata ext4 rw,noatime 0 0\n']
        with self.assertRaisesRegex(RuntimeError, 'read-only'):
            flash.calibration_hash(device)
        self.assertEqual(device.shell.call_count, 2)

    def test_calibration_read_error_still_unmounts_managed_readonly_mount(self):
        device = mock.Mock()
        device.shell.side_effect = [b'/dev/block/mmcblk0p14\n', b'', b'', RuntimeError('missing GPS record'), b'']
        with self.assertRaisesRegex(RuntimeError, 'missing GPS'):
            flash.calibration_hash(device)
        self.assertIn('mount -t ext4 -o ro', device.shell.call_args_list[2].args[0])
        self.assertEqual(device.shell.call_args_list[-1].args[0], 'umount /tmp/le620-release-calibration')

    def test_boot_rebuild_keeps_recipient_kernel_and_addresses(self):
        page = 2048
        header = bytearray(page)
        kernel = b'recipient-only-kernel-and-dtb'
        struct.pack_into('<8s10I', header, 0, b'ANDROID!', len(kernel), 0x40080000,
                         0, 0x44000000, 0, 0x45000000, 0x40000100, page, 0, 0)
        original = bytes(header) + kernel.ljust(page, b'\0')
        image = bootimage.rebuild(original, {'init': {'mode': 0o100755, 'data': b'#!/bin/sh\n'}})
        parsed = bootimage.parse_boot(image)
        self.assertEqual(parsed['kernel'], kernel)
        self.assertEqual(parsed['header'][12:16], original[12:16])
        self.assertEqual(parsed['header'][20:48], original[20:48])
        self.assertEqual(bootimage.read_ramdisk(parsed['ramdisk'])['init']['data'], b'#!/bin/sh\n')


if __name__ == '__main__':
    unittest.main()
