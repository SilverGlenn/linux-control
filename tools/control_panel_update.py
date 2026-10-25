#!/usr/bin/python3
"""Version 1 manual, attested candidate updater. No timers or background polling.

Only explicit check/prepare/apply commands contact GitHub. The CLI deliberately
has no alternate home, destination, verifier, provider or staging switches.
Fixture code can inject those boundaries into Consumer without a live bypass.
"""
# Keep the public /usr/bin/python3 SCRIPT COMMAND contract, then isolate imports
# from PYTHONPATH, user site packages and interpreter environment variables.
import sys
if __name__ == '__main__' and not sys.flags.isolated:
    import os
    try:
        os.execv('/usr/bin/python3', ['/usr/bin/python3', '-I', os.path.abspath(__file__), *sys.argv[1:]])
    except OSError:
        sys.stdout.write('{"schema":1,"status":"error","message":"Cannot start isolated updater"}\n')
        sys.exit(1)

import argparse
import contextlib
import fcntl
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import pwd
import re
import selectors
import signal
import shutil
import stat
import subprocess
import sys
import tarfile
import time
from types import ModuleType, SimpleNamespace
import uuid

VERSION = 1
REPOSITORY = 'SilverGlenn/linux-control'
CHANNEL = 'laptop-candidate'
MAX_JSON = 65536
MAX_API = 4 * 1024 * 1024
MAX_ARCHIVE = 256 * 1024 * 1024
MAX_EXPANDED = 1024 * 1024 * 1024
MAX_MEMBERS = 20000
TAG = re.compile(r'laptop-candidate-([1-9][0-9]{0,15})-([0-9a-f]{12})\Z')
HEX = re.compile(r'[0-9a-f]{64}\Z')
COMMIT = re.compile(r'[0-9a-f]{40}\Z')


class Refusal(Exception):
    def __init__(self, message, status='error'):
        super().__init__(message)
        self.status = status


def require(condition, message, status='error'):
    if not condition:
        raise Refusal(message, status)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    value = hashlib.sha256()
    with open(path, 'rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def strict_json(data, limit=MAX_JSON):
    require(len(data) <= limit, 'JSON exceeds size limit')
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(Refusal('Nonfinite JSON value')))


def normalized(path):
    path = Path(path)
    require(path.is_absolute() and str(path) == os.path.normpath(str(path)), 'Noncanonical absolute path')
    return path


def no_links(path):
    for item in (path, *path.parents):
        require(not item.is_symlink(), 'Symlink path refused')


def private(path, directory=False):
    no_links(path)
    metadata = path.stat()
    require(metadata.st_uid == os.getuid(), 'Private path belongs to another user')
    require(stat.S_ISDIR(metadata.st_mode) if directory else stat.S_ISREG(metadata.st_mode), 'Invalid private path type')
    require(directory or metadata.st_nlink == 1, 'Hardlinked private file refused')
    require(stat.S_IMODE(metadata.st_mode) == (0o700 if directory else 0o600), 'Private path permissions must be 0700/0600')


def read_private(path):
    private(path)
    require(path.stat().st_size <= MAX_JSON, 'Private JSON exceeds size limit')
    return strict_json(path.read_bytes())


def atomic(path, value):
    data = json.dumps(value, sort_keys=True, separators=(',', ':')).encode() + b'\n'
    require(len(data) <= MAX_JSON, 'State exceeds size limit')
    temp = path.with_name('.' + path.name + '.' + uuid.uuid4().hex)
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temp.unlink(missing_ok=True)


