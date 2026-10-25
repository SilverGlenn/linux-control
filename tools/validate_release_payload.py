#!/usr/bin/env python3
"""Validate exact release bytes in disposable, provider-free installation fixtures.

Never installs into the compiled live prefix. External runtime files must already
be pinned in the bundle; explicit copies are fixture inputs, outside ownership.
The result is staging evidence, not desktop/hardware qualification.
"""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from unittest import mock

if str(Path(__file__).resolve().parent) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
import rc_bundle as rc


SYSTEM_ROOTS = tuple(Path(p).resolve() for p in ('/usr/lib', '/usr/lib64', '/lib', '/lib64'))


def within(path, root):
    return path == root or root in path.parents


def require(condition, message):
    rc.require(condition, message)


def private_environment(root):
    # Allowlist rather than trying to enumerate loader/plugin injection variables.
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
           'QT_QPA_PLATFORM': 'deliberately-invalid', 'KDE_SKIP_KDERC': '1',
           'DBUS_SESSION_BUS_ADDRESS': 'unix:path=' + str(root / 'no-session-bus'),
           'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=' + str(root / 'no-system-bus')}
    for variable, name in (('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'),
                           ('XDG_CONFIG_DIRS', 'defaults'), ('XDG_DATA_HOME', 'data'),
                           ('XDG_DATA_DIRS', 'data-defaults'), ('XDG_STATE_HOME', 'state'),
                           ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')):
        path = root / name
        path.mkdir(mode=0o700, parents=True)
        env[variable] = str(path)
    return env


def parse_loader(output):
    """Parse ldd's complete resolved inventory, including the ELF interpreter."""
    result = []
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        require('not found' not in line, 'Unresolved runtime dependency: ' + line)
        if re.fullmatch(r'linux-vdso\.so\.\d+ \(0x[0-9a-f]+\)', line):
            continue
        match = re.fullmatch(r'(?:(\S+) => )?(/.+?) \(0x[0-9a-f]+\)', line)
        require(match is not None, 'Unrecognized loader output: ' + line)
        result.append((match.group(1) or Path(match.group(2)).name, Path(match.group(2))))
    require(bool(result), 'Loader returned no resolved runtime dependencies')
    return result


class Validator:
    def __init__(self, args, output):
        self.args, self.output = args, output
        self.report = {'schema': 1, 'status': 'failed', 'staged_deployment_passed': False,
                       'runtime_check_passed': False, 'checks': {},
                       'qualification': 'staging only; live desktop and hardware gates remain open'}
        self.events = []

    def command(self, label, command, cwd, env):
        result = subprocess.run([str(p) for p in command], cwd=cwd, env=env,
                                capture_output=True, text=True, timeout=30)
        (self.output / (label + '.log')).write_text(
            '$ ' + ' '.join(str(p) for p in command) + '\n' + result.stdout + result.stderr)
        require(result.returncode == 0, f'{label} failed (exit {result.returncode}); see log')
        return result.stdout

    def operation(self, name, **arguments):
        function = {'install': rc.install, 'recover': rc.recover}.get(name, rc.rollback_or_remove)
        args = argparse.Namespace(command=name, **arguments)
        stream = io.StringIO()
        try:
            with contextlib.redirect_stdout(stream):
                function(args)
        finally:
            self.events.append({'operation': name, 'output': stream.getvalue()})

    def check(self, name):
        self.report['checks'][name] = 'passed'

    def expected(self, destination, entries, names):
        for name in names:
            require(rc.fingerprint(destination / name) == entries.get(name, {'type': 'absent'}),
                    'Staged fingerprint mismatch: ' + name)

    def synthetic_predecessor(self, bundle, manifest, directory):
        # Candidate executable bytes are copied unchanged. Only clearly labelled
        # fixture metadata differs; this is not evidence about an older release.
        for name, image in manifest['entries'].items():
            rc.write_atomic(directory / 'payload' / name,
                            rc.object_bytes(bundle / 'payload' / name, image), image['mode'])
        desktop = directory / 'payload/share/applications/linux-controlpanel.desktop'
        rc.write_atomic(desktop, desktop.read_bytes() + b'\n# Synthetic validation predecessor only\n',
                        manifest['entries']['share/applications/linux-controlpanel.desktop']['mode'])
        marker = directory / 'payload/share/linux-control/validator-predecessor.txt'
        require(not marker.exists(), 'Candidate uses reserved validation fixture marker')
        rc.write_atomic(marker, b'Synthetic validation metadata; never distributed.\n', 0o640)
        old = {**manifest, 'entries': rc.inventory(directory / 'payload', bounded=True)}
        data = rc.encoded(old)
        rc.write_atomic(directory / 'payload-manifest.json', data)
        rc.write_atomic(directory / 'payload-manifest.sha256', (rc.sha(data) + '\n').encode())
        return directory, old

    def runtime(self, destination, manifest, root, env, copied):
        forbidden = [Path(p).resolve() for p in self.args.forbid_root]
        recorded = {}
        for dependency in manifest.get('external_dependencies', []):
            path = Path(dependency['resolved_path'])
            require(path.is_absolute(), 'External dependency resolved path must be absolute')
            image = dependency['image']
            require(image.get('type') == 'file', 'External runtime dependency must be an ordinary file')
            # Check the original pin again even when an explicit fixture copy is used.
            require(rc.fingerprint(path) == image, 'External dependency drift: ' + str(path))
            recorded[str(path.resolve(strict=True))] = image
        require(recorded, 'Full pinned external runtime dependency inventory is required')
        executables, all_dependencies = [], {}
        for name, image in manifest['entries'].items():
            path = destination / name
            if not image['mode'] & 0o111:
                continue
            if rc.object_bytes(path, image)[:4] != b'\x7fELF':
                require(name != 'bin/control', 'Control executable must be ELF')
                continue
            label = name.replace('/', '-')
            dynamic = self.command('dynamic-' + label, ['/usr/bin/readelf', '-d', path], root, env)
            for search in re.findall(r'\((?:RPATH|RUNPATH)\).*?\[(.*?)\]', dynamic):
                for entry in search.split(':'):
                    require(bool(entry), 'Empty runtime search path refused')
                    if entry == '$ORIGIN' or entry.startswith('$ORIGIN/') or entry == '${ORIGIN}' or entry.startswith('${ORIGIN}/'):
                        expanded = entry.replace('${ORIGIN}', str(path.parent)).replace('$ORIGIN', str(path.parent))
                        require(within(Path(expanded).resolve(), destination), 'Runtime search path escapes staged prefix')
                    else:
                        search_path = Path(entry)
                        require(search_path.is_absolute() and any(within(search_path.resolve(), p) for p in SYSTEM_ROOTS),
                                'Non-system absolute/relative runtime search path refused: ' + entry)
                        require(not any(within(search_path.resolve(), p) for p in forbidden),
                                'Forbidden runtime search path: ' + entry)
            loader = self.command('loader-' + label, ['/usr/bin/ldd', path], root, env)
            dependencies = []
            for loader_name, supplied in parse_loader(loader):
                resolved = supplied.resolve(strict=True)
                resolved_image = rc.fingerprint(resolved)
                if str(resolved) in copied:
                    origin, pinned = copied[str(resolved)]
                    require(resolved_image == pinned, 'Fixture dependency copy changed: ' + str(resolved))
                elif within(resolved, destination):
                    relative = resolved.relative_to(destination).as_posix()
                    require(manifest['entries'].get(relative) == resolved_image,
                            'Unmanaged staged runtime dependency: ' + relative)
                    origin = None
                else:
                    require(not any(within(resolved, p) or within(supplied, p) for p in forbidden),
                            'Runtime resolved through checkout/development root: ' + str(resolved))
                    require(any(within(resolved, p) for p in SYSTEM_ROOTS),
                            'Non-system dependency must be supplied separately: ' + str(resolved))
                    require(recorded.get(str(resolved)) == resolved_image,
                            'Runtime dependency missing from pinned bundle inventory: ' + str(resolved))
                    origin = str(resolved)
                item = {'loader_name': loader_name, 'path': str(supplied), 'resolved_path': str(resolved),
                        'image': resolved_image, 'external_origin': origin}
                dependencies.append(item)
                all_dependencies[str(resolved)] = item
            executables.append({'path': name, 'dependencies': dependencies})
        require(any(item['path'] == 'bin/control' for item in executables), 'No ELF control executable checked')
        version = self.command('staged-version', [destination / 'bin/control', '--version'], root, env)
        for label, key in (('Build', 'build_id'), ('Source revision', 'revision'),
                           ('Source manifest SHA-256', 'source_manifest_sha256')):
            require(label + ': ' + manifest['source'][key] in version.splitlines(),
                    'Staged version identity mismatch: ' + label)
        for dependency in all_dependencies.values():
            require(rc.fingerprint(Path(dependency['resolved_path'])) == dependency['image'],
                    'Runtime dependency changed during probe: ' + dependency['resolved_path'])
        for path, image in recorded.items():
            require(rc.fingerprint(Path(path)) == image, 'Pinned dependency changed during probe: ' + path)
        self.report['runtime'] = {'executables': executables,
                                  'resolved_dependencies': list(all_dependencies.values()),
                                  'dependency_count': len(all_dependencies), 'version': version,
                                  'environment': 'allowlisted; private HOME/XDG; unreachable private bus endpoints; invalid Qt platform'}
        self.report['runtime_check_passed'] = True
        self.check('provider_free_identity_and_runtime_loading')

    def run(self):
        bundle, manifest = rc.load_bundle(self.args.bundle)
        for required in ('bin/control', 'share/applications/linux-controlpanel.desktop'):
            require(required in manifest['entries'], 'Required staged payload missing: ' + required)
        prefix = Path(manifest['prefix'])
        require(prefix.is_absolute() and '..' not in prefix.parts and str(prefix) == manifest['prefix'],
                'Invalid bundle prefix')
        self.report.update(source_manifest_sha256=manifest['source']['source_manifest_sha256'],
                           payload_manifest_sha256=rc.sha(rc.regular_bytes(bundle / 'payload-manifest.json')),
                           source=manifest['source'], payload_files=len(manifest['entries']))
        with tempfile.TemporaryDirectory(prefix='control-release-stage-') as temporary:
            root = Path(temporary)
            env = private_environment(root / 'private')
            work = root / 'unrelated-working-directory'
            work.mkdir()
            staging = root / 'staging'
            destination = staging / prefix.relative_to('/')
            state = root / 'journal'
            if self.args.previous_bundle:
                previous, old = rc.load_bundle(self.args.previous_bundle)
                require(old['prefix'] == manifest['prefix'], 'Previous bundle prefix mismatch')
                require(old['entries'] != manifest['entries'], 'Previous bundle must have differing payload bytes/modes/paths')
                self.report['predecessor'] = {'kind': 'provided release bundle',
                    'payload_manifest_sha256': rc.sha(rc.regular_bytes(previous / 'payload-manifest.json'))}
            else:
                previous, old = self.synthetic_predecessor(bundle, manifest, root / 'synthetic-predecessor')
                self.report['predecessor'] = {'kind': 'synthetic metadata fixture',
                    'scope': 'candidate executable bytes unchanged; desktop comment and fixture-only managed marker differ; not prior-release qualification'}
            names = set(manifest['entries']) | set(old['entries'])
            control = destination / 'bin/control'
            desktop = destination / 'share/applications/linux-controlpanel.desktop'
            rc.write_atomic(control, b'original private fixture application\n', 0o751)
            desktop.parent.mkdir(parents=True, exist_ok=True)
            desktop.symlink_to('../../original-private-menu')
            unrelated = destination / 'share/unrelated-private-data'
            prefs = staging / 'private-preferences/kwinrc'
            rc.write_atomic(unrelated, b'unrelated fixture bytes\n', 0o640)
            rc.write_atomic(prefs, b'private preferences untouched\n', 0o600)
            baseline = {name: rc.fingerprint(destination / name) for name in names}
            protected = {str(p): rc.fingerprint(p) for p in (unrelated, prefs)}
            copied, copied_evidence = {}, []
            pins = manifest.get('external_dependencies', [])
            for supplied in self.args.runtime_dependency:
                supplied = Path(supplied).absolute()
                resolved = supplied.resolve(strict=True)
                image = rc.fingerprint(resolved)
                require(any(d['resolved_path'] == str(resolved) and d['image'] == image for d in pins),
                        'Explicit runtime copy is not pinned in the bundle: ' + str(supplied))
                require(re.fullmatch(r'[^/]+\.so(?:\.[A-Za-z0-9_.+-]+)?', supplied.name) is not None,
                        'Runtime copy must use its shared-library loader name')
                target = destination / 'lib' / supplied.name
                require(not target.exists(), 'Duplicate fixture runtime copy: ' + str(target))
                rc.write_atomic(target, rc.object_bytes(resolved, image), image['mode'])
                copied[str(target.resolve())] = (str(resolved), image)
                protected[str(target)] = image
                copied_evidence.append({'path': str(supplied), 'resolved_path': str(resolved), 'image': image,
                                         'ownership': 'fixture only; outside application payload'})
            self.report['runtime_copies'] = copied_evidence
            def intact():
                for name, image in protected.items():
                    require(rc.fingerprint(Path(name)) == image, 'Unrelated/external fixture changed: ' + name)
            def install(which):
                self.operation('install', bundle=which, staging_root=staging, state=state)
            def operation(name):
                self.operation(name, state=state)
            install(previous)
            self.expected(destination, old['entries'], names)
            install(bundle)
            self.expected(destination, manifest['entries'], names)
            self.check('install_and_bytes_changing_upgrade')
            self.runtime(destination, manifest, work, env, copied)
            operation('rollback')
            self.expected(destination, old['entries'], names)
            self.check('rollback_to_predecessor')
            install(bundle)
            saved = rc.fingerprint(desktop)
            saved_data = rc.object_bytes(desktop, saved)
            rc.write_atomic(desktop, b'foreign intervening edit\n', saved['mode'])
            before = {name: rc.fingerprint(destination / name) for name in names}
            journal_before = rc.fingerprint(state / 'state.json')
            for name in ('install', 'rollback', 'remove'):
                try:
                    install(bundle) if name == 'install' else operation(name)
                except rc.Refusal as error:
                    require('Intervening edits' in str(error), 'Unexpected conflict refusal: ' + str(error))
                    self.events.append({'expected_refusal': name, 'reason': str(error)})
                else:
                    raise rc.Refusal('Intervening edit was not refused: ' + name)
                self.expected(destination, before, names)
                require(rc.fingerprint(state / 'state.json') == journal_before, 'Conflict changed deployment state')
            rc.write_atomic(desktop, saved_data, saved['mode'])
            self.check('intervening_edits_refused_before_mutation')
            operation('remove')
            self.expected(destination, baseline, names)
            intact()
            self.check('remove_restores_original_bytes_modes_symlink')
            install(previous)
            prior_state = rc.read_json(state / 'state.json')
            prior = {name: rc.fingerprint(destination / name) for name in names}
            actual_apply = rc.apply_object
            interruptions = []
            class DeliberateInterruption(Exception):
                pass
            def interrupt(path, image, data=None):
                changed = rc.fingerprint(path) != image
                actual_apply(path, image, data)
                if changed:
                    interruptions.append(str(path.relative_to(destination)))
                    raise DeliberateInterruption()
            try:
                with mock.patch.object(rc, 'apply_object', interrupt):
                    install(bundle)
            except DeliberateInterruption:
                pass
            require(len(interruptions) == 1 and (state / 'pending.json').is_file(),
                    'Did not interrupt after exactly one changed object')
            try:
                install(bundle)
            except rc.Refusal as error:
                require('Unfinished deployment' in str(error), 'Unexpected pending-transaction refusal')
            else:
                raise rc.Refusal('Pending transaction did not block a new install')
            operation('recover')
            self.expected(destination, prior, names)
            require(rc.read_json(state / 'state.json') == prior_state and not (state / 'pending.json').exists(),
                    'Recovery did not restore prior deployment state')
            self.report['interruption'] = {'scope': 'one boundary only; not exhaustive crash/power-loss testing',
                                           'after_changed_path': interruptions[0]}
            self.check('one_changed_object_interruption_and_recovery')
            operation('remove')
            self.expected(destination, baseline, names)
            intact()
            self.check('unrelated_preferences_and_external_runtime_preserved')
            # Re-read exact bundle after every fixture, catching accidental input writes.
            _, final_manifest = rc.load_bundle(bundle)
            require(final_manifest == manifest, 'Candidate bundle changed during validation')
            self.report['staged_deployment_passed'] = True
            self.report['status'] = 'passed'
        return self.report


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--bundle', required=True, type=Path)
    cli.add_argument('--output', required=True, type=Path, help='New evidence directory')
    cli.add_argument('--previous-bundle', type=Path)
    cli.add_argument('--runtime-dependency', action='append', default=[], type=Path,
                     help='Explicit pinned external library copied into fixture prefix/lib only')
    cli.add_argument('--forbid-root', action='append', default=[], type=Path,
                     help='Checkout, build or extracted development root forbidden in loader resolution')
    return cli


def main(argv=None):
    args = parser().parse_args(argv)
    validator = None
    try:
        output = rc.absolute(args.output)
        require(not os.path.lexists(output), 'Evidence output already exists')
        output.mkdir(parents=True, mode=0o700)
        validator = Validator(args, output)
        validator.run()
        return 0
    except (rc.Refusal, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        if validator:
            validator.report['error'] = str(error)
        print('REFUSED: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if validator:
            rc.write_atomic(output / 'evidence.json', rc.encoded(validator.report), 0o644)
            rc.write_atomic(output / 'transactions.json', rc.encoded(validator.events), 0o644)


if __name__ == '__main__':
    sys.exit(main())
