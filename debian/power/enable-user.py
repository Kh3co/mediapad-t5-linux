#!/usr/bin/python3
from gi.repository import Gio

uuid = 't5-sleep-mode@mediapad'
shell = Gio.Settings.new('org.gnome.shell')
if shell.get_boolean('disable-user-extensions'):
    raise RuntimeError('GNOME extensions are globally disabled')
# A disabled list entry for this UUID takes priority over enabled-extensions.
for key in ('disabled-extensions', 'enabled-extensions'):
    old = list(shell.get_strv(key))
    if key == 'disabled-extensions':
        new = [entry for entry in old if entry != uuid]
    else:
        new = old if uuid in old else old + [uuid]
    if new != old and not shell.set_strv(key, new):
        raise RuntimeError(f'{key} is locked')
Gio.Settings.sync()
