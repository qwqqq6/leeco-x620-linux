#!/bin/sh
# MTK command-mode panels need an explicit pan to submit framebuffer damage.
# The native worker paces submissions at 60 Hz without a fork/exec each frame.
set -e
echo 0 >/sys/class/graphics/fb0/blank
echo 120 >/sys/class/leds/lcd-backlight/brightness
exec /usr/local/libexec/native-display-refresh --fps 60
