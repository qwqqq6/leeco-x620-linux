"""Space press/hold/release state machine; no microphone or GUI dependencies."""


class SpaceHold:
    def __init__(self, schedule, remove, insert_space, start, stop, delay=450):
        self.schedule, self.remove = schedule, remove
        self.insert_space, self.start, self.stop = insert_space, start, stop
        self.delay = delay
        self.timer = 0
        self.down = self.held = False

    def press(self):
        if not self.down:  # Ignore autorepeat while the same finger is down.
            self.down, self.held = True, False
            self.timer = self.schedule(self.delay, self.on_hold)

    def on_hold(self):
        self.timer = 0
        if self.down:
            self.held = True
            self.start()
        return False

    def release(self):
        if not self.down:
            return
        self.down = False
        if self.timer:
            self.remove(self.timer)
            self.timer = 0
        if self.held:
            self.stop()
        else:
            self.insert_space()
        self.held = False

    def cancel(self):
        if self.timer:
            self.remove(self.timer)
            self.timer = 0
        self.down = False
        if self.held:
            self.stop()
        self.held = False
