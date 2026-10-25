#!/usr/bin/env python3
"""Package an already built/tested RC bundle for the manual candidate updater.

This is a local packaging stage, not a Cloud build/test driver or publisher.
Recorded evidence is checked for consistency; this tool does not prove that the
recorded tests ran. It never contacts a server, executes payloads or installs.
"""
import argparse
import gzip
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

import rc_bundle as rc
import control_panel_update as consumer

REPOSITORY_ID = 1421857804
TARGET = {'architecture': 'x86_64', 'os_id': 'ubuntu', 'os_version_id': '26.04', 'session_type': 'wayland'}
SOURCE_FILES = ('source-manifest.json', 'source.tar.gz', 'identity.json', 'build-identity.cmake')
require = rc.require


def git(checkout, *arguments):
    result = subprocess.run(['git', '-C', str(checkout), *arguments], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False, timeout=30)
    require(result.returncode == 0, 'Local Git validation failed: ' + ' '.join(arguments[:2]))
    return result.stdout


def clean_source(checkout):
    top = git(checkout, 'rev-parse', '--show-toplevel').decode().strip()
    require(top == str(checkout), 'Checkout must name the complete repository root')
    flags = git(checkout, 'ls-files', '-v', '-z').split(b'\0')
    require(all(not entry or entry.startswith(b'H ') for entry in flags),
            'Assume-unchanged or sparse/skip-worktree source entries refused')
    require(not git(checkout, 'status', '--porcelain=v1', '--untracked-files=all', '-z'), 'Release checkout must be clean')
    commit = git(checkout, 'rev-parse', '--verify', 'HEAD^{commit}').decode().strip()
    main = git(checkout, 'rev-parse', '--verify', 'refs/heads/main^{commit}').decode().strip()
    require(consumer.COMMIT.fullmatch(commit) and consumer.COMMIT.fullmatch(main), 'Invalid local commit identity')
    git(checkout, 'merge-base', '--is-ancestor', commit, main)
    names = git(checkout, 'ls-files', '--cached', '-z').decode().split('\0')
    names = [name for name in names if name]
    require(names and len(names) == len(set(names)), 'Invalid tracked source inventory')
    entries = {}
    for name in sorted(names):
        rc.safe_relative(name)
        require(not name.startswith('.git/'), 'Git internals are not source inputs')
        image = rc.fingerprint(checkout / name)
        require(image['type'] in ('file', 'symlink'), 'Missing or unsupported tracked source input')
        entries[name] = image
    return commit, entries


def read_object(path, limit=consumer.MAX_JSON):
    data = rc.regular_bytes(path)
    require(len(data) <= limit, 'JSON exceeds packaging size limit')
    value = consumer.strict_json(data, limit=limit)
    require(isinstance(value, dict), 'JSON object required')
    return value, data


def verify_source(bundle, manifest, commit, entries):
    source = bundle / 'source'
    identity, _ = read_object(source / 'identity.json')
    frozen, frozen_bytes = read_object(source / 'source-manifest.json', 8 * 1024 * 1024)
    require(set(frozen) == {'schema', 'version', 'revision', 'entries'} and type(frozen['schema']) is int
            and frozen['schema'] == 1, 'Unsupported source manifest schema')
    require(set(identity) == {'version', 'revision', 'source_manifest_sha256', 'build_id', 'source_archive_sha256'},
            'Unsupported frozen identity schema')
    require(frozen['revision'] == commit and identity['revision'] == commit, 'Frozen source revision differs from checkout HEAD')
    require(frozen['entries'] == entries, 'Frozen source inputs differ from clean tracked checkout')
    version = frozen['version']
    require(isinstance(version, str) and re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_-]*', version), 'Invalid source release version')
    digest = rc.sha(frozen_bytes)
    require(identity == manifest['source'] and identity['version'] == version
            and identity['source_manifest_sha256'] == digest
            and identity['build_id'] == version + '+' + digest[:16], 'Frozen source identity mismatch')
    require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_-]{0,199}', identity['build_id']), 'Invalid release build ID')
    archive = source / 'source.tar.gz'
    archive_image = rc.fingerprint(archive)
    require(archive_image['type'] == 'file' and archive_image['sha256'] == identity['source_archive_sha256']
            and archive_image['bytes'] <= consumer.MAX_ARCHIVE, 'Frozen source archive hash/size mismatch')
    # Hashes bind bytes, while this check also binds their archived content to the
    # clean tracked checkout. Do not extract source or execute build instructions.
    seen, expanded = set(), 0
    with tarfile.open(archive, 'r:gz') as stream:
        for member in stream:
            require(len(seen) < consumer.MAX_MEMBERS and member.name in entries and member.name not in seen,
                    'Unexpected or duplicate frozen source member')
            seen.add(member.name)
            image = entries[member.name]
            if image['type'] == 'symlink':
                require(member.issym() and member.linkname == image['target'], 'Frozen source symlink mismatch')
            else:
                require(member.isfile() and member.size == image['bytes'] and member.mode == image['mode'],
                        'Frozen source file metadata mismatch')
                expanded += member.size
                require(expanded <= consumer.MAX_EXPANDED, 'Frozen source archive exceeds expanded limit')
                with stream.extractfile(member) as contents:
                    require(rc.sha(contents.read()) == image['sha256'], 'Frozen source file hash mismatch')
    require(seen == set(entries), 'Frozen source archive omits tracked inputs')
    variables = {'CONTROL_RELEASE_VERSION': version, 'CONTROL_SOURCE_REVISION': commit,
                 'CONTROL_SOURCE_MANIFEST_SHA256': digest, 'CONTROL_BUILD_ID': identity['build_id']}
    cmake = ''.join(f'set({key} "{value}" CACHE STRING "Frozen local RC identity" FORCE)\n' for key, value in variables.items())
    require(rc.regular_bytes(source / 'build-identity.cmake') == cmake.encode(), 'Frozen CMake identity mismatch')
    return identity


