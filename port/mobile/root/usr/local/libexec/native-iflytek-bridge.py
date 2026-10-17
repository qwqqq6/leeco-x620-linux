#!/usr/bin/python3
"""Expose the unmodified official Fcitx4 engine to Phosh's IBus clients.

No recognizer or dictionary is implemented here. User text is never logged.
Microphone routing starts only when the user activates the VoiceStart action.
"""
import os
import configparser
import re
import signal
import subprocess
import time
from pathlib import Path

import gi
gi.require_version('IBus', '1.0')
from gi.repository import IBus, Gio, GLib
from native_iflytek_space import SpaceHold

SERVICE = 'org.le620.Iflytek'
FCITX = 'org.fcitx.Fcitx'
METHOD = FCITX + '.InputMethod'
CONTEXT = FCITX + '.InputContext'
ADDRESS_DIR = Path('/opt/native-compat/iflytek-focal/home/desktop/.config/fcitx/dbus')
active = None
voice = None
engines = set()
TRIAL = Path('/opt/native-compat/iflytek-focal')


class Voice:
    """KIMPanel adapter: state comes from the official engine and real PCM.

    Start and stop are separate operations. In particular, a second Stop never
    emits another toggle that could start recording again.
    """
    def __init__(self):
        self.engine = self.route = None
        self.state = 'unavailable'
        self.native = None
        self.deadline = 0
        self.start_sent = self.stop_sent = False
        self.route_stop_sent = False
        self.has_result = False
        self.discard_result = False
        self.result_settle = 0
        self.ready = Path(os.environ['XDG_RUNTIME_DIR']) / 'iflytek-microphone-ready'
        self.client = Gio.DBusConnection.new_for_address_sync(
            'unix:path=/run/native-iflytek-client/session/bus',
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        # The official Qt client owns panel services used internally by Fcitx.
        # Never race it for the name during boot; its documented hotkey works
        # independently of whether the notification panel has registered yet.
        self.owns_panel = False
        self.client.signal_subscribe(FCITX, 'org.kde.kimpanel.inputmethod', None,
            '/kimpanel', None, Gio.DBusSignalFlags.NONE, self.received, None)
        self.client.connect('closed', self.backend_closed)
        process_id = self.client.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
            GLib.Variant('(s)', (FCITX,)), None, Gio.DBusCallFlags.NONE, 2000, None).unpack()[0]
        self.capture_status = Path(os.environ['XDG_RUNTIME_DIR']) / f'iflytek-capture.{process_id}.state'
        self.last_refresh = time.monotonic()
        self.state = 'ready' if not self.pcm_running() else 'unavailable'
        if not Path('/etc/native-mobile/iflytek-voice-enabled').is_file():
            self.state = 'disabled'

    def backend_closed(self, *_args):
        self.fail('unavailable')
        # Let the session supervisor restore libpinyin and reconnect cleanly.
        GLib.idle_add(lambda: (os.kill(os.getpid(), signal.SIGTERM), False)[1])

    def emit(self, name, parameters=None):
        self.client.emit_signal(None, '/org/kde/impanel', 'org.kde.impanel',
                                name, parameters)

    def trigger(self):
        if self.owns_panel:
            self.emit('TriggerProperty', GLib.Variant('(s)', ('/Fcitx/iflyime-voice',)))
        elif self.engine:
            self.engine.hotkey()

    @staticmethod
    def pcm_running():
        try:
            return 'state: RUNNING' in Path('/proc/asound/card0/pcm1c/sub0/status').read_text()
        except OSError:
            return False

    def capture_state(self):
        try:
            return self.capture_status.read_text().strip()
        except OSError:
            return 'unknown'

    def received(self, connection, sender, path, interface, member, parameters, data):
        if member == 'RegisterProperties':
            properties = parameters.unpack()[0]
        elif member == 'UpdateProperty':
            properties = parameters.unpack()
        else:
            return
        for prop in properties:
            fields = prop.split(':')
            if len(fields) < 2 or fields[0] != '/Fcitx/iflyime-voice':
                continue
            known = ('voiceidle', 'voiceworking', 'startReord_err')
            self.native = next((item for item in fields[1:] if item in known), fields[1])
            print('voice-state', self.native if self.native in (
                'voiceidle', 'voiceworking', 'startReord_err') else 'other', flush=True)
            if self.native == 'voiceworking':
                if (self.state == 'starting' and self.start_sent and self.pcm_running()
                        and self.capture_state() == 'streaming'):
                    self.state = 'recording'
                    self.deadline = time.monotonic() + 55
                elif self.state == 'stopping':
                    self.send_stop()
                elif self.state != 'recording':
                    # Never adopt an unsolicited recording as a fresh request.
                    self.stop()
            elif self.native == 'voiceidle':
                if self.state == 'unavailable':
                    self.state = 'ready'
                elif self.state in ('stopping', 'processing') and not self.pcm_running():
                    self.release_route()
                    # voiceidle can precede the final punctuation CommitString.
                    # monitor() waits for the official result to settle.
                    if self.state != 'processing':
                        self.state = 'processing'
                        self.deadline = time.monotonic() + 15
            elif 'err' in self.native.lower():
                self.fail('record-error' if self.native == 'startReord_err' else 'service-error')

    def release_route(self):
        if self.route is not None and self.route.poll() is None and not self.route_stop_sent:
            self.route_stop_sent = True
            self.route.terminate()

    def fail(self, state):
        self.release_route()
        self.state = state

    def start(self):
        if not Path('/etc/native-mobile/iflytek-voice-enabled').is_file():
            self.state = 'disabled'
            return False
        if self.state not in ('ready', 'record-error', 'route-error', 'service-error'):
            return False
        if active is None or active.secure or self.native == 'voiceworking':
            return False
        if self.route is not None:
            if self.route.poll() is None:
                return False
            self.route = None
        self.engine = active
        self.engine.context('FocusIn')
        self.engine.do_enable()
        self.ready.unlink(missing_ok=True)
        self.state = 'starting'
        self.start_sent = self.stop_sent = False
        self.route_stop_sent = self.has_result = False
        self.discard_result = False
        self.deadline = time.monotonic() + 4
        log = open(Path(os.environ['XDG_RUNTIME_DIR']) / 'iflytek-microphone-route.log', 'wb')
        try:
            self.route = subprocess.Popen(['/usr/local/libexec/native-iflytek-mic-route', '65'],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log)
        finally:
            log.close()
        return True

    def send_stop(self):
        if not self.stop_sent and self.capture_state() in ('opened', 'recording', 'streaming', 'stopped'):
            self.stop_sent = True
            self.trigger()

    def stop(self):
        if self.state in ('stopping', 'processing'):
            return True
        if self.state not in ('starting', 'recording') and self.native != 'voiceworking':
            self.release_route()
            return True
        self.state = 'stopping'
        self.deadline = time.monotonic() + 3
        self.send_stop()
        self.release_route()
        if not self.start_sent and not self.pcm_running():
            self.state = 'ready'
        return True

    def committed(self, engine):
        if engine is self.engine and self.state in ('starting', 'recording', 'stopping', 'processing'):
            self.has_result = True
            self.result_settle = time.monotonic() + .6
            if not self.pcm_running():
                self.release_route()
                self.state = 'processing'
                self.deadline = time.monotonic() + 15

    def emergency_stop(self):
        # A broken vendor stop must still release the microphone. Only signal
        # the exact, unprivileged Fcitx executable in the isolated runtime.
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                argv = (proc / 'cmdline').read_bytes().split(b'\0')
                if argv[0] == b'/usr/bin/fcitx' and os.readlink(proc / 'root') == str(TRIAL):
                    os.kill(int(proc.name), signal.SIGTERM)
            except OSError:
                pass
        self.fail('stop-error')

    def monitor(self):
        now = time.monotonic()
        if self.engine is not None and self.start_sent and self.state in (
                'starting', 'recording', 'stopping', 'processing'):
            try:
                # The vendor queues completed speech and uses XTest Pause to
                # wake its main thread. Deliver its existing wakeup key over
                # Fcitx DBus for this Wayland context, without X11 injection.
                self.engine.key(IBus.KEY_Pause)
            except GLib.Error:
                self.fail('service-error')
        if self.owns_panel and self.native is None and now - self.last_refresh >= 2:
            self.last_refresh = now
            self.emit('PanelCreated')
        if self.route is not None and self.route.poll() is not None:
            self.route = None
            if self.state == 'starting':
                self.fail('route-error')
            elif self.state == 'recording':
                self.stop()
        if self.state == 'starting':
            if not self.start_sent and self.route is not None:
                try:
                    route_ready = self.ready.read_text().strip() == str(self.route.pid)
                except OSError:
                    route_ready = False
                if route_ready:
                    self.start_sent = True
                    self.deadline = now + 8
                    self.trigger()
            if self.start_sent and self.pcm_running() and self.capture_state() == 'streaming':
                self.state = 'recording'
                self.deadline = now + 55
            if now >= self.deadline:
                self.fail('record-error')
        elif self.state == 'recording':
            if not self.pcm_running() or self.capture_state() in ('stopped', 'closed'):
                self.release_route()
                self.state = 'processing'
                self.deadline = now + 15
            elif now >= self.deadline:
                self.stop()
        elif self.state == 'stopping':
            self.send_stop()
            if not self.pcm_running() and (self.stop_sent or not self.start_sent):
                self.state = 'processing'
                self.deadline = now + 15
            elif now >= self.deadline:
                if self.pcm_running():
                    self.emergency_stop()
                else:
                    self.fail('record-error')
        elif self.state == 'processing':
            if self.has_result and not self.pcm_running() and now >= self.result_settle:
                self.release_route()
                self.state = 'ready'
            elif now >= self.deadline:
                self.fail('service-error')
        return True

    def close(self):
        self.stop()
        self.client.flush_sync(None)
        if self.route is not None:
            try:
                self.route.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
        self.client.close_sync(None)


