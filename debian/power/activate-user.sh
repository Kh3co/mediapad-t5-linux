#!/bin/sh
# Run as the logged-in desktop user in their existing session.
set -eu
uuid=t5-sleep-mode@mediapad
test "$(id -u)" -ne 0
export LC_ALL=C
test -S "${XDG_RUNTIME_DIR:?}/bus"
test "$(gsettings get org.gnome.shell disable-user-extensions)" = false
gnome-extensions enable "$uuid"
for n in 1 2 3 4 5; do
    info=$(gnome-extensions info "$uuid")
    if echo "$info" | grep -Eq 'State: ACTIVE|State: ENABLED'; then
        echo "$info"
        gsettings get org.gnome.shell.extensions.t5-sleep-mode mode
        exit 0
    fi
    sleep 1
done
echo "$info"
exit 1