def verify_validation(path, identity, payload_sha):
    evidence, data = read_object(path)
    fields = {'schema', 'source_manifest_sha256', 'payload_manifest_sha256', 'complete_fixture_suite',
              'staged_deployment_passed', 'runtime_check_passed', 'qualification'}
    require(set(evidence) == fields and type(evidence['schema']) is int and evidence['schema'] == 1,
            'Unsupported validation evidence schema')
    require(evidence['source_manifest_sha256'] == identity['source_manifest_sha256']
            and evidence['payload_manifest_sha256'] == payload_sha, 'Validation evidence identifies another artifact')
    suite = evidence['complete_fixture_suite']
    require(isinstance(suite, dict) and set(suite) == {'total', 'passed', 'failed', 'not_run'}
            and all(type(value) is int for value in suite.values()) and suite['total'] > 0
            and suite['passed'] == suite['total'] and suite['failed'] == 0 and suite['not_run'] == 0,
            'Complete fixture suite did not pass')
    require(evidence['staged_deployment_passed'] is True and evidence['runtime_check_passed'] is True,
            'Staged deployment and runtime evidence must pass')
    require(evidence['qualification'] == 'candidate for acceptance', 'Release must remain a candidate for acceptance')
    return data


def dependencies(manifest):
    items = manifest.get('external_dependencies')
    require(isinstance(items, list) and 0 < len(items) <= 512, 'Nonempty exact external dependency inventory required')
    result, seen = [], set()
    for item in items:
        require(isinstance(item, dict) and isinstance(item.get('image'), dict), 'Invalid external dependency record')
        path = item.get('path')
        require(isinstance(path, str), 'Invalid dependency path')
        consumer.normalized(path)
        sha = item['image'].get('sha256')
        require(item['image'].get('type') == 'file' and isinstance(sha, str) and consumer.HEX.fullmatch(sha), 'Invalid dependency digest')
        require(path not in seen, 'Duplicate external dependency path')
        seen.add(path)
        result.append({'path': path, 'sha256': sha})
    return result


def archive_bundle(bundle, manifest, path):
    files = {'payload-manifest.json', 'payload-manifest.sha256'}
    files.update('source/' + name for name in SOURCE_FILES)
    files.update('payload/' + name for name in manifest['entries'])
    actual = set()
    for item in bundle.rglob('*'):
        require(not item.is_symlink() and (item.is_dir() or item.is_file()), 'Bundle links/special files refused')
        if item.is_file():
            actual.add(item.relative_to(bundle).as_posix())
    require(actual == files, 'Unexpected or missing bundle files')
    images = {name: rc.fingerprint(bundle / name) for name in files}
    directories = {parent.as_posix() for name in files for parent in Path(name).parents if parent != Path('.')}
    require(len(files) + len(directories) <= consumer.MAX_MEMBERS, 'Excessive bundle members')
    require(sum(image['bytes'] for image in images.values()) <= consumer.MAX_EXPANDED, 'Bundle exceeds expanded limit')
    with path.open('xb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w', format=tarfile.USTAR_FORMAT) as archive:
            for name in sorted(files | directories):
                info = tarfile.TarInfo(name)
                info.mtime = info.uid = info.gid = 0
                info.uname = info.gname = ''
                if name in directories:
                    info.type, info.mode = tarfile.DIRTYPE, 0o755
                    archive.addfile(info)
                else:
                    image = images[name]
                    require(image['type'] == 'file' and not image['mode'] & ~0o777, 'Unsupported bundle file mode')
                    info.mode, info.size = image['mode'], image['bytes']
                    archive.addfile(info, io.BytesIO(rc.object_bytes(bundle / name, image)))
    require(path.stat().st_size <= consumer.MAX_ARCHIVE, 'Bundle archive exceeds download limit')


