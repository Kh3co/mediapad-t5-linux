#!/usr/bin/python3
"""Own the T5's single-purpose power key in the active local seat."""
import argparse
import fcntl
import os
import pwd
import struct
import subprocess
import time
import select
from contextlib import contextmanager
from pathlib import Path

PANEL = Path('/sys/class/backlight/t5-lp8556')
KEY_NODE = Path('/sys/bus/platform/devices/power-key')
RUNTIME = Path('/run/t5-power')
EVENT = struct.Struct('@llHHi')
EVIOCGRAB = 0x40044590
EVIOCSCLOCKID = 0x400445a0
MODE_SCHEMA = 'org.gnome.shell.extensions.t5-sleep-mode'
DISPLAY = 'org.gnome.Mutter.DisplayConfig'
DISPLAY_OBJECT = '/org/gnome/Mutter/DisplayConfig'
DSS = Path('/sys/bus/platform/devices/e8600000.display/scanout_status')
FIRMWARE_FB = 0x5d000000


def report(msg):
    print(msg, flush=True)


class CommandFailure(subprocess.CalledProcessError):
    def __str__(self):
        return f'{super().__str__()} {self.stderr.strip()}'


def run(argv, timeout=4):
    result = subprocess.run(argv, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise CommandFailure(result.returncode, argv, result.stdout, result.stderr)
    return result.stdout.strip()


def clock():
    return time.clock_gettime_ns(time.CLOCK_BOOTTIME)


def save_marker(name, value):
    RUNTIME.mkdir(mode=0o755, exist_ok=True)
    tmp = RUNTIME / (name + '.new')
    tmp.write_text(str(value))
    tmp.replace(RUNTIME / name)


@contextmanager
def transition_lock():
    RUNTIME.mkdir(mode=0o755, exist_ok=True)
    with (RUNTIME / 'transition.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def transition_pending():
    return (RUNTIME / 'requested').exists() or (RUNTIME / 'preparing').exists()


def active_session():
    session = run(['loginctl', 'show-seat', 'seat0', '-p', 'ActiveSession', '--value'])
    if not session:
        return None
    fields = dict(line.split('=', 1) for line in run([
        'loginctl', 'show-session', session, '-p', 'Type', '-p', 'Class', '-p', 'User',
    ]).splitlines())
    if fields.get('Type') not in ('wayland', 'x11') or fields.get('Class') not in ('user', 'user-early', 'greeter'):
        return None
    uid = int(fields['User'])
    if uid == 0 or not Path(f'/run/user/{uid}/bus').is_socket():
        return None
    return uid, fields['Class']


def as_session(session, *cmd):
    uid, _kind = session
    account = pwd.getpwuid(uid)
    return run([
        '/usr/sbin/runuser', '-u', account.pw_name, '--', '/usr/bin/env', '-i',
        f'HOME={account.pw_dir}', f'USER={account.pw_name}', 'PATH=/usr/bin:/bin',
        f'XDG_RUNTIME_DIR=/run/user/{uid}', f'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus', *cmd,
    ])


def bus(session, destination, path, method, *args):
    return as_session(session, 'gdbus', 'call', '--session', '--timeout', '2', '--dest', destination,
                      '--object-path', path, '--method', method, *args)


def try_session():
    try:
        return active_session()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        report(f'no active desktop session: {exc}')
        return None


def mode(session):
    if session and session[1] != 'greeter':
        try:
            value = as_session(session, 'gsettings', 'get', MODE_SCHEMA, 'mode')
        except (OSError, subprocess.SubprocessError) as exc:
            report(f'mode unavailable, using display-only action: {exc}')
            return 'screen-off'
        if value == "'suspend'":
            return 'suspend'
    return 'screen-off'


def display_asleep(session):
    if (PANEL / 'bl_power').read_text().strip() != '0':
        return True
    # A display pipeline can blank the backlight without changing bl_power.
    if (int((PANEL / 'actual_brightness').read_text()) == 0
            and int((PANEL / 'brightness').read_text()) > 0):
        return True
    if session:
        try:
            value = bus(session, DISPLAY, DISPLAY_OBJECT, 'org.freedesktop.DBus.Properties.Get',
                        DISPLAY, 'PowerSaveMode')
            # A negative value means the display backend has no DPMS support.
            return any(value in (f'(<{n}>,)', f'(<int32 {n}>,)') for n in (1, 2, 3))
        except (OSError, subprocess.SubprocessError):
            pass
    return False


def panel_power(on):
    (PANEL / 'bl_power').write_text('0' if on else '4')
    if (PANEL / 'bl_power').read_text().strip() != ('0' if on else '4'):
        raise RuntimeError('panel power readback mismatch')


def wake_desktop(session):
    if not session:
        return
    for destination, path, method, args in [
        (DISPLAY, DISPLAY_OBJECT, 'org.freedesktop.DBus.Properties.Set', (DISPLAY, 'PowerSaveMode', '<int32 0>')),
        ('org.gnome.Shell', '/org/gnome/Shell/Extensions/MediaPadT5Power',
         'org.gnome.Shell.Extensions.MediaPadT5Power.WakeDisplay', ()),
    ]:
        try:
            bus(session, destination, path, method, *args)
        except (OSError, subprocess.SubprocessError) as exc:
            report(f'{method}: {exc}')


def wake(session):
    # Keep display-only wake immediate.
    with transition_lock():
        if transition_pending():
            return
        panel_power(True)
    wake_desktop(session)


def live_native_scanout():
    try:
        s = dict(line.split('=', 1) for line in DSS.read_text().splitlines())
        dma = int(s['programmed_dma'], 16)
        return (s['native_dss'] == '1' and s['active'] == '1' and dma not in (0, FIRMWARE_FB)
                and dma == int(s['last_dma'], 16) and int(s['last_stride']) == 4800)
    except (OSError, ValueError, KeyError):
        return False


def finish_transition(reason):
    with transition_lock():
        if not transition_pending():
            return
        try:
            thaw_by = time.monotonic() + 3
            events = Path('/sys/fs/cgroup/user.slice/cgroup.events')
            while events.is_file() and 'frozen 1' in events.read_text().splitlines() and time.monotonic() < thaw_by:
                time.sleep(.05)
            wake_desktop(try_session())
            deadline = time.monotonic() + 3
            consecutive = 0
            while time.monotonic() < deadline:
                # sample twice so scanout has time to latch at least one refresh
                consecutive = consecutive + 1 if live_native_scanout() else 0
                if consecutive == 2:
                    break
                time.sleep(.05)
            report(f'restore={reason} native_scanout={consecutive == 2}')
        finally:
            # Keep a usable panel even if Mutter died, no GUI session exists or
            # native scanout took longer to return. The v106 fallback FB is black.
            panel_power(True)
            save_marker('resumed', clock())
            for marker in ('requested', 'preparing'):
                (RUNTIME / marker).unlink(missing_ok=True)


def sleep_hook(phase):
    with transition_lock():
        if phase == 'pre':
            save_marker('preparing', time.monotonic_ns())
            panel_power(False)
        else:
            # user.slice is still frozen in system-sleep post. Keep the panel
            # dark until the separately ordered resume service runs.
            save_marker('resumed', clock())
    report(f'system sleep {phase}')


def consumed_by_sleep(event_ns):
    if transition_pending():
        return True
    try:
        return event_ns <= int((RUNTIME / 'resumed').read_text())
    except FileNotFoundError:
        return False


def suspend_in_flight():
    try:
        active = run(['systemctl', 'show', '-p', 'ActiveState', '--value', 'systemd-suspend.service'])
        jobs = run(['systemctl', 'list-jobs', '--no-legend', '--no-pager'])
        preparing = run(['busctl', 'get-property', 'org.freedesktop.login1', '/org/freedesktop/login1',
                         'org.freedesktop.login1.Manager', 'PreparingForSleep'])
        queued = any(len(line.split()) > 1 and line.split()[1] in
                     ('systemd-suspend.service', 'suspend.target', 'sleep.target') for line in jobs.splitlines())
        return active in ('active', 'activating', 'deactivating') or queued or preparing == 'b true'
    except (OSError, subprocess.SubprocessError) as exc:
        report(f'sleep status unavailable: {exc}')
        return None


def watch_transition():
    # Normally ExecStopPost schedules the restore. Detect aborted/no-op jobs
    # that never ran systemd-sleep, including across a listener restart.
    starts = []
    for name in ('requested', 'preparing'):
        try:
            starts.append(int((RUNTIME / name).read_text()))
        except FileNotFoundError:
            pass
    if not starts:
        return
    age = (time.monotonic_ns() - max(starts)) / 1e9
    if age < 5:
        return
    pending = suspend_in_flight()
    if pending is False or (pending is None and age > 20) or age > 60:
        # CLOCK_MONOTONIC excludes time spent asleep; an indefinitely stalled
        # job must not leave the device inaccessible. A later pre-hook reblanks.
        finish_transition('sleep job ended or timed out')


def request_suspend():
    with transition_lock():
        if transition_pending():
            return
        save_marker('requested', time.monotonic_ns())
        try:
            panel_power(False)
        except (OSError, RuntimeError):
            (RUNTIME / 'requested').unlink(missing_ok=True)
            raise
    try:
        run(['systemctl', '--no-block', 'suspend'])
    except (OSError, subprocess.SubprocessError):
        if suspend_in_flight() is False:
            finish_transition('sleep request rejected')
        raise
    report('action=suspend panel blanked before logind')


def short_press():
    if transition_pending():
        return
    session = try_session()
    if display_asleep(session):
        wake(session)
        report('action=wake')
        return
    if mode(session) == 'suspend':
        if (KEY_NODE / 'armed').read_text().strip() != '1' or (KEY_NODE / 'power/wakeup').read_text().strip() != 'enabled':
            raise RuntimeError('power key wake unavailable')
        request_suspend()
    else:
        panel_power(False)
        report('action=screen-off')


def power_device():
    matches = []
    for event in Path('/sys/class/input').glob('event*'):
        node = (event / 'device').resolve()
        if Path('/sys/devices/platform/power-key') not in node.parents:
            continue
        if (node / 'name').read_text().strip() != 'MediaPad T5 power key' or (node / 'phys').read_text().strip() != 'ags2l03-pmic/input0':
            continue
        # This grab must never steal a keyboard, touch or volume controls.
        words = (node / 'capabilities/key').read_text().split()
        mask = sum(int(word, 16) << (64 * n) for n, word in enumerate(reversed(words)))
        if mask == (1 << 116):
            matches.append(Path('/dev/input') / event.name)
    if len(matches) != 1:
        raise RuntimeError(f'expected one dedicated T5 power button, found {matches}')
    return matches[0]


def worker():
    # Leave the button available until the display hardware is present.
    for name in ('bl_power', 'brightness', 'actual_brightness'):
        if not (PANEL / name).is_file():
            raise RuntimeError(f'waiting for {PANEL / name}')
    armed = KEY_NODE / 'armed'
    if armed.read_text().strip() == '0':
        armed.write_text('1')
    if armed.read_text().strip() != '1':
        raise RuntimeError('power key did not arm')
    path = power_device()
    with path.open('rb', buffering=0) as key:
        fcntl.ioctl(key, EVIOCSCLOCKID, struct.pack('i', time.CLOCK_BOOTTIME))
        fcntl.ioctl(key, EVIOCGRAB, struct.pack('i', 1))
        report(f'listening on {path} exclusively')
        down = None
        dropped = False
        while True:
            watch_transition()
            # No recurring polls while idle. When a request is pending, make
            # progress even if no physical button event follows a failed job.
            if not select.select([key], [], [], 1 if transition_pending() else None)[0]:
                continue
            data = key.read(EVENT.size * 16)
            if not data:
                raise OSError('power key disconnected')
            for sec, usec, kind, code, value in EVENT.iter_unpack(data):
                if kind == 0:
                    if code == 3:  # SYN_DROPPED
                        dropped = True
                        down = None
                    elif code == 0:  # SYN_REPORT
                        dropped = False
                    continue
                if dropped or kind != 1 or code != 116 or value == 2:
                    continue
                timestamp = sec * 1_000_000_000 + usec * 1000
                if consumed_by_sleep(timestamp):
                    down = None
                elif value == 1:
                    down = timestamp
                elif value == 0:
                    pressed = down
                    down = None
                    if pressed is None or timestamp - pressed > 1_000_000_000 or consumed_by_sleep(pressed):
                        continue
                    try:
                        short_press()
                    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                        report(f'power action failed: {exc}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['listen', 'pre', 'post', 'resume-desktop'])
    command = parser.parse_args().command
    if command == 'listen':
        worker()
    elif command == 'resume-desktop':
        finish_transition('systemd sleep finished')
    else:
        sleep_hook(command)
