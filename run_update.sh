#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#
# run_update.sh - run the whole Android Auto update from a laptop. The head unit needs SSH already open
# (the one-time unlock from install.md) - the point is that no terminal session is needed during the update
# itself: one command does stop nbt-aad, swap the daemon + plugin, install the firewall rules, and reboot.
#
#   ./run_update.sh 169.254.199.99 /path/to/updates [hu-root-password]
#
# /path/to/updates is the release layout from the Releases page (install.md "Prepared Flash Drive"):
#   nbt-aad        the new daemon
#   pf_aa          the firewall installer
#   update_aa      the in-car updater (ships with the release)
#   Releases/      the five plugin files (libNbtAad.so, NbtAad.manifest, NbtAad.xml, NbtAad_ALL_Texts.zip,
#                  NbtAad_ALL_Images.zip)
#
# It copies the release files to the head unit's RAM disk (/dev/shmem, gone on reboot, so nothing sticks
# around) and runs the release's own update_aa on the stick there; that tool reboots the unit, so the
# connection dropping at the end is expected, not an error. A password as the third argument is piped to
# ssh/scp via sshpass when it is installed; otherwise the usual password prompt appears once.
#
# No terminal session at all? update_aa ships on the flash drive next to the other files: put it there,
# plug the stick in, and press "Update from USB" in the Android Auto iDrive menu.

set -eu
HU=${1:?usage: run_update.sh <head-unit-ip> <updates-folder> [hu-root-password]}
DIR=${2:?usage: run_update.sh <head-unit-ip> <updates-folder> [hu-root-password]}
[ -d "$DIR" ] || { echo "no such folder: $DIR" >&2; exit 1; }
for f in nbt-aad pf_aa Releases; do
    [ -e "$DIR/$f" ] || { echo "missing in $DIR: $f (need nbt-aad, pf_aa, and a Releases/ folder with the five plugin files)" >&2; exit 1; }
done

SSH_OPTS="-o HostKeyAlgorithms=+ssh-rsa,+ssh-dss -o PreferredAuthentications=password -o PubkeyAuthentication=no -m hmac-sha1 -o StrictHostKeyChecking=accept-new"

SSH=ssh
SCP=scp
if [ "${3:-}" ]; then
    # Password given: use sshpass if present, otherwise fall through to interactive.
    if command -v sshpass >/dev/null 2>&1; then
        SSH="sshpass -p $3 ssh"
        SCP="sshpass -p $3 scp"
    else
        echo "note: sshpass not installed, the password will be asked interactively."
        sleep 2
    fi
fi

remote() {
    $SSH $SSH_OPTS root@"$HU" "$@"
}
remote_cp() {
    $SCP $SSH_OPTS "$@"
}

echo "[run_update] connecting to $HU ..."
remote "true"

echo "[run_update] copying the update files to the head unit's RAM disk ..."
remote "rm -rf /dev/shmem/update-stick && mkdir -p /dev/shmem/update-stick/Releases"
remote_cp "$DIR/nbt-aad" "root@$HU:/dev/shmem/update-stick/"
remote_cp "$DIR/pf_aa"   "root@$HU:/dev/shmem/update-stick/"
remote_cp "$DIR/Releases/"* "root@$HU:/dev/shmem/update-stick/Releases/"
remote "chmod +x /dev/shmem/update-stick/nbt-aad /dev/shmem/update-stick/pf_aa"

echo "[run_update] running the release's update_aa on the head unit (stops nbt-aad, swaps the daemon + plugin, installs the firewall rules, reboots) ..."
# update_aa reboots the head unit, so the ssh connection drops mid-command; that is expected, not an error.
set +e
if [ -f "$DIR/update_aa" ]; then
    remote_cp "$DIR/update_aa" "root@$HU:/dev/shmem/update-stick/"
    remote "chmod +x /dev/shmem/update-stick/update_aa"
    remote "/dev/shmem/update-stick/update_aa /dev/shmem/update-stick"
else
    echo "note: update_aa not in the release folder: files are staged on the head unit, but there is no updater on the stick to swap them."
    remote "echo 1"
    echo "[run_update] finish by hand on the head unit: run 'on /net/hu-omap/ /bin/sh' over SSH and follow updating.md steps 3-6."
fi
set -e

echo "[run_update] done. The head unit is rebooting; check Android Auto in Media/Radio after it comes back up."
echo "[run_update] verify: ssh into $HU again and run: grep Build /dev/shmem/nbt-aad.log"
