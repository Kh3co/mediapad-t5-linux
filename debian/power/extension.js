import Gio from 'gi://Gio';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const choices = [
    ['screen-off', '关闭屏幕'],
    ['suspend', '进入睡眠'],
];
const wakeInterface = `<node>
  <interface name="org.gnome.Shell.Extensions.MediaPadT5Power">
    <method name="WakeDisplay"/>
  </interface>
</node>`;

const SleepMode = GObject.registerClass({GTypeName: 'MediaPadT5SleepModeToggle'}, class SleepMode extends QuickMenuToggle {
    _init(settings) {
        super._init({title: '睡眠模式', iconName: 'preferences-system-power-symbolic', toggleMode: false});
        this._settings = settings;
        this.menu.setHeader('preferences-system-power-symbolic', '睡眠模式');
        this._items = new Map();
        for (const [value, label] of choices) {
            const item = this.menu.addAction(label, () => {
                this._settings.set_string('mode', value);
            });
            this._items.set(value, item);
        }
        this.connect('clicked', () => this.menu.open());
        this._settings.connectObject('changed::mode', () => this._sync(), this);
        Main.sessionMode.connectObject('updated', () => this._sync(), this);
        this._sync();
    }

    _sync() {
        const selected = this._settings.get_string('mode');
        for (const [value, label] of choices) {
            this._items.get(value).setOrnament(
                value === selected ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
            if (value === selected)
                this.subtitle = label;
        }
        this.visible = Main.sessionMode.allowSettings;
    }
});

export default class T5Power extends Extension {
    enable() {
        this._indicator = new SystemIndicator();
        this._tile = new SleepMode(this.getSettings());
        this._indicator.quickSettingsItems.push(this._tile);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
        // Show the existing lock curtain on a physical wake. Keep the same
        // authentication dialog; never call SetActive(false) or deactivate().
        this._wake = Gio.DBusExportedObject.wrapJSObject(wakeInterface, {
            WakeDisplay() {
                const shield = Main.screenShield;
                if (shield?.locked && shield.active && shield._becameActiveId)
                    shield._wakeUpScreen();
            },
        });
        this._wake.export(Gio.DBus.session, '/org/gnome/Shell/Extensions/MediaPadT5Power');
    }

    disable() {
        this._wake?.unexport();
        this._tile?.destroy();
        this._indicator?.destroy();
        this._wake = this._tile = this._indicator = null;
    }
}