def package(args):
    bundle, manifest = rc.load_bundle(args.bundle)
    checkout = rc.absolute(args.checkout)
    output = rc.absolute(args.output)
    require(not output.exists() and not output.is_symlink(), 'Output directory must be new')
    require(output.parent.is_dir(), 'Output parent directory must exist')
    require(type(args.sequence) is int and 0 < args.sequence < 10 ** 16, 'Sequence must be a positive integer of at most 16 digits')
    if output.is_relative_to(checkout):
        relative = output.relative_to(checkout).as_posix()
        git(checkout, 'check-ignore', '-q', relative + '/')
    commit, entries = clean_source(checkout)
    identity = verify_source(bundle, manifest, commit, entries)
    payload_sha = consumer.file_digest(bundle / 'payload-manifest.json')
    validation = verify_validation(Path(args.validation), identity, payload_sha)
    prefix = manifest['prefix']
    consumer.normalized(prefix)
    external = dependencies(manifest)
    tag = f'laptop-candidate-{args.sequence}-{commit[:12]}'
    with tempfile.TemporaryDirectory(prefix='control-release-', dir='/tmp') as temporary:
        stage = Path(temporary)
        archive = stage / 'control-panel-bundle.tar.gz'
        archive_bundle(bundle, manifest, archive)
        descriptor = {'schema': 1, 'repository': consumer.REPOSITORY, 'repository_id': REPOSITORY_ID,
                      'channel': consumer.CHANNEL, 'sequence': args.sequence, 'tag': tag, 'commit': commit,
                      'build_id': identity['build_id'], 'source_manifest_sha256': identity['source_manifest_sha256'],
                      'payload_manifest_sha256': payload_sha, 'archive_sha256': consumer.file_digest(archive),
                      'archive_size': archive.stat().st_size,
                      'target': {**TARGET, 'prefix': prefix, 'external_dependencies': external}, 'minimum_updater_version': 1}
        encoded = rc.encoded(descriptor)
        require(len(encoded) <= consumer.MAX_JSON, 'Release descriptor exceeds consumer limit')
        (stage / 'release.json').write_bytes(encoded)
        (stage / 'validation.json').write_bytes(validation)
        for asset in (archive, stage / 'release.json', stage / 'validation.json'):
            asset.chmod(0o644)
        consumer.safe_extract(archive, stage / 'extracted')
        fixture = consumer.Consumer(home=stage.absolute(), machine=lambda: dict(TARGET))
        fixture.config = {'target': dict(TARGET), 'prefix': prefix, 'repository_id': REPOSITORY_ID}
        fixture.validate_bundle(stage / 'extracted', descriptor)
        require(clean_source(checkout) == (commit, entries), 'Checkout changed during packaging')
        # Revalidate original inputs before committing output; no existing output
        # directory or file is replaced, even if another process creates one.
        _, current_manifest = rc.load_bundle(bundle)
        require(current_manifest == manifest and consumer.file_digest(bundle / 'payload-manifest.json') == payload_sha,
                'Payload manifest changed during packaging')
        require(verify_source(bundle, manifest, commit, entries) == identity, 'Source changed during packaging')
        require(verify_validation(Path(args.validation), identity, payload_sha) == validation, 'Evidence changed during packaging')
        output.mkdir(mode=0o700)
        created = []
        try:
            for name in ('control-panel-bundle.tar.gz', 'release.json', 'validation.json'):
                path = output / name
                descriptor_fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
                created.append(path)
                with os.fdopen(descriptor_fd, 'wb') as outgoing, (stage / name).open('rb') as incoming:
                    shutil.copyfileobj(incoming, outgoing)
                    outgoing.flush()
                    os.fsync(outgoing.fileno())
        except Exception:
            for path in created:
                path.unlink()
            output.rmdir()
            raise
    return descriptor


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('bundle', 'checkout', 'output', 'validation'):
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--sequence', type=int, required=True)
    args = parser.parse_args(argv)
    try:
        descriptor = package(args)
    except (rc.Refusal, consumer.Refusal, OSError, ValueError, KeyError, TypeError, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.exit(1, 'Release packaging refused: ' + str(error) + '\n')
    print(json.dumps({'tag': descriptor['tag'], 'build_id': descriptor['build_id'],
                      'output': str(args.output), 'qualification': 'candidate for acceptance',
                      'evidence': 'Recorded evidence checked; no fresh build or tests run by this packaging stage'}, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
