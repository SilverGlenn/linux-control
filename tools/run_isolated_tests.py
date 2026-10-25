#!/usr/bin/python3
"""Run fixture commands with private HOME/XDG and buses, without service activation."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path, help='New private directory to retain fixture files')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    parser.error('Supply a fixture command after --') if not command else None
    temporary = None
    if args.work_dir:
        root = args.work_dir.absolute()
        if root.exists() or root.is_symlink():
            parser.error('--work-dir must not already exist')
        root.mkdir(mode=0o700, parents=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='control-private-fixtures-')
        root = Path(temporary.name)
    try:
        env = os.environ.copy()
        for variable, name in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'),
                               ('XDG_CONFIG_DIRS', 'defaults'), ('XDG_CACHE_HOME', 'cache'),
                               ('XDG_STATE_HOME', 'state'), ('XDG_DATA_HOME', 'data'),
                               ('XDG_RUNTIME_DIR', 'runtime')]:
            directory = root / name
            directory.mkdir(mode=0o700)
            env[variable] = str(directory)
        for variable in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS',
                         'DBUS_SYSTEM_BUS_ADDRESS', 'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE'):
            env.pop(variable, None)
        env.update(KDE_SKIP_KDERC='1', QT_QPA_PLATFORM='offscreen', PYTHONDONTWRITEBYTECODE='1')
        # No includes or service directories: unknown activation cannot launch
        # real desktop providers. Individual tests create their own fake buses.
        config = root / 'private-bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                          '<auth>EXTERNAL</auth><policy context="default"><allow own="*"/>'
                          '<allow send_destination="*"/><allow receive_sender="*"/>'
                          '</policy></busconfig>')
        child = ('import os,sys; os.environ["DBUS_SYSTEM_BUS_ADDRESS"]='
                 'os.environ["DBUS_SESSION_BUS_ADDRESS"]; os.execvpe(sys.argv[1],sys.argv[1:],os.environ)')
        return subprocess.run(['/usr/bin/dbus-run-session', '--config-file=' + str(config), '--',
                               sys.executable, '-c', child, *command], env=env).returncode
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    sys.exit(main())
