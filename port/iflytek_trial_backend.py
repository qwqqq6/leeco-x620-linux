"""Phone-side session service or bounded trial of the official iFlytek backend.

Run as root on the handset. Each client runs as desktop, with a separate home,
glibc filesystem and DBus session. It does not record or enable microphone ADCs.
"""
from __future__ import annotations

import argparse
import configparser
import fcntl
import json
import os
import shutil
import signal
import stat
import subprocess
import time
from pathlib import Path

ROOT = Path('/opt/native-compat/iflytek-focal')
CONTROL = Path('/run/native-iflytek')
RUNTIME = Path('/run/native-iflytek-client/session')
stopping = False


def drop():
    os.setgroups([100, 18, 3003])
    os.setgid(100)
    os.setuid(1000)


def enter():
    os.chroot(ROOT)
    os.chdir('/home/desktop')
    drop()


def stop(signum, frame):
    global stopping
    stopping = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--duration', type=int)
    group.add_argument('--service', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0 or not (ROOT / '.le620-iflytek-trial').is_file():
        raise SystemExit('Run only in the prepared phone trial')
    if args.duration is not None and not 10 <= args.duration <= 3600:
        parser.error('Trial duration must be between 10 and 3600 seconds')
    os.umask(0o077)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if args.service:
        while not Path('/run/user/1000/wayland-0').is_socket():
            if stopping:
                return
            time.sleep(.25)
    # The official ARM64 libiflyime.so's HMAC provider dlopens this MIPS
    # distribution path. Supply a path alias to the signed ARM64 OpenSSL 1.1
    # runtime; leave the proprietary library and certificate checks intact.
    ssl = ROOT / 'usr/lib/aarch64-linux-gnu/libssl.so.1.1'
    if not ssl.is_file():
        raise SystemExit('Signed OpenSSL 1.1 runtime is required')
    alias_dir = ROOT / 'usr/lib/mips64el-linux-gnuabi64'
    alias_dir.mkdir(mode=0o755, exist_ok=True)
    os.chmod(alias_dir, 0o755)
    alias = alias_dir / 'libssl.so'
    target = '../aarch64-linux-gnu/libssl.so.1.1'
    if alias.is_symlink():
        if os.readlink(alias) != target:
            raise SystemExit('Unexpected OpenSSL compatibility alias')
    elif alias.exists():
        raise SystemExit('Refusing to replace an OpenSSL runtime file')
    else:
        alias.symlink_to(target)
    CONTROL.mkdir(mode=0o700, exist_ok=True)
    if CONTROL.is_symlink() or CONTROL.stat().st_uid != 0:
        raise SystemExit('Invalid root control directory')
    os.chmod(CONTROL, 0o700)
    lock = (CONTROL / 'backend.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    RUNTIME.parent.mkdir(mode=0o755, exist_ok=True)
    if RUNTIME.parent.is_symlink() or RUNTIME.parent.stat().st_uid != 0:
        raise SystemExit('Invalid IPC directory')
    os.chmod(RUNTIME.parent, 0o755)
    RUNTIME.mkdir(exist_ok=True)
    if RUNTIME.is_symlink():
        raise SystemExit('Invalid session directory')
    os.chown(RUNTIME, 1000, 100)
    os.chmod(RUNTIME, 0o700)
    pidfile = CONTROL / 'backend.pid'
    pidfile.write_text(str(os.getpid()) + '\n')
    # Recreate only non-personal defaults if restoring a privacy-filtered
    # snapshot. Never copy account identity, recognition history or user text.
    home = ROOT / 'home/desktop'
    home.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chown(home, 1000, 100)
    defaults = Path('/etc/native-mobile/iflytek-defaults')
    templates = [(p, home / p.relative_to(defaults)) for p in defaults.rglob('*') if p.is_file()]
    templates.append((Path('/etc/native-mobile/iflytek-asound.conf'), home / '.asoundrc'))
    for source, target in templates:
        if target.exists():
            continue
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        parent = target.parent
        while parent != home:
            os.chown(parent, 1000, 100)
            parent = parent.parent
        shutil.copyfile(source, target)
        os.chown(target, 1000, 100)
        os.chmod(target, 0o600)
    # Keep the official Qt services for voice, but hide their desktop status
    # bar: Phosh already has the touch keyboard and its own candidate surface.
    # Existing trial profiles may still have the vendor's visible-bar default.
    settings_path = home / '.config/iflytek/inputmethod.ini'
    settings = configparser.ConfigParser(interpolation=None)
    settings.optionxform = str
    settings.read(settings_path)
    if not settings.has_section('common'):
        settings.add_section('common')
    if settings.get('common', 'status_bar_display', fallback=None) != '0':
        settings.set('common', 'status_bar_display', '0')
        temporary = settings_path.with_suffix('.ini.tmp')
        with temporary.open('w') as stream:
            settings.write(stream, space_around_delimiters=False)
        os.chown(temporary, 1000, 100)
        os.chmod(temporary, 0o600)
        temporary.replace(settings_path)
    for name, kind in [('proc', 'proc'), ('sys', 'sysfs')]:
        path = ROOT / name
        path.mkdir(exist_ok=True)
        if not os.path.ismount(path):
            subprocess.run(['mount', '-t', kind, '-o', 'ro,nosuid,nodev,noexec',
                            kind, str(path)], check=True)
    for target, source in [('run/user/1000', '/run/user/1000'),
                           ('tmp/.X11-unix', '/tmp/.X11-unix'),
                           ('run/native-iflytek-client', str(RUNTIME.parent))]:
        path = ROOT / target
        path.mkdir(parents=True, exist_ok=True)
        if not os.path.ismount(path):
            subprocess.run(['mount', '-o', 'bind', source, str(path)], check=True)
    # PortAudio probes every visible hardware PCM. Several unused MTK voice/FM
    # drivers are unsafe to probe outside Android: expose only the proven ADC.
    sound = ROOT / 'dev/snd'
    if os.path.ismount(sound):
        raise SystemExit('Refusing an unrestricted sound-device bind mount')
    sound.mkdir(exist_ok=True)
    # os.umask(077) also applies to mkdir. The desktop audio group must be able
    # to traverse this directory to open the two explicitly allowed nodes.
    os.chmod(sound, 0o755)
    allowed = {'controlC0', 'pcmC0D1c'}
    if any(p.name not in allowed for p in sound.iterdir()):
        raise SystemExit('Unexpected device in isolated ALSA directory')
    for name in allowed:
        device = sound / name
        original = Path('/dev/snd') / name
        if not device.exists():
            os.mknod(device, stat.S_IFCHR | 0o660, original.stat().st_rdev)
        if not stat.S_ISCHR(device.stat().st_mode) or device.stat().st_rdev != original.stat().st_rdev:
            raise SystemExit('Invalid isolated ALSA node')
        os.chown(device, 0, 18)
        os.chmod(device, 0o660)
    authority = ROOT / 'run/iflytek-Xauthority'
    shutil.copyfile('/run/native-desktop/Xauthority', authority)
    os.chown(authority, 1000, 100)
    os.chmod(authority, 0o600)
    bus = RUNTIME / 'bus'
    bus.unlink(missing_ok=True)
    adapter = '/usr/local/lib/libnativeiflytekalsa.so'
    if not (ROOT / adapter.lstrip('/')).is_file():
        raise SystemExit('The verified native capture adapter is required')
    env = {'PATH': '/usr/bin:/bin', 'HOME': '/home/desktop',
           'USER': 'desktop', 'LOGNAME': 'desktop', 'LANG': 'C.UTF-8',
           'XDG_RUNTIME_DIR': '/run/user/1000', 'WAYLAND_DISPLAY': 'wayland-0',
           'QT_QPA_PLATFORM': 'wayland', 'QT_QUICK_BACKEND': 'software',
           'QT_WAYLAND_DISABLE_WINDOWDECORATION': '1', 'QT_IM_MODULE': 'compose',
           'DISPLAY': ':0', 'XAUTHORITY': '/run/iflytek-Xauthority',
           'XMODIFIERS': '@im=fcitx', 'DBUS_SESSION_BUS_ADDRESS': 'unix:path=' + str(bus)}
    processes, logs = [], []
    try:
        for name, command, setup in [
            ('bus', ['/usr/bin/dbus-daemon', '--session', '--nofork',
                     '--address=unix:path=' + str(bus)], drop),
            ('fcitx', ['/usr/bin/fcitx', '-D', '-s', '0', '--enable', 'fcitx-iflyime',
                       '--disable', 'fcitx-clipboard,fcitx-xim'], enter),
        ]:
            log = (CONTROL / (name + '.log')).open('wb')
            logs.append(log)
            client_env = dict(env)
            if name == 'fcitx':
                # The adapter resolves functions through dlsym. Focal's
                # libdl is separate from libc; Fcitx's shell/DBus children
                # inherit the preload and must be able to resolve it too.
                keyboard = '/usr/local/lib/libnativeiflytekkeyboard.so'
                client_env['LD_PRELOAD'] = 'libdl.so.2:' + adapter
                if (ROOT / keyboard.lstrip('/')).is_file():
                    client_env['LD_PRELOAD'] = keyboard + ':' + client_env['LD_PRELOAD']
            process = subprocess.Popen(command, env=client_env, preexec_fn=setup,
                                       stdin=subprocess.DEVNULL, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            processes.append(process)
            time.sleep(2)
            if process.poll() is not None:
                raise RuntimeError(name + ' exited during startup')
        status = {'running': True, 'pid': os.getpid(),
                  'client_uid': 1000, 'trial_duration_seconds': args.duration,
                  'persistent_session_service': args.service,
                  'microphone_route_enabled_by_backend': False,
                  'accessible_sound_nodes': sorted(allowed)}
        (CONTROL / 'backend-status.json').write_text(json.dumps(status) + '\n')
        deadline = time.monotonic() + args.duration if args.duration else float('inf')
        while not stopping and time.monotonic() < deadline:
            if any(process.poll() is not None for process in processes):
                break
            time.sleep(0.5)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        # Fcitx also starts its own DBus watcher and official Qt client.
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                if os.readlink(proc / 'root') == str(ROOT) and int(proc.name) != os.getpid():
                    os.kill(int(proc.name), signal.SIGTERM)
            except OSError:
                pass
        for log in logs:
            log.close()
        pidfile.unlink(missing_ok=True)
        (CONTROL / 'backend-status.json').write_text(json.dumps({'running': False}) + '\n')


if __name__ == '__main__':
    main()
