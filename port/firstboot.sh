#!/bin/sh
# Release images contain no shared host keys or machine identity.
set -eu
umask 077
mkdir -p /etc/dropbear /root/.ssh /home/desktop /var/lib/dbus /var/lib/native-bluetooth
chmod 700 /root /root/.ssh /var/lib/native-bluetooth
chown 1000:100 /home/desktop
chmod 700 /home/desktop
if [ ! -s /etc/dropbear/dropbear_ed25519_host_key ]; then
    dropbearkey -t ed25519 -f /etc/dropbear/dropbear_ed25519_host_key >/dev/null 2>&1
fi
dbus-uuidgen --ensure=/etc/machine-id
ln -sf /etc/machine-id /var/lib/dbus/machine-id
mkdir -p /home/desktop/.codex /home/desktop/Projects/codex
if [ ! -f /home/desktop/.codex/config.toml ]; then
    cp /etc/native-mobile/codex-config.toml /home/desktop/.codex/config.toml
fi
chown -R 1000:100 /home/desktop/.codex /home/desktop/Projects
chmod 700 /home/desktop/.codex
chmod 600 /home/desktop/.codex/config.toml
