#!/bin/bash
# Run once as root inside the offline Debian root with the source directory
# available. The user bus below is private to this offline install.
set -euo pipefail
user=${1:?Pass the intended GNOME account name}
cd "$(dirname "$(readlink -f "$0")")"
test "$(id -u)" = 0
test "$(dpkg-query -W -f='${Version}' gnome-shell)" = 48.7-0+deb13u2
getent passwd "$user" >/dev/null
extension=/usr/share/gnome-shell/extensions/t5-sleep-mode@mediapad
schema=/usr/share/glib-2.0/schemas
backup=/var/backups/t5-power-mode-v105
user_home=$(getent passwd "$user" | cut -d: -f6)
case "$user_home" in /home/*) ;; *) exit 1;; esac
if [ ! -e "$backup/original.tar" ]; then
    install -d -m 0700 "$backup"
    # Record absent files as well as originals. Reinstalling must never replace
    # the pre-deployment snapshot with one of our own versions.
    paths=("$extension/metadata.json" "$extension/extension.js"
        "$schema/org.gnome.shell.extensions.t5-sleep-mode.gschema.xml"
        "$schema/90-mediapad-t5-power.gschema.override" /usr/local/libexec/t5-power-key
        /usr/lib/systemd/system-sleep/60-t5-power /etc/systemd/logind.conf.d/80-mediapad-t5-power.conf
        /etc/systemd/system/t5-power-key.service /etc/systemd/system/t5-power-display-resume.service
        /etc/systemd/system/multi-user.target.wants/t5-power-key.service
        /etc/systemd/system/suspend.target.wants/t5-power-display-resume.service
        /usr/local/libexec/t5-power-activate "$user_home/.config/dconf/user")
    : >"$backup/present.list"
    : >"$backup/absent.list"
    for dst in "${paths[@]}"; do
        if [ -e "$dst" ] || [ -L "$dst" ]; then
            printf '%s\n' "${dst#/}" >>"$backup/present.list"
        else
            printf '%s\n' "${dst#/}" >>"$backup/absent.list"
        fi
    done
    tar -C / --verbatim-files-from -cf "$backup/original.tar.tmp" -T "$backup/present.list"
    tar -tf "$backup/original.tar.tmp" >/dev/null
    mv "$backup/original.tar.tmp" "$backup/original.tar"
    (cd "$backup"; sha256sum original.tar >SHA256SUMS)
else
    (cd "$backup"; sha256sum -c SHA256SUMS)
fi
deploy() {
    local src=$1 dst=$2 mode=$3
    install -D -m "$mode" "$src" "$dst"
}
deploy metadata.json "$extension/metadata.json" 0644
deploy extension.js "$extension/extension.js" 0644
deploy org.gnome.shell.extensions.t5-sleep-mode.gschema.xml "$schema/org.gnome.shell.extensions.t5-sleep-mode.gschema.xml" 0644
deploy 90-mediapad-t5-power.gschema.override "$schema/90-mediapad-t5-power.gschema.override" 0644
deploy power-key.py /usr/local/libexec/t5-power-key 0755
deploy activate-user.sh /usr/local/libexec/t5-power-activate 0755
deploy sleep-hook /usr/lib/systemd/system-sleep/60-t5-power 0755
deploy logind.conf /etc/systemd/logind.conf.d/80-mediapad-t5-power.conf 0644
deploy t5-power-key.service /etc/systemd/system/t5-power-key.service 0644
deploy t5-power-display-resume.service /etc/systemd/system/t5-power-display-resume.service 0644
glib-compile-schemas --strict "$schema"
systemctl --root=/ enable t5-power-key.service t5-power-display-resume.service
bash enable-suspend.sh
runtime=$(mktemp -d /tmp/t5-extension-dconf.XXXXXX)
trap 'rm -f -- "$runtime/enable-user.py" "$runtime/dconf/user"; rmdir -- "$runtime/dconf" "$runtime" 2>/dev/null || true' EXIT
chown "$user:" "$runtime"
chmod 700 "$runtime"
# The upload staging path may only be traversable by root.
install -m 0644 enable-user.py "$runtime/enable-user.py"
runuser -u "$user" -- env -u DBUS_SESSION_BUS_ADDRESS HOME="$user_home" XDG_RUNTIME_DIR="$runtime" dbus-run-session -- python3 "$runtime/enable-user.py"
rm -f "$runtime/enable-user.py"
rmdir "$runtime"/dconf 2>/dev/null || :
echo "T5 quick setting and power key handler installed"
