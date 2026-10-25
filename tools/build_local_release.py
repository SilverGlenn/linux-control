#!/usr/bin/env python3
"""Freeze, compile, test and privately validate a local candidate; never install live.

All outputs are new. A configuration pins explicit development inputs and runtime
roots; resolved transitive ELF libraries are inventoried separately. Successful
fixture evidence remains "candidate for acceptance", never hardware qualification.
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import xml.etree.ElementTree as ET

import rc_bundle as rc
import control_panel_update as consumer
import make_update_release as producer

DEFINITIONS = {'CMAKE_PREFIX_PATH', 'AeroQt_DIR', 'Qt6Positioning_DIR',
               'KNightTime_DIR', 'KF6Holidays_DIR', 'CUPS_INCLUDE_DIR', 'CUPS_LIBRARIES'}
require = rc.require


def normalized(value):
    require(isinstance(value, str) and value and '\x00' not in value, 'Invalid absolute path')
    path = Path(value)
    require(path.is_absolute() and str(path) == value and '..' not in path.parts,
            'Absolute normalized path required: ' + value)
    return path


def input_image(path):
    """Canonical pin: files hash bytes; directory hashes bind all leaves and links."""
    path = normalized(str(path))
    resolved = path.resolve(strict=True)
    if not resolved.is_dir():
        image = rc.fingerprint(resolved)
        require(image['type'] == 'file', 'Input must resolve to an ordinary file')
        return {'path': str(path), 'resolved_path': str(resolved), **image}
    entries = {}
    for directory, directories, files in os.walk(resolved, followlinks=False):
        base = Path(directory)
        for name in list(directories):
            candidate = base / name
            if candidate.is_symlink():
                directories.remove(name)
                files.append(name)
            else:
                entries[candidate.relative_to(resolved).as_posix()] = {
                    'type': 'directory', 'mode': stat.S_IMODE(candidate.stat().st_mode)}
        for name in files:
            candidate = base / name
            if candidate.is_symlink():
                require(candidate.resolve(strict=False).is_relative_to(resolved),
                        'Escaping development-tree link refused: ' + str(candidate))
            entries[candidate.relative_to(resolved).as_posix()] = rc.fingerprint(candidate)
    return {'path': str(path), 'resolved_path': str(resolved), 'type': 'directory',
            'sha256': rc.sha(rc.encoded(entries)), 'entries': len(entries),
            'mode': stat.S_IMODE(resolved.stat().st_mode)}


def configuration(path):
    config = consumer.strict_json(rc.regular_bytes(path), limit=8 * 1024 * 1024)
    require(isinstance(config, dict) and set(config) == {
        'schema', 'cmake_definitions', 'development_inputs', 'runtime_dependencies'}
        and type(config['schema']) is int and config['schema'] == 1, 'Unsupported dependency configuration')
    definitions = config['cmake_definitions']
    require(isinstance(definitions, dict) and set(definitions) <= DEFINITIONS,
            'Only dependency-location CMake definitions are allowed')
    for key, value in definitions.items():
        require(isinstance(value, str) and value and '\n' not in value, 'Invalid CMake dependency value')
        for item in value.split(';') if key == 'CMAKE_PREFIX_PATH' else [value]:
            normalized(item)
            require(Path(item).exists(), 'Missing dependency location: ' + item)
    images = {}
    for field in ('development_inputs', 'runtime_dependencies'):
        pins = config[field]
        require(isinstance(pins, list) and len(pins) <= 512, 'Invalid input pin list')
        for pin in pins:
            require(isinstance(pin, dict) and set(pin) == {'path', 'sha256'}
                    and isinstance(pin['sha256'], str) and consumer.HEX.fullmatch(pin['sha256']),
                    'Each input pin requires exactly path and SHA-256')
            require(pin['path'] not in images, 'Duplicate input pin')
            image = input_image(pin['path'])
            require(image['sha256'] == pin['sha256'], 'Input pin differs: ' + pin['path'])
            require(field != 'runtime_dependencies' or image['type'] == 'file',
                    'Runtime roots must be ordinary files')
            images[pin['path']] = image
    # Every explicit dependency search path must be covered by an exact pin or
    # pinned ancestor directory; unpinned cache overrides cannot silently drift.
    development = [images[pin['path']] for pin in config['development_inputs']]
    for key, value in definitions.items():
        for item in value.split(';') if key == 'CMAKE_PREFIX_PATH' else [value]:
            location = Path(item).resolve(strict=True)
            require(any(location == Path(image['resolved_path']) or
                        (image['type'] == 'directory' and location.is_relative_to(image['resolved_path']))
                        for image in development), 'Unpinned CMake dependency location: ' + item)
    return config, images


def source_entries(root):
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '--cached', '--others',
                             '--exclude-standard', '-z'], check=True, stdout=subprocess.PIPE)
    entries = {}
    for name in sorted(set(result.stdout.decode().split('\0')) - {''}):
        rc.safe_relative(name)
        require(not name.startswith('.git/'), 'Git internals refused')
        image = rc.fingerprint(root / name)
        if image['type'] != 'absent':
            entries[name] = image
    return entries


def verify_tree(root, entries):
    actual = {}
    for directory, directories, files in os.walk(root, followlinks=False):
        base = Path(directory)
        for name in list(directories):
            if (base / name).is_symlink():
                directories.remove(name)
                files.append(name)
        for name in files:
            path = base / name
            actual[path.relative_to(root).as_posix()] = rc.fingerprint(path)
    require(actual == entries, 'Extracted source bytes, modes or links changed')


def extract_source(source, destination):
    identity = rc.read_json(source / 'identity.json')
    manifest_bytes = rc.regular_bytes(source / 'source-manifest.json')
    require(rc.sha(manifest_bytes) == identity['source_manifest_sha256'], 'Source manifest digest differs')
    archive = source / 'source.tar.gz'
    require(rc.fingerprint(archive)['sha256'] == identity['source_archive_sha256'], 'Source archive digest differs')
    entries = json.loads(manifest_bytes)['entries']
    require(isinstance(entries, dict) and 0 < len(entries) <= consumer.MAX_MEMBERS, 'Invalid source inventory')
    for name, image in entries.items():
        rc.safe_relative(name)
        require(not name.startswith('.git/'), 'Git internals refused')
        require(image['type'] in ('file', 'symlink'), 'Unsupported source object')
        for ancestor in PurePosixPath(name).parents:
            require(entries.get(str(ancestor), {}).get('type') != 'symlink', 'Source symlink ancestor refused')
        if image['type'] == 'symlink':
            target = image['target']
            require(isinstance(target, str) and target and not Path(target).is_absolute(), 'Absolute source link refused')
            require(Path(os.path.abspath(destination / name / '..' / target)).is_relative_to(destination),
                    'Escaping source link refused')
    require(not destination.exists() and not destination.is_symlink(), 'Source extraction destination must be new')
    destination.mkdir(mode=0o700)
    seen, expanded, links = set(), 0, []
    with tarfile.open(archive, 'r:gz') as stream:
        for member in stream:
            require(member.name in entries and member.name not in seen, 'Unexpected or duplicate source archive member')
            seen.add(member.name)
            image = entries[member.name]
            path = destination / member.name
            require(member.uid == member.gid == member.mtime == 0, 'Unexpected source archive metadata')
            if image['type'] == 'symlink':
                require(member.issym() and member.linkname == image['target'], 'Source link mismatch')
                links.append((path, image['target']))
            else:
                require(member.isfile() and member.size == image['bytes'] and member.mode == image['mode'],
                        'Source archive metadata differs')
                expanded += member.size
                require(expanded <= consumer.MAX_EXPANDED, 'Source expanded size limit exceeded')
                with stream.extractfile(member) as incoming:
                    data = incoming.read()
                require(rc.sha(data) == image['sha256'], 'Source archive bytes differ')
                rc.write_atomic(path, data, image['mode'])
    require(seen == set(entries), 'Source archive omits inputs')
    for path, target in links:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
    # Lexical checks alone do not catch a chain whose intermediate link escapes.
    for path, _ in links:
        require(path.resolve(strict=False).is_relative_to(destination), 'Escaping source link chain refused')
    verify_tree(destination, entries)
    return identity, entries


def fixture_result(discovery, junit):
    tests = json.loads(Path(discovery).read_bytes())['tests']
    names = [item['name'] for item in tests]
    require(names and len(names) == len(set(names)), 'Empty or duplicate CTest discovery')
    require(all(not any(prop['name'] == 'DISABLED' and prop['value'] for prop in item.get('properties', []))
                for item in tests), 'Disabled CTest suite refused')
    root = ET.parse(junit).getroot()
    cases = list(root.iter('testcase'))
    require(len(cases) == len(names) and sorted(case.get('name') for case in cases) == sorted(names),
            'Executed CTest suite differs from complete discovery')
    require(root.get('tests') == str(len(names)) and all(int(root.get(key, '0')) == 0
            for key in ('failures', 'errors', 'skipped', 'disabled')), 'Incomplete or failed CTest suite')
    require(all(case.get('status') == 'run' and not any(case.find(tag) is not None
                for tag in ('failure', 'error', 'skipped')) for case in cases), 'A CTest suite did not pass')
    return {'total': len(names), 'passed': len(names), 'failed': 0, 'not_run': 0}


class Runner:
    def __init__(self, output):
        self.output = output
        self.commands = []
        self.environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                            'PYTHONDONTWRITEBYTECODE': '1', 'GIT_CONFIG_NOSYSTEM': '1',
                            'GIT_CONFIG_GLOBAL': '/dev/null', 'QT_QPA_PLATFORM': 'offscreen',
                            'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/local-release',
                            'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/local-release'}
        for variable, name in [('HOME', 'home'), ('XDG_CONFIG_HOME', 'config'), ('XDG_CONFIG_DIRS', 'defaults'),
                ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state'), ('XDG_DATA_HOME', 'data'),
                ('XDG_RUNTIME_DIR', 'runtime')]:
            path = output / 'private-environment' / name
            path.mkdir(parents=True, mode=0o700)
            self.environment[variable] = str(path)

    def run(self, name, command, env=None, cwd=None):
        command = [str(item) for item in command]
        print(name + ': ' + str(self.output / (name + '.log')), flush=True)
        record = {'phase': name, 'argv': command, 'cwd': str(cwd or self.output),
                  'environment_overrides': env or {}, 'started_unix': time.time()}
        self.commands.append(record)
        with (self.output / (name + '.log')).open('xb') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, cwd=cwd or self.output,
                                    env={**self.environment, **(env or {})})
        record.update(returncode=result.returncode, elapsed_seconds=time.time() - record['started_unix'])
        rc.write_atomic(self.output / 'commands.json', rc.encoded(self.commands))
        require(result.returncode == 0, name + ' failed; inspect ' + str(self.output / (name + '.log')))
        return self.output / (name + '.log')


def runtime_inventory(stage, roots, runner, forbidden):
    paths = {item['path'] for item in roots}
    environment = {'LD_LIBRARY_PATH': ':'.join(sorted({str(Path(item['path']).parent) for item in roots}))} if roots else {}
    for number, executable in enumerate(sorted(stage.rglob('*'))):
        if not executable.is_file() or executable.is_symlink():
            continue
        with executable.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        log = runner.run('dependencies-' + str(number), ['/usr/bin/ldd', executable], env=environment)
        for line in log.read_text().splitlines():
            require('not found' not in line, 'Unresolved staged runtime dependency')
            stripped = line.strip()
            match = re.match(r'\S+\s+=>\s+(/\S+)\s+\(', stripped) or re.match(r'(/\S+)\s+\(', stripped)
            if match:
                path = Path(match.group(1))
                resolved = path.resolve(strict=True)
                require(not any(resolved.is_relative_to(root) for root in forbidden),
                        'Runtime dependency resolves through a checkout/development path: ' + str(path))
                require(not resolved.is_relative_to(stage), 'Unexpected managed runtime library')
                paths.add(str(path))
            else:
                require(stripped.startswith('linux-vdso.so.'), 'Unrecognized ldd output: ' + stripped)
    require(paths, 'No external runtime dependencies discovered')
    return [input_image(path) for path in sorted(paths)]


def build(args):
    root, output = rc.absolute(args.root), rc.absolute(args.output)
    prefix = normalized(args.prefix)
    require(0 < args.jobs <= 2, 'Build parallelism must be 1 or 2')
    require(not args.rehearsal or args.sequence is None, 'Rehearsal cannot package an updater release')
    require(root.is_dir() and output.parent.is_dir(), 'Checkout and output parent must exist')
    require(not output.exists() and not output.is_symlink(), 'Output directory must be new')
    if output.is_relative_to(root):
        result = subprocess.run(['git', '-C', str(root), 'check-ignore', '-q',
                                 output.relative_to(root).as_posix() + '/'], check=False)
        require(result.returncode == 0, 'In-checkout output must be ignored by Git')
    config_path = rc.absolute(args.config)
    config, inputs = configuration(config_path)
    checkout_status = producer.git(root, 'status', '--porcelain=v1', '--untracked-files=all').decode()
    if not args.rehearsal:
        producer.clean_source(root)
        require(producer.git(root, 'symbolic-ref', '--quiet', 'HEAD').decode().strip() == 'refs/heads/main',
                'Release build requires the main branch; use --rehearsal for development')
    require(not any(output.is_relative_to(Path(image['resolved_path'])) for image in inputs.values()
                    if image['type'] == 'directory'), 'Output would change a pinned input tree')
    output.mkdir(mode=0o700)
    runner = Runner(output)
    qualification = 'development rehearsal' if args.rehearsal else 'candidate for acceptance'
    report = {'schema': 1, 'status': 'running', 'qualification': qualification,
              'mode': 'rehearsal' if args.rehearsal else 'release', 'initial_checkout_status': checkout_status,
              'prefix': str(prefix), 'jobs': args.jobs, 'configuration': config, 'input_images': inputs}
    try:
        rc.write_atomic(output / 'dependency-config.json', rc.encoded(config))
        for name, command in [('cmake-version', ['/usr/bin/cmake', '--version']),
                              ('ctest-version', ['/usr/bin/ctest', '--version']),
                              ('compiler-version', ['/usr/bin/c++', '--version'])]:
            runner.run(name, command)
        report['tools'] = {path: input_image(path) for path in
                           ('/usr/bin/cmake', '/usr/bin/ctest', '/usr/bin/c++', '/usr/bin/ldd',
                            '/usr/bin/readelf', '/usr/bin/dbus-run-session', sys.executable)}
        if Path('/usr/bin/dpkg-query').is_file():
            runner.run('installed-packages', ['/usr/bin/dpkg-query', '-W', '-f=${Package}\t${Version}\t${Architecture}\n'])
        runner.run('source-freeze', [sys.executable, root / 'tools/rc_bundle.py', 'source',
                   '--root', root, '--output', output / 'source', '--version', args.version])
        frozen = output / 'frozen-source'
        identity, entries = extract_source(output / 'source', frozen)
        report['source'] = identity
        require(source_entries(root) == entries, 'Checkout changed while freezing')
        archived = frozen / 'tools'
        cmake = ['/usr/bin/cmake', '-S', frozen, '-B', output / 'build', '-C', output / 'source/build-identity.cmake',
                 '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=ON', '-DCMAKE_INSTALL_PREFIX=' + str(prefix)]
        cmake += ['-D' + key + '=' + value for key, value in sorted(config['cmake_definitions'].items())]
        runner.run('configure', cmake)
        shutil.copyfile(output / 'build/CMakeCache.txt', output / 'CMakeCache.txt')
        runner.run('build', ['/usr/bin/cmake', '--build', output / 'build', '--parallel', args.jobs])
        discovery = runner.run('ctest-discovery', ['/usr/bin/ctest', '--test-dir', output / 'build', '--show-only=json-v1'])
        # Check discovery before execution as well, refusing a deliberately disabled fixture.
        discovered = json.loads(discovery.read_bytes())['tests']
        require(discovered and all(not any(prop['name'] == 'DISABLED' and prop['value']
                for prop in item.get('properties', [])) for item in discovered), 'Empty or disabled CTest suite')
        runner.run('ctest', [sys.executable, archived / 'run_isolated_tests.py', '--work-dir', output / 'test-environment', '--',
                   '/usr/bin/ctest', '--test-dir', output / 'build', '--output-on-failure', '--no-tests=error',
                   '--parallel', args.jobs, '--output-junit', output / 'ctest.xml'])
        suite = fixture_result(discovery, output / 'ctest.xml')
        report['complete_fixture_suite'] = suite
        runner.run('destdir-install', ['/usr/bin/cmake', '--install', output / 'build'], env={'DESTDIR': str(output / 'destdir')})
        stage = output / 'destdir' / str(prefix).lstrip('/')
        development_roots = [Path(image['resolved_path']) for image in inputs.values() if image['type'] == 'directory'
                             and not str(image['resolved_path']).startswith('/usr/')]
        forbidden = [root, frozen, output / 'build', *development_roots]
        roots = [inputs[pin['path']] for pin in config['runtime_dependencies']]
        for index, item in enumerate(roots):
            runner.run('runtime-root-dynamic-' + str(index), ['/usr/bin/readelf', '-d', item['path']])
        runtime = runtime_inventory(stage, roots, runner, forbidden)
        report['runtime_dependencies'] = runtime
        bundle_command = [sys.executable, archived / 'rc_bundle.py', 'bundle', '--stage', stage, '--prefix', prefix,
                          '--source', output / 'source', '--output', output / 'bundle']
        for item in runtime:
            bundle_command += ['--dependency', item['path']]
        runner.run('bundle', bundle_command)
        validator = [sys.executable, archived / 'run_isolated_tests.py', '--work-dir', output / 'staged-environment', '--',
                     sys.executable, archived / 'validate_release_payload.py', '--bundle', output / 'bundle',
                     '--output', output / 'staged-validation']
        for pin in config['runtime_dependencies']:
            validator += ['--runtime-dependency', pin['path']]
        for path in forbidden:
            validator += ['--forbid-root', path]
        runner.run('staged-validation', validator)
        staged = rc.read_json(output / 'staged-validation/evidence.json')
        payload_sha = rc.sha(rc.regular_bytes(output / 'bundle/payload-manifest.json'))
        require(staged.get('schema') == 1 and staged.get('status') == 'passed'
                and staged.get('source_manifest_sha256') == identity['source_manifest_sha256']
                and staged.get('payload_manifest_sha256') == payload_sha
                and staged.get('staged_deployment_passed') is True and staged.get('runtime_check_passed') is True,
                'Staged validator evidence does not pass or identifies another artifact')
        verify_tree(frozen, entries)
        require(source_entries(root) == entries, 'Checkout source changed during release build')
        if not args.rehearsal:
            commit, tracked = producer.clean_source(root)
            require(commit == identity['revision'] and tracked == entries
                    and producer.git(root, 'symbolic-ref', '--quiet', 'HEAD').decode().strip() == 'refs/heads/main',
                    'Clean main checkout changed during release build')
        require(configuration(config_path) == (config, inputs), 'Dependency configuration or pins changed')
        require(all(input_image(item['path']) == item for item in runtime), 'Runtime dependencies changed during validation')
        _, final_manifest = rc.load_bundle(output / 'bundle')
        require(final_manifest['source'] == identity
                and rc.sha(rc.regular_bytes(output / 'bundle/payload-manifest.json')) == payload_sha,
                'Bundle changed after staged validation')
        validation = {'schema': 1, 'source_manifest_sha256': identity['source_manifest_sha256'],
                      'payload_manifest_sha256': payload_sha, 'complete_fixture_suite': suite,
                      'staged_deployment_passed': True, 'runtime_check_passed': True,
                      'qualification': qualification}
        validation_name = 'rehearsal-validation.json' if args.rehearsal else 'validation.json'
        rc.write_atomic(output / validation_name, rc.encoded(validation), 0o644)
        if args.sequence is not None:
            runner.run('package-update', [sys.executable, archived / 'make_update_release.py', '--bundle', output / 'bundle',
                       '--checkout', root, '--sequence', args.sequence, '--output', output / 'release',
                       '--validation', output / 'validation.json'])
        report.update(status='passed', payload_manifest_sha256=payload_sha)
        return report
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        rc.write_atomic(output / 'build-report.json', rc.encoded(report))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-digest', type=Path, help='Print a local input image for an explicit configuration pin; do not build')
    parser.add_argument('--check-config', type=Path, help='Check all configuration pins and print their images; do not build')
    for name in ('root', 'output', 'config'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--version')
    parser.add_argument('--prefix', default='/home/siru/.local')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--sequence', type=int, help='Additionally package via the existing clean-main gate; never publish')
    parser.add_argument('--rehearsal', action='store_true', help='Allow dirty/non-main development; no release validation or updater packaging')
    args = parser.parse_args(argv)
    try:
        if args.input_digest:
            require(not args.check_config, 'Choose a single inspection mode')
            result = input_image(args.input_digest)
        elif args.check_config:
            config, images = configuration(rc.absolute(args.check_config))
            result = {'configuration': config, 'input_images': images}
        else:
            require(all(getattr(args, key) is not None for key in ('root', 'output', 'config', 'version')),
                    'Build requires --root, --output, --config and --version')
            require(args.sequence is None or 0 < args.sequence < 10 ** 16, 'Invalid packaging sequence')
            result = build(args)
    except (rc.Refusal, consumer.Refusal, OSError, ValueError, KeyError, TypeError,
            tarfile.TarError, subprocess.SubprocessError, ET.ParseError, RuntimeError) as error:
        parser.exit(1, 'Local release refused: ' + str(error) + '\n')
    print(json.dumps(result, sort_keys=True, indent=2))


if __name__ == '__main__':
    main()
