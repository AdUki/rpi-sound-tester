#!/bin/sh
# Generate the ssh host keys once, on the data partition, so they stay stable across
# reboots. /data is normally mounted read-only; flip it just long enough to write.
#
# If /data did not mount (missing, corrupt, or a fresh card whose partition is not there),
# fall back to a tmpfs so sshd still gets host keys and the box stays reachable.
set -e

KEYDIR=/data/ssh

if ! mountpoint -q /data; then
    echo "soundtester: /data is not mounted — using a tmpfs for ssh host keys (they will"
    echo "soundtester: change on every boot; check the data partition)"
    mount -t tmpfs -o mode=0755,size=1M tmpfs /data
fi

if [ -f "$KEYDIR/ssh_host_ed25519_key" ] && [ -f "$KEYDIR/ssh_host_rsa_key" ]; then
    exit 0
fi

echo "soundtester: generating ssh host keys in $KEYDIR"
mount -o remount,rw /data 2>/dev/null || true
mkdir -p "$KEYDIR"
[ -f "$KEYDIR/ssh_host_ed25519_key" ] || ssh-keygen -q -t ed25519 -N '' -f "$KEYDIR/ssh_host_ed25519_key"
[ -f "$KEYDIR/ssh_host_rsa_key" ]     || ssh-keygen -q -t rsa -b 2048 -N '' -f "$KEYDIR/ssh_host_rsa_key"
chmod 600 "$KEYDIR"/ssh_host_*_key
sync
mount -o remount,ro /data 2>/dev/null || true
