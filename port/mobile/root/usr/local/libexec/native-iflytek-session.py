#!/usr/bin/python3
"""Keep the accepted official IBus bridge alive; fall back while unavailable."""
import os
import signal
import subprocess
import time
from pathlib import Path

stopping = False


def stop(*_args):
    global stopping
    stopping = True


def select(engine):
    try:
        return subprocess.run(['ibus', 'engine', engine], stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=4).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def main():
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, stop)
    child = None
    runtime = Path(os.environ['XDG_RUNTIME_DIR'])
    try:
        while not stopping:
            if not Path('/run/native-iflytek-client/session/bus').is_socket():
                time.sleep(.5)
                continue
            with (runtime / 'iflytek-bridge.log').open('wb') as log:
                child = subprocess.Popen(['/usr/local/libexec/native-iflytek-bridge.py'],
                    stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            selected = False
            for _ in range(24):
                if stopping or child.poll() is not None:
                    break
                if select('iflytek'):
                    selected = True
                    break
                time.sleep(.25)
            if not selected and child.poll() is None:
                child.terminate()
            while not stopping and child.poll() is None:
                time.sleep(.5)
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            child = None
            if not stopping:
                select('libpinyin')
                for _ in range(6):
                    if stopping:
                        break
                    time.sleep(.5)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=5)


if __name__ == '__main__':
    main()