def fcitx_connection():
    for path in sorted(ADDRESS_DIR.iterdir()):
        try:
            address = path.read_bytes().split(b'\0', 1)[0].decode()
            return Gio.DBusConnection.new_for_address_sync(
                address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
                Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        except (OSError, UnicodeError, GLib.Error):
            continue
    raise RuntimeError('Official iFlytek Fcitx backend is unavailable')


class Engine(IBus.Engine):
    __gtype_name__ = 'Le620OfficialIflytekEngine'

    def __init__(self, **kwargs):
        kwargs['has_focus_id'] = True
        super().__init__(**kwargs)
        self.client = fcitx_connection()
        self.path = None
        self.chinese = True
        self.shift_used = False
        self.secure = False
        self.layout = 26
        self.preedit = ''
        self.space = SpaceHold(GLib.timeout_add, GLib.source_remove,
                               self.short_space, self.hold_voice, self.stop_voice)
        result = self.call('/inputmethod', METHOD, 'CreateICv3',
                           GLib.Variant('(si)', ('le620-phosh-ibus', os.getpid())))
        self.path = '/inputcontext_' + str(result[0])
        self.subscription = self.client.signal_subscribe(
            FCITX, CONTEXT, None, self.path, None, Gio.DBusSignalFlags.NONE,
            self.received, None)
        self.context('SetCapacity', GLib.Variant('(u)', (3,)))
        engines.add(self)

    def call(self, path, interface, member, parameters=None):
        return self.client.call_sync(FCITX, path, interface, member, parameters,
                                     None, Gio.DBusCallFlags.NONE, 2500, None).unpack()

    def context(self, member, parameters=None):
        return self.call(self.path, CONTEXT, member, parameters)

    def key(self, key, state=0, release=False):
        return bool(self.context('ProcessKeyEvent', GLib.Variant(
            '(uuuiu)', (key, 0, state & 0x0fffffff, int(release), 0)))[0])

    def hotkey(self):
        self.context('FocusIn')
        self.context('EnableIC')
        config = configparser.ConfigParser(interpolation=None)
        config.read(ADDRESS_DIR.parent.parent / 'iflytek/inputmethod.ini')
        parts = config.get('shortcut', 'speech_shortcut_value', fallback='Ctrl+Shift+Z').lower().split('+')
        state = (4 if 'ctrl' in parts else 0) | (1 if 'shift' in parts else 0) | (8 if 'alt' in parts else 0)
        self.key(ord(parts[-1]), state)
        self.key(ord(parts[-1]), state, True)

    def use_layout(self, layout):
        if layout != self.layout:
            self.context('Reset')
            self.preedit = ''
        self.layout = layout
        path = Path(os.environ['XDG_RUNTIME_DIR']) / 'iflytek-keyboard-layout'
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(str(layout))

    def short_space(self):
        if self.chinese and not self.secure:
            handled = self.key(IBus.KEY_space)
            self.key(IBus.KEY_space, release=True)
            if handled:
                return
        self.commit_text(IBus.Text.new_from_string(' '))

    def hold_voice(self):
        if voice and active is self and not self.secure:
            # Finish an existing Pinyin composition before beginning dictation.
            if self.preedit:
                self.short_space()
            try:
                voice.start()
            except (OSError, GLib.Error):
                voice.fail('service-error')

    def stop_voice(self):
        if voice and voice.engine is self:
            voice.stop()

    def received(self, connection, sender, path, interface, member, parameters, data):
        values = parameters.unpack()
        if member == 'CommitString':
            # Never insert a late cloud result into a different focused field.
            discarded = (voice and voice.engine is self and voice.discard_result and
                         voice.state in ('starting', 'recording', 'stopping', 'processing'))
            if active is self and not self.secure and not discarded:
                self.commit_text(IBus.Text.new_from_string(values[0]))
            self.preedit = ''
            self.hide_preedit_text()
            self.hide_lookup_table()
            if voice:
                voice.committed(self)
        elif member == 'UpdateClientSideUI':
            _up, _down, preedit, candidates, _name, cursor = values
            self.preedit = preedit
            self.update_preedit_text(IBus.Text.new_from_string(preedit),
                                     min(len(preedit), max(0, cursor)), bool(preedit))
            items = re.split(r'(?:^|\s)[1-9][0-9]*[.．]', candidates)[1:]
            table = IBus.LookupTable.new(5, 0, True, False)
            for item in items[:5]:
                table.append_candidate(IBus.Text.new_from_string(item.strip()))
            self.update_lookup_table(table, bool(items))
        elif member == 'ForwardKey':
            key, state, event_type = values
            self.forward_key_event(key, 0, state | ((1 << 30) if event_type else 0))

    def do_process_key_event(self, keyval, keycode, state):
        release = bool(state & (1 << 30))
        try:
            if self.secure:
                return False
            if keyval == IBus.KEY_space and (self.space.down or not state & (4 | 8 | (1 << 26))):
                if release:
                    self.space.release()
                else:
                    self.space.press()
                return True
            if not release and self.space.down:
                self.space.cancel()
            # These keysyms exist only on the on-screen 9-key Pinyin view.
            if IBus.KEY_F13 <= keyval <= IBus.KEY_F20:
                if not release:
                    self.chinese = True
                    self.use_layout(9)
                    self.key(ord('a') + keyval - IBus.KEY_F13)
                return True
            # Squeekboard's 中 button generates a complete Shift key tap.
            if keyval in (IBus.KEY_Shift_L, IBus.KEY_Shift_R):
                if not release:
                    self.shift_used = False
                elif not self.shift_used:
                    self.context('Reset')
                    self.chinese = not self.chinese
                    self.hide_preedit_text()
                    self.hide_lookup_table()
                return True
            if not release and state & 1:
                self.shift_used = True
            if not self.chinese:
                return False
            if not release and (ord('a') <= keyval <= ord('z') or ord('A') <= keyval <= ord('Z')):
                self.use_layout(26)
            return self.key(keyval, state, release)
        except GLib.Error:
            return False

    def do_enable(self):
        self.context('EnableIC')
        self.call('/inputmethod', METHOD, 'SetCurrentIM', GLib.Variant('(s)', ('iflyime',)))
        self.call('/inputmethod', METHOD, 'ActivateIM')

    def do_focus_in(self):
        global active
        active = self
        self.context('FocusIn')
        self.do_enable()
        self.use_layout(self.layout)

    def do_focus_in_id(self, object_path, client):
        real_input = not client.startswith('fake')
        if real_input:
            self.do_focus_in()
        else:
            self.do_focus_out()
        Gio.bus_get_sync(Gio.BusType.SESSION, None).emit_signal(
            None, '/org/le620/Iflytek', SERVICE, 'InputFocus',
            GLib.Variant('(sb)', (object_path, real_input)))

    def do_focus_out_id(self, object_path):
        self.do_focus_out()

    def do_focus_out(self):
        global active
        self.space.cancel()
        if voice and voice.engine is self:
            voice.discard_result = True
        if active is self:
            active = None
        if voice and voice.engine is self and voice.state in ('starting', 'recording'):
            voice.stop()
        if voice is None or voice.engine is not self:
            self.context('FocusOut')
        self.hide_lookup_table()

    def do_reset(self):
        self.space.cancel()
        self.context('Reset')
        self.preedit = ''
        self.hide_preedit_text()
        self.hide_lookup_table()
        if active is self:
            # A reset alone may be programmatic. The panel also requires a
            # fresh physical press before reopening a manually hidden OSK.
            Gio.bus_get_sync(Gio.BusType.SESSION, None).emit_signal(
                None, '/org/le620/Iflytek', SERVICE, 'InputReset', None)

    def do_candidate_clicked(self, index, button, state):
        if 0 <= index < 5:
            self.key(ord('1') + index)

    def do_page_up(self):
        self.key(IBus.KEY_Page_Up)

    def do_page_down(self):
        self.key(IBus.KEY_Page_Down)

    def do_destroy(self):
        global active
        self.space.cancel()
        if active is self:
            active = None
        if voice and voice.engine is self:
            voice.stop()
            voice.engine = None
        engines.discard(self)
        try:
            self.context('DestroyIC')
        except GLib.Error:
            pass
        self.client.signal_unsubscribe(self.subscription)
        self.client.close_sync(None)
        super().do_destroy()

    def do_set_content_type(self, purpose, hints):
        self.secure = purpose in (int(IBus.InputPurpose.PASSWORD), int(IBus.InputPurpose.PIN))
        if self.secure:
            self.space.cancel()
            self.stop_voice()
            self.do_reset()


def main():
    global voice
    IBus.init()
    bus = IBus.Bus.new()
    if not bus.is_connected():
        raise SystemExit('IBus unavailable')
    voice = Voice()
    factory = IBus.Factory.new(bus.get_connection())
    factory.add_engine('iflytek', Engine.__gtype__)
    component = IBus.Component.new('org.le620.Iflytek', 'Official iFlytek bridge',
        '1.0', 'MIT (bridge only)', 'le620 project', '', '', '')
    component.add_engine(IBus.EngineDesc.new('iflytek', '讯飞输入法',
        '官方讯飞拼音与在线语音', 'zh_CN', 'Proprietary (official engine)',
        'iFlytek', 'input-keyboard', 'us'))
    if not bus.register_component(component):
        raise SystemExit('Unable to register official engine bridge')
    session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    xml = '''<node><interface name="org.le620.Iflytek">
      <method name="VoiceStart"><arg direction="out" type="b" name="started"/></method>
      <method name="VoiceStop"><arg direction="out" type="b" name="stopped"/></method>
      <method name="Status"><arg direction="out" type="s" name="status"/></method>
      <signal name="InputReset"/>
      <signal name="InputFocus"><arg type="s" name="context"/><arg type="b" name="editable"/></signal>
    </interface></node>'''
    interface = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
    def method(connection, sender, path, iface, name, parameters, invocation):
        if name in ('VoiceStart', 'VoiceStop'):
            try:
                ok = voice.start() if name == 'VoiceStart' else voice.stop()
            except (OSError, GLib.Error):
                voice.fail('service-error')
                ok = False
            invocation.return_value(GLib.Variant('(b)', (ok,)))
        else:
            invocation.return_value(GLib.Variant('(s)', (voice.state,)))
    session.register_object('/org/le620/Iflytek', interface, method, None, None)
    Gio.bus_own_name_on_connection(session, SERVICE, Gio.BusNameOwnerFlags.NONE, None, None)
    loop = GLib.MainLoop()
    GLib.timeout_add(100, voice.monitor)
    for sig in (signal.SIGTERM, signal.SIGINT):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: (loop.quit(), False)[1])
    bus.connect('disconnected', lambda _bus: loop.quit())
    try:
        loop.run()
    finally:
        voice.close()


if __name__ == '__main__':
    main()