def bounded_run(argv, limit=MAX_JSON, timeout=60, output=None, env=None):
    """Bound both pipes while running; never include credential-bearing stderr."""
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=env, start_new_session=True)
    buffers = {process.stdout: bytearray(), process.stderr: bytearray()}
    counts = {process.stdout: 0, process.stderr: 0}
    selector = selectors.DefaultSelector()
    for stream in buffers:
        selector.register(stream, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    try:
        while selector.get_map():
            require(time.monotonic() < deadline, 'Verifier/API command timed out')
            for key, _ in selector.select(min(0.2, max(0, deadline - time.monotonic()))):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                counts[key.fileobj] += len(chunk)
                cap = limit if key.fileobj is process.stdout else MAX_JSON
                require(counts[key.fileobj] <= cap, 'Verifier/API command exceeded output limit')
                if output is not None and key.fileobj is process.stdout:
                    output.write(chunk)
                else:
                    buffers[key.fileobj].extend(chunk)
        require(process.wait(timeout=max(0.01, deadline - time.monotonic())) == 0,
                'GitHub verifier/API unavailable, unauthorized, unsupported or verification failed')
        return bytes(buffers[process.stdout])
    finally:
        selector.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        process.stdout.close()
        process.stderr.close()


class GitHub:
    def __init__(self, executable):
        self.executable = normalized(executable)
        no_links(self.executable)
        for item in (self.executable, *self.executable.parents):
            metadata = item.stat()
            require(metadata.st_uid in (0, os.getuid()) and not metadata.st_mode & 0o022,
                    'GitHub verifier must be owned by root or this user and protected from other writers')
        require(self.executable.is_file() and os.access(self.executable, os.X_OK), 'GitHub verifier is unavailable')

    def command(self, *args, **kwargs):
        # Do not allow ambient endpoint/CLI configuration to change the repository.
        env = dict(os.environ)
        for name in ('GH_HOST', 'GH_REPO', 'GH_DEBUG', 'GH_ENTERPRISE_TOKEN', 'GITHUB_API_URL'):
            env.pop(name, None)
        env.update(GH_HOST='github.com', GH_PROMPT_DISABLED='1', GH_PAGER='cat', PAGER='cat', NO_COLOR='1')
        return bounded_run([str(self.executable), *args], env=env, **kwargs)

    def api(self, endpoint):
        return strict_json(self.command('api', '--hostname', 'github.com', endpoint, limit=MAX_API), limit=MAX_API)

    def verify(self, tag):
        self.command('release', 'verify', tag, '-R', REPOSITORY)

    def verify_asset(self, tag, path):
        self.command('release', 'verify-asset', tag, str(path), '-R', REPOSITORY)

    def download(self, asset, path, maximum):
        with open(path, 'xb') as stream:
            os.chmod(path, 0o600)
            self.command('api', '--hostname', 'github.com',
                         f'repos/{REPOSITORY}/releases/assets/{asset["id"]}',
                         '-H', 'Accept: application/octet-stream', limit=maximum,
                         timeout=180, output=stream)
            stream.flush()
            os.fsync(stream.fileno())


def actual_machine():
    values = {}
    for line in Path('/etc/os-release').read_text().splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            values[key] = value.strip('"\'')
    return {'architecture': platform.machine(), 'os_id': values.get('ID'),
            'os_version_id': values.get('VERSION_ID'),
            'session_type': os.environ.get('XDG_SESSION_TYPE', '')}


def safe_extract(archive, destination):
    """Read bounded USTAR headers directly, before tarfile can expand metadata.

    tarfile's normal iterator consumes PAX/GNU long-name payloads before yielding
    a member, so checking member types afterward would not bound those payloads.
    """
    destination.mkdir(mode=0o700)
    seen, expanded = set(), 0
    with gzip.open(archive, 'rb') as source:
        while True:
            header = source.read(512)
            require(len(header) == 512, 'Truncated archive header')
            if header == bytes(512):
                trailer = source.read(10241)
                require(512 <= len(trailer) <= 10240 and not any(trailer), 'Invalid/excessive archive trailer')
                break
            require(header[257:263] == b'ustar\x00', 'Only bounded USTAR archive headers are supported')
            member = tarfile.TarInfo.frombuf(header, 'utf-8', 'strict')
            require(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE),
                    'Archive links/special/extended metadata members refused')
            name = member.name
            if member.isdir() and name.endswith('/'):
                name = name[:-1]
            parts = PurePosixPath(name).parts
            require(name and '\x00' not in name and not name.startswith('/') and '\\' not in name and
                    all(part not in ('', '.', '..') for part in name.split('/')) and
                    str(PurePosixPath(name)) == name, 'Unsafe archive path')
            require(name not in seen and len(seen) < MAX_MEMBERS, 'Duplicate or excessive archive members')
            seen.add(name)
            require(not member.mode & ~0o777, 'Archive privileged modes refused')
            top = parts[0]
            require(top in ('payload', 'source', 'payload-manifest.json', 'payload-manifest.sha256'),
                    'Unexpected archive layout')
            if top in ('payload-manifest.json', 'payload-manifest.sha256'):
                require(len(parts) == 1 and member.isfile(), 'Invalid manifest member')
            if top == 'source' and len(parts) > 1:
                require(len(parts) == 2 and parts[1] in ('source-manifest.json', 'source.tar.gz', 'identity.json', 'build-identity.cmake'),
                        'Unexpected source member')
            expanded += member.size
            require(0 <= member.size <= MAX_EXPANDED and expanded <= MAX_EXPANDED, 'Expanded archive exceeds limit')
            target = destination.joinpath(*parts)
            if member.isdir():
                require(member.size == 0, 'Archive directory has content')
                target.mkdir(parents=True, exist_ok=True, mode=0o700)
                continue
            target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            with open(target, 'xb') as outgoing:
                remaining = member.size
                while remaining:
                    chunk = source.read(min(1024 * 1024, remaining))
                    require(chunk, 'Truncated archive member')
                    outgoing.write(chunk)
                    remaining -= len(chunk)
            padding = (-member.size) % 512
            tail = source.read(padding)
            require(len(tail) == padding and not any(tail), 'Invalid archive member padding')
            os.chmod(target, member.mode)


