#!/bin/bash
set -eu
backup=/var/backups/t5-power-mode-v105
mkdir -p "$backup"
for unit in sleep.target suspend.target; do
    path=/etc/systemd/system/$unit
    if [ -L "$path" ] && [ "$(readlink "$path")" = /dev/null ]; then
        if [ ! -L "$backup/masked-$unit" ]; then cp -a "$path" "$backup/masked-$unit"; fi
        systemctl --root=/ unmask "$unit"
    fi
done
