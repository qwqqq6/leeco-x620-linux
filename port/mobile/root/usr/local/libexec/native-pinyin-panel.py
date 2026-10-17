#!/usr/bin/python3
"""IBus candidate panel on a non-focusing Wayland layer surface."""
import os
import ctypes as C
import time

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("GtkLayerShell", "0.1")
gi.require_version("IBus", "1.0")
from gi.repository import Gtk, GtkLayerShell, IBus, Pango, Gio, GLib


class InputPress:
    """Observe presses on the nested X11 seat without grabbing user input."""
    class Cookie(C.Structure):
        _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                    ('display', C.c_void_p), ('extension', C.c_int), ('evtype', C.c_int),
                    ('cookie', C.c_uint), ('data', C.c_void_p)]

    class Mask(C.Structure):
        _fields_ = [('deviceid', C.c_int), ('mask_len', C.c_int),
                    ('mask', C.POINTER(C.c_ubyte))]

    class Raw(C.Structure):
        _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                    ('display', C.c_void_p), ('extension', C.c_int), ('evtype', C.c_int),
                    ('time', C.c_ulong), ('deviceid', C.c_int), ('sourceid', C.c_int),
                    ('detail', C.c_int)]

    def __init__(self):
        self.last_press = 0
        self.display = None
        self.source = 0
        try:
            self.connect()
        except (OSError, RuntimeError):
            self.close()
            print('input tap watcher unavailable', flush=True)

    def connect(self):
        self.x = C.CDLL('libX11.so.6')
        xi = C.CDLL('libXi.so.6')
        self.x.XOpenDisplay.argtypes = [C.c_char_p]
        self.x.XOpenDisplay.restype = C.c_void_p
        self.x.XDefaultRootWindow.argtypes = [C.c_void_p]
        self.x.XDefaultRootWindow.restype = C.c_ulong
        self.x.XConnectionNumber.argtypes = [C.c_void_p]
        self.x.XQueryExtension.argtypes = [C.c_void_p, C.c_char_p,
                                         C.POINTER(C.c_int), C.POINTER(C.c_int), C.POINTER(C.c_int)]
        for name in ['XPending', 'XFlush', 'XCloseDisplay']:
            getattr(self.x, name).argtypes = [C.c_void_p]
        self.x.XNextEvent.argtypes = [C.c_void_p, C.c_void_p]
        for name in ['XGetEventData', 'XFreeEventData']:
            getattr(self.x, name).argtypes = [C.c_void_p, C.POINTER(self.Cookie)]
        xi.XIQueryVersion.argtypes = [C.c_void_p, C.POINTER(C.c_int), C.POINTER(C.c_int)]
        xi.XISelectEvents.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(self.Mask), C.c_int]
        self.display = self.x.XOpenDisplay(os.environ.get('DISPLAY', ':0').encode())
        if not self.display:
            raise RuntimeError('X11 seat unavailable')
        opcode, event, error = C.c_int(), C.c_int(), C.c_int()
        if not self.x.XQueryExtension(self.display, b'XInputExtension',
                                     C.byref(opcode), C.byref(event), C.byref(error)):
            raise RuntimeError('XInput unavailable')
        self.opcode = opcode.value
        major, minor = C.c_int(2), C.c_int(2)
        if xi.XIQueryVersion(self.display, C.byref(major), C.byref(minor)):
            raise RuntimeError('XInput2 unavailable')
        self.events = [15]  # XI_RawButtonPress, including emulated touch presses.
        if (major.value, minor.value) >= (2, 2):
            self.events.append(22)  # XI_RawTouchBegin.
        bits = (C.c_ubyte * 3)()
        for value in self.events:
            bits[value // 8] |= 1 << (value % 8)
        mask = self.Mask(1, len(bits), bits)  # XIAllMasterDevices, no grab.
        if xi.XISelectEvents(self.display, self.x.XDefaultRootWindow(self.display), C.byref(mask), 1):
            raise RuntimeError('Cannot observe XInput2')
        self.x.XFlush(self.display)
        self.source = GLib.io_add_watch(self.x.XConnectionNumber(self.display),
            GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, self.read)

    def read(self, _fd, condition):
        if condition & (GLib.IO_HUP | GLib.IO_ERR):
            self.source = 0
            self.close()
            return False
        event = (C.c_long * 24)()
        while self.x.XPending(self.display):
            self.x.XNextEvent(self.display, C.byref(event))
            cookie = C.cast(C.byref(event), C.POINTER(self.Cookie))
            if cookie.contents.type != 35 or cookie.contents.extension != self.opcode:
                continue
            if self.x.XGetEventData(self.display, cookie):
                kind = cookie.contents.evtype
                if kind == 22 or (kind == 15 and
                        C.cast(cookie.contents.data, C.POINTER(self.Raw)).contents.detail == 1):
                    # Do not inspect coordinates, keys or application contents.
                    self.last_press = time.monotonic()
                self.x.XFreeEventData(self.display, cookie)
        return True

    def close(self):
        if self.source:
            GLib.source_remove(self.source)
            self.source = 0
        if self.display:
            self.x.XCloseDisplay(self.display)
            self.display = None


class Panel(IBus.PanelService):
    def __init__(self, bus):
        super().__init__(connection=bus.get_connection(), object_path=IBus.PATH_PANEL)
        self.ibus = bus
        self.session = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.hide_source = 0
        self.focused = False
        self.context = None
        self.input_contexts = {}
        self.dismissed_context = None
        self.dismissed_at = time.monotonic()
        self.input_press = InputPress()
        self.tap_source = 0
        self.session.signal_subscribe('org.le620.Iflytek', 'org.le620.Iflytek',
            'InputReset', '/org/le620/Iflytek', None, Gio.DBusSignalFlags.NONE,
            self.input_reset, None)
        self.session.signal_subscribe('org.le620.Iflytek', 'org.le620.Iflytek',
            'InputFocus', '/org/le620/Iflytek', None, Gio.DBusSignalFlags.NONE,
            self.input_focus, None)
        self.osk = Gio.DBusProxy.new_sync(
            self.session, Gio.DBusProxyFlags.DO_NOT_AUTO_START, None,
            "sm.puri.OSK0", "/sm/puri/OSK0", "sm.puri.OSK0", None)
        visible = self.osk.get_cached_property("Visible")
        self.osk_visible = bool(visible and visible.unpack())
        self.voice_state = 'unavailable'
        self.window = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
        self.window.set_title("中文候选字")
        self.window.set_accept_focus(False)
        GtkLayerShell.init_for_window(self.window)
        GtkLayerShell.set_namespace(self.window, "native-pinyin-candidates")
        GtkLayerShell.set_layer(self.window, GtkLayerShell.Layer.OVERLAY)
        GtkLayerShell.set_keyboard_mode(self.window, GtkLayerShell.KeyboardMode.NONE)
        for edge in (GtkLayerShell.Edge.BOTTOM, GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self.window, edge, True)
        # Zone 0 follows the usable area, placing this above the OSK's reserved area.
        GtkLayerShell.set_exclusive_zone(self.window, 0)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        box.set_border_width(4)
        self.window.add(box)
        header = Gtk.Box(spacing=6)
        self.preedit = Gtk.Label(xalign=0)
        self.preedit.set_line_wrap(True)
        self.preedit.set_line_wrap_mode(Pango.WrapMode.WORD_CHAR)
        header.pack_start(self.preedit, True, True, 3)
        self.voice = Gtk.Button(label="停止")
        self.voice.set_can_focus(False)
        self.voice.set_size_request(58, 34)
        self.voice.connect("clicked", self.voice_clicked)
        header.pack_start(self.voice, False, False, 0)
        for title, method in (("‹", self.page_up), ("›", self.page_down)):
            button = Gtk.Button(label=title)
            button.set_size_request(38, 32)
            button.set_can_focus(False)
            button.connect("clicked", lambda _button, fn=method: fn())
            header.pack_start(button, False, False, 0)
        self.dismiss = Gtk.Button(label="收起")
        self.dismiss.set_can_focus(False)
        self.dismiss.set_size_request(56, 34)
        self.dismiss.connect("clicked", self.dismiss_clicked)
        header.pack_start(self.dismiss, False, False, 0)
        box.pack_start(header, False, False, 0)
        self.row = Gtk.FlowBox()
        self.row.set_selection_mode(Gtk.SelectionMode.NONE)
        self.row.set_can_focus(False)
        self.row.set_homogeneous(False)
        self.row.set_min_children_per_line(2)
        self.row.set_max_children_per_line(5)
        self.row.set_row_spacing(3)
        self.row.set_column_spacing(3)
        box.pack_start(self.row, False, False, 0)
        self.buttons = []
        for index in range(5):
            button = Gtk.Button()
            label = Gtk.Label()
            label.set_ellipsize(Pango.EllipsizeMode.NONE)
            label.set_line_wrap(True)
            label.set_line_wrap_mode(Pango.WrapMode.WORD_CHAR)
            label.set_max_width_chars(20)
            label.set_justify(Gtk.Justification.CENTER)
            button.add(label)
            button.set_size_request(0, 42)
            button.set_can_focus(False)
            button.connect("clicked", lambda _button, i=index: self.candidate_clicked(i, 1, 0))
            self.row.add(button)
            button.get_parent().set_can_focus(False)
            self.buttons.append((button, label))
        box.show_all()
        self.row.hide()
        self.voice.hide()
        self.osk.connect("g-properties-changed", self.osk_changed)
        self.osk.connect("notify::g-name-owner", self.osk_changed)
        GLib.timeout_add(250, self.voice_status)

    def commercial(self):
        engine = self.ibus.get_global_engine()
        return engine is not None and engine.get_name() == 'iflytek'

    def update_visibility(self):
        self.window.set_visible(
            self.focused and self.osk_visible
            and self.dismissed_context != self.context
            and (self.commercial() or self.row.get_visible()))

    def stop_active_voice(self):
        if self.commercial():
            # VoiceStop is idempotent: hiding must never toggle recording on.
            self.session.call('org.le620.Iflytek', '/org/le620/Iflytek',
                'org.le620.Iflytek', 'VoiceStop', None, None,
                Gio.DBusCallFlags.NONE, 2500, None, None, None)

    def osk_changed(self, *_args):
        visible = self.osk.get_cached_property("Visible")
        was_visible = self.osk_visible
        self.osk_visible = bool(self.osk.get_name_owner() and visible and visible.unpack())
        if was_visible and not self.osk_visible:
            self.dismissed_at = time.monotonic()
            if self.focused:
                self.dismissed_context = self.context
            self.stop_active_voice()
        elif self.osk_visible and not was_visible:
            self.dismissed_context = None
        self.update_visibility()

    def dismiss_clicked(self, _button):
        self.dismissed_at = time.monotonic()
        self.dismissed_context = self.context
        self.window.hide()
        self.stop_active_voice()
        self.set_osk(False)

    def input_reset(self, *_args):
        # Only the bridge's active real input context emits this signal.
        if self.focused:
            self.input_contexts[self.context] = True
        if self.focused and not self.osk_visible and not self.tap_source:
            # The app's reset and raw press arrive on different connections.
            self.tap_source = GLib.timeout_add(40, self.show_from_input_press)

    def input_focus(self, connection, sender, path, interface, name, parameters, user_data):
        context, editable = parameters.unpack()
        self.input_contexts[context] = editable
        if self.focused and context == self.context:
            if not editable:
                self.set_osk(False)
            elif self.dismissed_context != context:
                self.set_osk(True)
            self.update_visibility()

    def show_from_input_press(self):
        self.tap_source = 0
        pressed = self.input_press.last_press
        if (self.focused and not self.osk_visible and pressed > self.dismissed_at
                and self.input_contexts.get(self.context, False)
                and time.monotonic() - pressed < .75):
            self.dismissed_context = None
            self.set_osk(True)
        return False

    def voice_clicked(self, button):
        # Recording begins only by holding Space. This remains a stop escape.
        method = 'VoiceStop'
        try:
            value = self.session.call_sync('org.le620.Iflytek', '/org/le620/Iflytek',
                'org.le620.Iflytek', method, None, None,
                Gio.DBusCallFlags.NONE, 2500, None).unpack()[0]
            if not value:
                self.preedit.set_text('请先点一下输入区')
            self.voice_status()
        except GLib.Error:
            self.preedit.set_text('语音服务暂不可用')

    def voice_status(self):
        if self.focused and self.commercial():
            try:
                value = self.session.call_sync('org.le620.Iflytek', '/org/le620/Iflytek',
                    'org.le620.Iflytek', 'Status', None, None,
                    Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
                changed = value != self.voice_state
                self.voice_state = value
                labels = {'starting': '取消', 'recording': '停止', 'stopping': '停止中',
                          'processing': '识别中', 'unavailable': '不可用', 'disabled': '不可用',
                          'stop-error': '不可用'}
                self.voice.set_label(labels.get(value, '语音'))
                self.voice.set_visible(value in ('starting', 'recording', 'stopping', 'processing'))
                self.voice.set_sensitive(value not in ('stopping', 'processing', 'unavailable', 'disabled', 'stop-error'))
                messages = {'starting': '正在打开麦克风…', 'recording': '请说话，松开空格结束',
                            'stopping': '正在关闭麦克风…', 'processing': '录音已停止，等待识别…',
                            'record-error': '无法启动录音，请重试', 'route-error': '麦克风暂不可用',
                            'service-error': '识别服务未成功，请重试',
                            'stop-error': '录音已强制停止，请重启输入法',
                            'unavailable': '语音服务暂不可用', 'disabled': '语音暂不可用，拼音可正常使用',
                            'ready': '讯飞'}
                if changed or value != 'ready':
                    self.preedit.set_text(messages.get(value, '讯飞输入法'))
            except GLib.Error:
                self.voice_state = 'unavailable'
                self.voice.set_label('不可用')
                self.voice.set_sensitive(False)
        return True

    def clear_candidates(self):
        self.row.hide()
        if self.focused and self.commercial():
            self.preedit.set_text('讯飞')
            self.voice.set_visible(self.voice_state in ('starting', 'recording', 'stopping', 'processing'))
        self.update_visibility()

    def do_update_preedit_text(self, text, cursor, visible):
        if visible:
            self.preedit.set_text(text.get_text())
        elif self.commercial():
            self.preedit.set_text('讯飞')

    def do_update_auxiliary_text(self, text, visible):
        if visible:
            self.preedit.set_text(text.get_text())

    def do_update_lookup_table(self, table, visible):
        if not visible or not table.get_number_of_candidates():
            self.clear_candidates()
            return
        page_size = table.get_page_size()
        start = table.get_cursor_pos() // page_size * page_size
        for index, (button, label) in enumerate(self.buttons):
            position = start + index
            if index < page_size and position < table.get_number_of_candidates():
                value = table.get_candidate(position).get_text()
                label.set_text(value)
                button.set_tooltip_text(value)
                button.set_sensitive(True)
                button.get_parent().show()
            else:
                label.set_text("")
                button.set_sensitive(False)
                button.get_parent().hide()
        self.row.show()
        self.voice.set_visible(self.commercial() and self.voice_state in ('starting', 'recording', 'stopping', 'processing'))
        self.update_visibility()

    def do_hide_lookup_table(self):
        self.clear_candidates()

    def do_focus_out(self, context):
        self.focused = False
        self.window.hide()
        if self.hide_source:
            GLib.source_remove(self.hide_source)
        self.hide_source = GLib.timeout_add(250, self.hide_osk)

    def set_osk(self, visible):
        self.session.call("sm.puri.OSK0", "/sm/puri/OSK0", "sm.puri.OSK0",
                          "SetVisible", GLib.Variant("(b)", (visible,)), None,
                          Gio.DBusCallFlags.NONE, 2000, None, None, None)

    def hide_osk(self):
        self.hide_source = 0
        # Only a settled focus loss clears a manual dismissal. Brief focus
        # changes from shell controls must not immediately reopen the OSK.
        self.dismissed_context = None
        self.set_osk(False)
        return False

    def do_focus_in(self, context):
        new_focus = not self.focused or context != self.context
        if context != self.context:
            self.dismissed_context = None
        self.context = context
        self.focused = True
        if self.hide_source:
            GLib.source_remove(self.hide_source)
            self.hide_source = 0
        editable = self.input_contexts.get(context, not self.commercial())
        if not editable and context in self.input_contexts:
            self.set_osk(False)
        elif new_focus and editable and self.dismissed_context != context:
            self.set_osk(True)
        self.clear_candidates()

    def do_destroy_context(self, context):
        self.input_contexts.pop(context, None)


def main():
    IBus.init()
    bus = IBus.Bus.new()
    if not bus.is_connected():
        raise SystemExit("IBus connection unavailable")
    panel = Panel(bus)
    bus.request_name(IBus.SERVICE_PANEL, 0)
    bus.connect("disconnected", lambda _bus: Gtk.main_quit())
    try:
        Gtk.main()
    finally:
        panel.input_press.close()


if __name__ == "__main__":
    main()
