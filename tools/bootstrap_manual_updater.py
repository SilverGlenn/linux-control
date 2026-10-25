#!/usr/bin/python3
"""Copy reviewed updater tools to a private runtime. Does not install the app or contact GitHub."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import sys

from control_panel_update import Consumer
import install_lock
import rc_bundle as rc

REPOSITORY_ID = 1421857804  # Read-only GitHub repository metadata checked 25 Oct 2026.


def private_dir(path):
    rc.no_symlink_ancestors(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    image = path.stat()
    rc.require(image.st_uid == os.getuid() and stat.S_IMODE(image.st_mode) == 0o700,
               f'Updater directory must be privately owned, mode 0700: {path}')


def bootstrap(home, verifier, replace=False, reviewed_sha256=None):
    """Explicit home injection is for private fixture staging, never a consumer bypass."""
    home = Path(home).absolute()
    runtime = home / '.control-panel-updater/runtime'
    config_dir = home / '.config/linux-control'
    private_dir(runtime.parent)
    private_dir(runtime)
    private_dir(config_dir)
    source = Path(__file__).resolve().parent
    verifier = Path(verifier).resolve(strict=True)
    if reviewed_sha256 is not None:
        rc.require(re.fullmatch('[0-9a-f]{64}', reviewed_sha256) is not None,
                   'Reviewed verifier SHA-256 must be a full lowercase digest')
    for path in (verifier, *verifier.parents):
        metadata = path.stat()
        rc.require(metadata.st_uid in (0, os.getuid()), 'Verifier source belongs to another user')
        sticky_root = path != verifier and metadata.st_uid == 0 and metadata.st_mode & stat.S_ISVTX
        if path == verifier:
            rc.require(not metadata.st_mode & 0o022, 'Verifier source must not be group/world writable')
        elif metadata.st_mode & 0o022 and not sticky_root:
            rc.require(reviewed_sha256 is not None,
                       'Verifier source has writable ancestry; use a protected CLI or an independently reviewed --reviewed-verifier-sha256 pin')
    verifier_image = rc.fingerprint(verifier)
    rc.require(verifier_image['type'] == 'file' and verifier_image['mode'] & 0o111,
               'Supply an installed GitHub CLI executable, not a download or shell wrapper')
    verifier_bytes = rc.object_bytes(verifier, verifier_image)
    if reviewed_sha256 is not None:
        rc.require(rc.sha(verifier_bytes) == reviewed_sha256, 'Verifier bytes differ from the reviewed SHA-256 pin')
    rc.require(verifier_bytes[:4] == b'\x7fELF', 'The reviewed GitHub CLI must be an ELF executable')
    # Reviewed local bootstrap pins ordinary verifier bytes in a private directory;
    # runtime checks never follow ~/.local/bin/gh symlinks or writable ancestors.
    files = {name: (rc.regular_bytes(source / name), 0o600) for name in
             ('control_panel_update.py', 'rc_bundle.py', 'install_lock.py')}
    files['gh'] = (verifier_bytes, 0o500)
    config = {'schema': 1, 'repository': 'SilverGlenn/linux-control',
              'repository_id': REPOSITORY_ID, 'channel': 'laptop-candidate',
              'prefix': str(home / '.local'),
              'deployment_state': str(home / '.control-panel-rc-deployment'),
              'gh': str(runtime / 'gh'),
              'target': {'architecture': 'x86_64', 'os_id': 'ubuntu',
                         'os_version_id': '26.04', 'session_type': 'wayland'}}
    desired = {runtime / name: (data, mode) for name, (data, mode) in files.items()}
    desired[config_dir / 'updater.json'] = (rc.encoded(config), 0o600)
    # Plan all replacements before writing. Existing foreign edits are preserved
    # unless this separate reviewed bootstrap was explicitly asked to replace them.
    # Same lock order as apply: updater state, then the application destination.
    # Thus another updater cannot import half-replaced runtime modules.
    with Consumer(home=home).state(), install_lock.installation_lock(home / '.local'):
        before = {path: rc.fingerprint(path) for path in desired}
        for path, (data, mode) in desired.items():
            image = before[path]
            same = image == {'type': 'file', 'sha256': rc.sha(data), 'bytes': len(data), 'mode': mode}
            rc.require(image['type'] == 'absent' or same or
                       (replace and image['type'] == 'file' and path.stat().st_uid == os.getuid()),
                       f'Existing updater file differs; review it before --replace-reviewed: {path}')
        for path, (data, mode) in desired.items():
            rc.require(rc.fingerprint(path) == before[path], f'Updater file changed during bootstrap: {path}')
            if before[path] == {'type': 'file', 'sha256': rc.sha(data), 'bytes': len(data), 'mode': mode}:
                continue
            rc.write_atomic(path, data, mode)
    return {'runtime': str(runtime), 'configuration': str(config_dir / 'updater.json'),
            'verifier_sha256': hashlib.sha256(files['gh'][0]).hexdigest(),
            'schedule': 'manual only; no service or timer created'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verifier', type=Path, help='Reviewed installed GitHub CLI (resolved and copied)')
    parser.add_argument('--staging-root', type=Path, help='Private fixture root; never the actual user home')
    parser.add_argument('--replace-reviewed', action='store_true', help='Replace reviewed existing updater tools/config')
    parser.add_argument('--reviewed-verifier-sha256', help='Previously reviewed verifier digest, required for writable source ancestry')
    args = parser.parse_args()
    try:
        home = Path(pwd.getpwuid(os.getuid()).pw_dir)
        if args.staging_root:
            staging = rc.absolute(args.staging_root)
            rc.require(staging != home and not home.is_relative_to(staging), 'Fixture root must not contain the real home')
            private_dir(staging)
            home = staging / home.relative_to('/')
        verifier = args.verifier or shutil.which('gh')
        rc.require(verifier is not None, 'Install/configure a trusted GitHub CLI before bootstrapping updates')
        print(json.dumps(bootstrap(home, verifier, args.replace_reviewed, args.reviewed_verifier_sha256), indent=2))
        return 0
    except (rc.Refusal, install_lock.Refusal, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