def headless_probe(bundle, descriptor, prefix=None):
    binary = (Path(prefix) if prefix else bundle / 'payload') / 'bin/control'
    executables = [binary]
    if not prefix:
        executables += sorted((bundle / 'payload/libexec').glob('controlpanel-*'))
    for executable in executables:
        with open(executable, 'rb') as stream:
            header = stream.read(64)
        require(len(header) == 64 and header[:6] == b'\x7fELF\x02\x01' and
                int.from_bytes(header[18:20], 'little') == 62 and
                int.from_bytes(header[16:18], 'little') in (2, 3),
                'Candidate executable/helper is not an x86_64 ELF', 'incompatible')
    # --version is the sole argument; the application handles it before Qt/providers.
    env = {'PATH': '/usr/bin:/bin', 'HOME': '/nonexistent', 'QT_QPA_PLATFORM': 'offscreen',
           'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent',
           'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent'}
    if not prefix:
        # The staged binary's $ORIGIN/../lib names its private cache, which
        # deliberately contains no externally owned AeroQt. Permit only the
        # target's existing lib directory, after rechecking the exact recorded
        # dependency bytes immediately before execution. Never inherit a build
        # or development loader path. The installed probe uses its real RPATH.
        library_directory = normalized(descriptor['target']['prefix']) / 'lib'
        uses_external_library_directory = False
        for item in descriptor['target']['external_dependencies']:
            dependency = normalized(item['path'])
            require(dependency.is_file() and file_digest(dependency) == item['sha256'],
                    'External dependency changed before staged identity probe', 'incompatible')
            if dependency.parent == library_directory:
                no_links(library_directory)
                uses_external_library_directory = True
        if uses_external_library_directory:
            env['LD_LIBRARY_PATH'] = str(library_directory)
    output = bounded_run([str(binary), '--version'], timeout=10, env=env).decode('utf-8')
    lines = output.splitlines()
    require('Build: ' + descriptor['build_id'] in lines and
            'Source revision: ' + descriptor['commit'] in lines and
            'Source manifest SHA-256: ' + descriptor['source_manifest_sha256'] in lines,
            'Candidate headless identity differs from release', 'incompatible')


class Consumer:
    def __init__(self, home=None, github=None, machine=None, installer=None, probe=None, locker=None):
        self.home = normalized(home if home is not None else pwd.getpwuid(os.getuid()).pw_dir)
        self.config_path = self.home / '.config/linux-control/updater.json'
        self.root = self.home / '.control-panel-updater'
        self.github_override, self.machine = github, machine or actual_machine
        self.installer, self.probe, self.locker = installer, probe or headless_probe, locker
        self.config, self.github = None, None

    def production_runtime(self):
        """Load only reviewed siblings from the independent private bootstrap.

        Explicit source compilation also prevents an adjacent bytecode cache or
        a missing-module PYTHONPATH fallback from selecting another installer.
        Fixture API consumers use already imported modules and injected boundaries.
        """
        runtime = Path(__file__).absolute().parent
        private(runtime, True)
        private(runtime / 'control_panel_update.py')
        require(not runtime.is_relative_to(self.home / '.local'), 'Updater runtime must be outside managed prefix')
        for name in ('install_lock', 'rc_bundle'):
            path = runtime / (name + '.py')
            private(path)
            require(path.stat().st_size <= 1024 * 1024, 'Trusted runtime module exceeds size limit')
            module = ModuleType(name)
            module.__file__ = str(path)
            module.__package__ = ''
            sys.modules[name] = module
            exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)

    def configure(self, network=False):
        if not self.config_path.exists():
            raise Refusal('Manual updates have not been bootstrapped', 'not_configured')
        config = read_private(self.config_path)
        required = {'schema', 'repository', 'repository_id', 'channel', 'prefix', 'deployment_state', 'gh', 'target'}
        require(set(config) == required and type(config['schema']) is int and config['schema'] == 1, 'Unsupported updater configuration')
        require(config['repository'] == REPOSITORY and config['channel'] == CHANNEL, 'Wrong updater repository/channel')
        require(type(config['repository_id']) is int and config['repository_id'] > 0, 'Numeric repository identity is not pinned', 'not_configured')
        require(config['prefix'] == str(self.home / '.local'), 'Updater prefix must be this user\'s ~/.local')
        require(config['deployment_state'] == str(self.home / '.control-panel-rc-deployment'), 'Noncanonical deployment journal')
        no_links(self.home / '.local')
        require(config['target'] == {'architecture': 'x86_64', 'os_id': 'ubuntu', 'os_version_id': '26.04', 'session_type': 'wayland'},
                'Unsupported configured target', 'incompatible')
        self.config = config
        if network:
            self.github = self.github_override or GitHub(config['gh'])
        return config

    @contextlib.contextmanager
    def state(self):
        no_links(self.root)
        self.root.mkdir(mode=0o700, exist_ok=True)
        private(self.root, True)
        path = self.root / 'lock'
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, 0o600)
        try:
            image, named = os.fstat(fd), path.lstat()
            require(stat.S_ISREG(image.st_mode) and image.st_uid == os.getuid() and
                    stat.S_IMODE(image.st_mode) == 0o600 and image.st_nlink == 1 and
                    (image.st_dev, image.st_ino) == (named.st_dev, named.st_ino), 'Unsafe updater lock')
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise Refusal('Another manual updater operation is active')
            yield
        finally:
            os.close(fd)

    def memory(self):
        path = self.root / 'state.json'
        state = read_private(path) if path.exists() else {'schema': 1, 'highwater': 0, 'identity': None, 'quarantine': [], 'result': None}
        require(state.get('schema') == 1 and type(state.get('highwater')) is int and
                isinstance(state.get('quarantine'), list), 'Invalid updater state')
        return state

    def save(self, state):
        atomic(self.root / 'state.json', state)

    def result(self, status, message, descriptor=None, receipt=None):
        result = {'schema': 1, 'status': status, 'message': message}
        if descriptor:
            result.update(build_id=descriptor['build_id'], release_tag=descriptor['tag'])
        if receipt:
            result['receipt'] = str(receipt)
        return result

    def status(self, current):
        self.configure()
        if os.path.lexists(Path(self.config['deployment_state']) / 'pending.json'):
            return self.result('manual_repair', 'An interrupted deployment needs the independent repair tool')
        if not self.root.exists():
            return self.result('idle', 'Manual updates are configured; no check has run')
        with self.state():
            state = self.memory()
            result = state.get('result')
            if result:
                require(isinstance(result, dict) and result.get('schema') == 1, 'Invalid saved updater result')
                if result.get('status') == 'installed' and result.get('build_id') != current:
                    journal_path = Path(self.config['deployment_state']) / 'state.json'
                    journal = read_private(journal_path) if journal_path.exists() else {}
                    recorded = journal.get('source', {}).get('build_id')
                    if recorded == current or recorded is None:
                        state['quarantine'] = list(dict.fromkeys(state['quarantine'] + [result['build_id']]))[-100:]
                        result = {**result, 'status': 'rolled_back',
                                  'message': 'The candidate is no longer active after a manual deployment change'}
                    else:
                        result = self.result('error', 'Running build differs from the canonical deployment journal')
                    state['result'] = result
                    self.save(state)
                elif result.get('status') == 'ready':
                    receipt_path = normalized(result.get('receipt', ''))
                    require(receipt_path.parent.parent == self.root / 'receipts' and receipt_path.name == 'receipt.json',
                            'Saved receipt is outside the prepared cache')
                    receipt = read_private(receipt_path)
                    if receipt.get('used') is not False:
                        result = {**result, 'status': 'manual_repair',
                                  'message': 'Prepared receipt was consumed by an interrupted helper; inspect the journal'}
                        result.pop('receipt', None)
                        state['quarantine'] = list(dict.fromkeys(state['quarantine'] + [result['build_id']]))[-100:]
                        state['result'] = result
                        self.save(state)
                elif result.get('status') == 'available' and result.get('build_id') == current:
                    result = {**result, 'status': 'up_to_date', 'message': 'Running build matches the last verified candidate'}
                return result
            return self.result('idle', 'No candidate prepared')

    def repository(self):
        data = self.github.api(f'repos/{REPOSITORY}')
        require(data.get('id') == self.config['repository_id'] and data.get('full_name') == REPOSITORY,
                'Pinned repository identity differs')

    def release(self, tag=None):
        self.repository()
        endpoint = f'repos/{REPOSITORY}/releases'
        if tag:
            require(TAG.fullmatch(tag), 'Invalid candidate tag')
            release = self.github.api(endpoint + '/tags/' + tag)
        else:
            releases = self.github.api(endpoint + '?per_page=100')
            require(isinstance(releases, list) and len(releases) <= 100, 'Invalid release listing')
            releases = [item for item in releases if isinstance(item, dict) and TAG.fullmatch(str(item.get('tag_name', '')))
                        and not item.get('draft') and item.get('published_at')]
            if not releases:
                raise Refusal('No published candidate releases', 'no_releases')
            release = max(releases, key=lambda item: int(TAG.fullmatch(item['tag_name'])[1]))
        require(isinstance(release, dict) and type(release.get('id')) is int and release['id'] > 0,
                'Invalid release identity')
        require(release.get('draft') is False and release.get('published_at') and release.get('immutable') is True,
                'Candidate release is not published and immutable', 'incompatible')
        require(TAG.fullmatch(str(release.get('tag_name', ''))) and (not tag or release['tag_name'] == tag), 'Release tag mismatch')
        assets = release.get('assets', [])
        require(isinstance(assets, list) and len(assets) <= 100, 'Excessive release assets')
        selected = {}
        for name, maximum in (('release.json', MAX_JSON), ('control-panel-bundle.tar.gz', MAX_ARCHIVE)):
            matches = [asset for asset in assets if asset.get('name') == name]
            require(len(matches) == 1, 'Missing or duplicate required release asset')
            asset = matches[0]
            require(type(asset.get('id')) is int and asset['id'] > 0 and type(asset.get('size')) is int and
                    0 < asset['size'] <= maximum and asset.get('state') == 'uploaded', 'Invalid release asset')
            selected[name] = asset
        self.github.verify(release['tag_name'])
        return release, selected

    def descriptor(self, path, release):
        require(path.stat().st_size <= MAX_JSON, 'Release descriptor exceeds limit')
        value = strict_json(path.read_bytes())
        fields = {'schema', 'repository', 'repository_id', 'channel', 'sequence', 'tag', 'commit', 'build_id',
                  'source_manifest_sha256', 'payload_manifest_sha256', 'archive_sha256', 'archive_size', 'target', 'minimum_updater_version'}
        require(isinstance(value, dict) and set(value) == fields and type(value['schema']) is int and value['schema'] == 1,
                'Unsupported release descriptor schema', 'incompatible')
        require(value['repository'] == REPOSITORY and type(value['repository_id']) is int and
                value['repository_id'] == self.config['repository_id'] and value['channel'] == CHANNEL, 'Descriptor repository/channel mismatch')
        match = TAG.fullmatch(str(value['tag']))
        require(match and value['tag'] == release['tag_name'] and type(value['sequence']) is int and
                value['sequence'] == int(match[1]) and COMMIT.fullmatch(str(value['commit'])) and value['commit'].startswith(match[2]),
                'Descriptor tag/commit/sequence mismatch')
        require(isinstance(value['build_id'], str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_-]{0,199}', value['build_id']), 'Invalid build identity')
        for field in ('source_manifest_sha256', 'payload_manifest_sha256', 'archive_sha256'):
            require(HEX.fullmatch(str(value[field])), 'Invalid descriptor digest')
        require(type(value['archive_size']) is int and 0 < value['archive_size'] <= MAX_ARCHIVE, 'Invalid archive size')
        require(type(value['minimum_updater_version']) is int and 0 < value['minimum_updater_version'] <= VERSION,
                'Candidate requires a reviewed newer updater', 'incompatible')
        target = value['target']
        require(isinstance(target, dict) and set(target) == set(self.config['target']) | {'prefix', 'external_dependencies'}, 'Invalid target contract')
        require({key: target[key] for key in self.config['target']} == self.config['target'] and
                target['prefix'] == self.config['prefix'], 'Candidate target differs from configuration', 'incompatible')
        require(isinstance(target['external_dependencies'], list) and 0 < len(target['external_dependencies']) <= 512,
                'Exact external dependency inventory required', 'incompatible')
        seen = set()
        for item in target['external_dependencies']:
            require(isinstance(item, dict) and set(item) == {'path', 'sha256'} and HEX.fullmatch(str(item['sha256'])), 'Invalid dependency fingerprint')
            dependency = normalized(item['path'])
            require(str(dependency) not in seen, 'Duplicate dependency path')
            seen.add(str(dependency))
        # Bind the immutable release to an exact commit and current reviewed main
        # ancestry; release notes and mutable "latest" flags are never consulted.
        require(release.get('target_commitish') == value['commit'], 'Release must name a full main commit')
        ref = self.github.api(f'repos/{REPOSITORY}/git/ref/tags/{value["tag"]}')
        obj = ref.get('object', {})
        for _ in range(4):
            if obj.get('type') != 'tag':
                break
            require(COMMIT.fullmatch(str(obj.get('sha', ''))), 'Invalid annotated tag identity')
            obj = self.github.api(f'repos/{REPOSITORY}/git/tags/{obj["sha"]}').get('object', {})
        require(obj.get('type') == 'commit' and obj.get('sha') == value['commit'], 'Tag does not bind descriptor commit')
        ancestry = self.github.api(f'repos/{REPOSITORY}/compare/{value["commit"]}...main')
        require(ancestry.get('status') in ('ahead', 'identical') and ancestry.get('merge_base_commit', {}).get('sha') == value['commit'],
                'Candidate commit is outside reviewed main ancestry')
        return value

    def compatible(self, value):
        machine = self.machine()
        require(all(machine.get(key) == expected for key, expected in self.config['target'].items()),
                'Machine/session does not match the candidate target', 'incompatible')
        for item in value['target']['external_dependencies']:
            path = Path(item['path'])
            require(path.is_file() and file_digest(path) == item['sha256'], 'External dependency fingerprint differs', 'incompatible')

    def sequence(self, value, state):
        identity = {key: value[key] for key in ('tag', 'commit', 'build_id', 'archive_sha256', 'payload_manifest_sha256')}
        require(value['build_id'] not in state['quarantine'], 'Candidate is quarantined after a failed attempt', 'incompatible')
        require(value['sequence'] >= state['highwater'], 'Candidate sequence is below the accepted high-water mark', 'incompatible')
        require(value['sequence'] != state['highwater'] or identity == state['identity'], 'Sequence was reused for different bytes')
        return identity

    def candidate(self, tag, directory):
        release, assets = self.release(tag)
        descriptor_path = directory / 'release.json'
        self.github.download(assets['release.json'], descriptor_path, MAX_JSON)
        require(descriptor_path.stat().st_size == assets['release.json']['size'], 'Descriptor download size differs')
        self.github.verify_asset(release['tag_name'], descriptor_path)
        value = self.descriptor(descriptor_path, release)
        require(value['archive_size'] == assets['control-panel-bundle.tar.gz']['size'], 'Archive metadata size differs')
        self.compatible(value)
        return release, assets, value

    def check(self, current, tag=None, prepare=False):
        self.configure(network=True)
        with self.state():
            state = self.memory()
            receipts = self.root / 'receipts'
            receipts.mkdir(mode=0o700, exist_ok=True)
            private(receipts, True)
            directory = receipts / uuid.uuid4().hex
            directory.mkdir(mode=0o700)
            keep = False
            try:
                release, assets, value = self.candidate(tag, directory)
                identity = self.sequence(value, state)
                if value['build_id'] == current:
                    result = self.result('up_to_date', 'Current build matches the verified candidate', value)
                elif not prepare:
                    result = self.result('available', 'A verified candidate is available for acceptance', value)
                else:
                    archive = directory / 'control-panel-bundle.tar.gz'
                    self.github.download(assets[archive.name], archive, MAX_ARCHIVE)
                    self.verify_archive(archive, value)
                    self.github.verify_asset(value['tag'], archive)
                    bundle = directory / 'bundle'
                    safe_extract(archive, bundle)
                    self.validate_bundle(bundle, value)
                    self.probe(bundle, value)
                    receipt = {'schema': 1, 'token': directory.name, 'descriptor_sha256': file_digest(directory / 'release.json'),
                               'archive_sha256': value['archive_sha256'], 'release_id': release['id'], 'used': False}
                    atomic(directory / 'receipt.json', receipt)
                    result = self.result('ready', 'Verified exact candidate is prepared; close Control Panel to install', value,
                                         directory / 'receipt.json')
                    keep = True
                state.update(highwater=value['sequence'], identity=identity, result=result)
                self.save(state)
                return result
            finally:
                if not keep:
                    shutil.rmtree(directory)

    def verify_archive(self, archive, value):
        require(archive.stat().st_size == value['archive_size'] and file_digest(archive) == value['archive_sha256'], 'Archive hash/size mismatch')

    def validate_bundle(self, bundle, value):
        import rc_bundle
        manifest_path = bundle / 'payload-manifest.json'
        require(manifest_path.stat().st_size <= 1024 * 1024, 'Payload manifest exceeds size limit')
        strict_json(manifest_path.read_bytes(), limit=1024 * 1024)
        require((bundle / 'source/identity.json').stat().st_size <= MAX_JSON, 'Source identity exceeds size limit')
        _, manifest = rc_bundle.load_bundle(bundle)
        require(file_digest(bundle / 'payload-manifest.json') == value['payload_manifest_sha256'], 'Payload descriptor digest differs')
        identity = strict_json((bundle / 'source/identity.json').read_bytes())
        require(identity == manifest['source'] and identity.get('build_id') == value['build_id'] and
                identity.get('revision') == value['commit'] and identity.get('source_manifest_sha256') == value['source_manifest_sha256'],
                'Frozen source identity differs from release')
        require(file_digest(bundle / 'source/source-manifest.json') == value['source_manifest_sha256'] and
                file_digest(bundle / 'source/source.tar.gz') == identity.get('source_archive_sha256'), 'Frozen source hashes differ')
        require(manifest['prefix'] == value['target']['prefix'], 'Bundle prefix differs')
        dependencies = manifest.get('external_dependencies', [])
        expected = [{'path': item['path'], 'sha256': item.get('image', {}).get('sha256')} for item in dependencies]
        require(expected == value['target']['external_dependencies'], 'Bundle dependency inventory differs')
        # No unmanaged extra regular files may hide in the extracted bundle.
        allowed = {'payload-manifest.json', 'payload-manifest.sha256', 'source/source-manifest.json',
                   'source/source.tar.gz', 'source/identity.json', 'source/build-identity.cmake'}
        require(all(not image['mode'] & 0o022 for image in manifest['entries'].values()),
                'Managed payload must not be writable by other users')
        allowed.update('payload/' + name for name in manifest['entries'])
        require({str(path.relative_to(bundle)) for path in bundle.rglob('*') if path.is_file()} == allowed,
                'Unexpected bundle files')
        return manifest

    def apply(self, receipt_path, wait_seconds):
        self.configure(network=True)
        require(type(wait_seconds) is int and 0 <= wait_seconds <= 300, 'Wait limit must be between 0 and 300 seconds')
        with self.state():
            path = normalized(receipt_path)
            require(path.parent.parent == self.root / 'receipts' and path.name == 'receipt.json' and
                    re.fullmatch('[0-9a-f]{32}', path.parent.name), 'Receipt is outside the private prepared cache')
            private(path.parent, True)
            receipt = read_private(path)
            require(set(receipt) == {'schema', 'token', 'descriptor_sha256', 'archive_sha256', 'release_id', 'used'} and
                    receipt['schema'] == 1 and receipt['token'] == path.parent.name and receipt['used'] is False,
                    'Receipt is invalid or already consumed')
            directory = path.parent
            descriptor_path = directory / 'release.json'
            private(descriptor_path)
            require(file_digest(descriptor_path) == receipt['descriptor_sha256'], 'Prepared descriptor was substituted')
            raw = strict_json(descriptor_path.read_bytes())
            release, assets = self.release(raw['tag'])
            require(release['id'] == receipt['release_id'], 'Prepared release identity changed')
            self.github.verify_asset(raw['tag'], descriptor_path)
            value = self.descriptor(descriptor_path, release)
            state = self.memory()
            self.sequence(value, state)
            archive = directory / 'control-panel-bundle.tar.gz'
            private(archive)
            self.verify_archive(archive, value)
            require(receipt['archive_sha256'] == value['archive_sha256'] and assets[archive.name]['size'] == value['archive_size'],
                    'Receipt/archive differs')
            self.github.verify_asset(value['tag'], archive)
            # Re-extract the verified archive; never trust a previously staged tree.
            bundle = directory / ('apply-' + uuid.uuid4().hex)
            safe_extract(archive, bundle)
            try:
                self.validate_bundle(bundle, value)
                self.compatible(value)
                self.probe(bundle, value)
                import install_lock
                lock = self.locker or install_lock.installation_lock
                with contextlib.ExitStack() as locks:
                    try:
                        locks.enter_context(lock(self.config['prefix'], exclusive=True, wait_seconds=wait_seconds))
                    except install_lock.Busy:
                        result = self.result('ready', 'Control Panel remains open; installation deferred; retry after it closes', value, path)
                        state['result'] = result
                        self.save(state)
                        return result
                    require(not os.path.lexists(Path(self.config['deployment_state']) / 'pending.json'),
                            'An earlier interrupted deployment requires independent repair', 'manual_repair')
                    self.compatible(value)
                    receipt['used'] = True
                    atomic(path, receipt)  # durable replay barrier before the first mutation
                    state['quarantine'] = list(dict.fromkeys(state['quarantine'] + [value['build_id']]))[-100:]
                    state['result'] = self.result('manual_repair', 'Installation was interrupted; inspect the deployment journal', value)
                    self.save(state)
                    before = self.deployment_snapshot()
                    try:
                        self.install(bundle)
                        self.probe(bundle, value, prefix=self.config['prefix'])
                        state['quarantine'].remove(value['build_id'])
                        result = self.result('installed', 'Candidate installed for acceptance; live qualification remains open', value)
                    except Exception:
                        state['quarantine'] = list(dict.fromkeys(state['quarantine'] + [value['build_id']]))[-100:]
                        try:
                            self.repair_after_failure(before, value)
                            result = self.result('rolled_back', 'Candidate failed; previous application files were preserved or restored', value)
                        except Exception:
                            result = self.result('manual_repair', 'Installation failed; use the independent repair tool and preserve the journal', value)
                    state['result'] = result
                    self.save(state)
                    return result
            finally:
                shutil.rmtree(bundle)

    def persist_failure(self, result):
        # Detached apply helpers must leave a readable outcome even when they
        # refuse before mutation. Never obscure an already pending repair.
        if self.config is None:
            return
        try:
            with self.state():
                state = self.memory()
                previous = state.get('result') or {}
                if previous.get('status') == 'manual_repair':
                    return
                state['result'] = result
                self.save(state)
        except Exception:
            pass  # unsafe/busy state must never be replaced to report an error

    def install(self, bundle):
        if self.installer:
            return self.installer('install', bundle, self.config)
        import rc_bundle
        with contextlib.redirect_stdout(io.StringIO()):
            rc_bundle.install(SimpleNamespace(bundle=str(bundle), state=self.config['deployment_state'], staging_root=None))

    def deployment_snapshot(self):
        path = Path(self.config['deployment_state']) / 'state.json'
        if path.exists():
            return read_private(path)
        return None

    def repair_after_failure(self, before, value):
        if self.installer:
            return self.installer('repair', None, self.config)
        import rc_bundle
        args = SimpleNamespace(state=self.config['deployment_state'], command='rollback')
        with contextlib.redirect_stdout(io.StringIO()):
            if (Path(args.state) / 'pending.json').exists():
                rc_bundle.recover(args)
            else:
                after = self.deployment_snapshot()
                if after == before:
                    return  # refusal before mutation; never undo the prior release
                require(after is not None and after.get('source', {}).get('build_id') == value['build_id'] and
                        len(after.get('history', [])) == len((before or {}).get('history', [])) + 1,
                        'Unrecognized deployment state after failure')
                rc_bundle.rollback_or_remove(args)


class JSONParser(argparse.ArgumentParser):
    def error(self, message):
        raise Refusal('Invalid command arguments: ' + message)

    def exit(self, status=0, message=None):
        raise Refusal('Use check, prepare, apply or status; see the manual update documentation')


def main(argv=None):
    consumer, args = None, None
    try:
        parser = JSONParser(add_help=False)
        commands = parser.add_subparsers(dest='command', required=True, parser_class=JSONParser)
        for command in ('check', 'status'):
            item = commands.add_parser(command, add_help=False)
            item.add_argument('--current-build', required=True)
        item = commands.add_parser('prepare', add_help=False)
        item.add_argument('--release-tag', required=True)
        item.add_argument('--current-build', required=True)
        item = commands.add_parser('apply', add_help=False)
        item.add_argument('--receipt', required=True)
        item.add_argument('--wait-seconds', type=int, default=30)
        args = parser.parse_args(argv)
        consumer = Consumer()
        consumer.production_runtime()
        if args.command == 'status':
            result = consumer.status(args.current_build)
        elif args.command == 'check':
            result = consumer.check(args.current_build)
        elif args.command == 'prepare':
            result = consumer.check(args.current_build, args.release_tag, prepare=True)
        else:
            result = consumer.apply(args.receipt, args.wait_seconds)
        code = 1 if result['status'] in ('error', 'manual_repair', 'rolled_back') else 0
    except Refusal as error:
        result = {'schema': 1, 'status': error.status, 'message': str(error)[:1000]}
        code = 0 if error.status in ('not_configured', 'no_releases', 'incompatible') else 1
    except Exception:
        # API errors can contain token-bearing URLs; never serialize their text.
        result = {'schema': 1, 'status': 'error', 'message': 'Manual update refused; configuration, verifier, network or cache validation failed'}
        code = 1
    if consumer is not None and args is not None and args.command == 'apply' and result['status'] not in ('installed', 'ready'):
        consumer.persist_failure(result)
    output = json.dumps(result, sort_keys=True) + '\n'
    if len(output.encode()) > MAX_JSON:
        output = json.dumps({'schema': 1, 'status': 'error', 'message': 'Updater result exceeds size limit'}) + '\n'
        code = 1
    sys.stdout.write(output)
    return code


if __name__ == '__main__':
    sys.exit(main())
