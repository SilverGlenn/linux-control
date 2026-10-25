#!/usr/bin/python3
"""Private stdlib fixtures; no network, user prefix or desktop providers."""
import contextlib
import copy
import gzip
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import control_panel_update as update
import rc_bundle
import install_lock


def encoded(value):
    return json.dumps(value, sort_keys=True).encode()


class FakeGitHub:
    def __init__(self, fixture):
        self.fixture = fixture
        self.calls = []
        self.fail_asset = None
        self.bad_repository = False
        self.bad_tag = False
        self.bad_ancestry = False
        self.empty = False

    def api(self, endpoint):
        self.calls.append(('api', endpoint))
        f = self.fixture
        if endpoint == 'repos/' + update.REPOSITORY:
            return {'id': 999 if self.bad_repository else 123, 'full_name': update.REPOSITORY}
        if '/git/ref/tags/' in endpoint:
            return {'object': {'type': 'commit', 'sha': 'c' * 40 if self.bad_tag else f.commit}}
        if '/compare/' in endpoint:
            return {'status': 'diverged' if self.bad_ancestry else 'ahead', 'merge_base_commit': {'sha': f.commit}}
        if '/releases/tags/' in endpoint:
            return copy.deepcopy(f.release)
        if endpoint.endswith('/releases?per_page=100'):
            return [] if self.empty else [copy.deepcopy(f.release)]
        raise AssertionError(endpoint)

    def verify(self, tag):
        self.calls.append(('verify', tag))

    def verify_asset(self, tag, path):
        self.calls.append(('verify_asset', path.name))
        if path.name == self.fail_asset:
            raise update.Refusal('fake attestation failure')

    def download(self, asset, path, maximum):
        self.calls.append(('download', path.name))
        data = self.fixture.descriptor_bytes if path.name == 'release.json' else self.fixture.archive
        if len(data) > maximum:
            raise update.Refusal('fake oversized download')
        path.write_bytes(data)
        path.chmod(0o600)


class Fixture:
    def __init__(self, root):
        self.home = root / 'home'
        self.home.mkdir(mode=0o700)
        self.prefix = self.home / '.local'
        self.commit = 'a' * 40
        self.tag = 'laptop-candidate-1-' + self.commit[:12]
        self.build = '0.1.0-rc.2+' + 'b' * 16
        self.dependency = root / 'external.so'
        self.dependency.write_bytes(b'external dependency')
        self.machine = {'architecture': 'x86_64', 'os_id': 'ubuntu', 'os_version_id': '26.04', 'session_type': 'wayland'}
        self.config = {'schema': 1, 'repository': update.REPOSITORY, 'repository_id': 123,
                       'channel': update.CHANNEL, 'prefix': str(self.prefix),
                       'deployment_state': str(self.home / '.control-panel-rc-deployment'),
                       'gh': '/usr/bin/gh', 'target': self.machine}
        config_dir = self.home / '.config/linux-control'
        config_dir.mkdir(parents=True, mode=0o700)
        update.atomic(config_dir / 'updater.json', self.config)
        self.source_manifest = encoded({'schema': 1, 'entries': {}})
        self.source_archive = b'frozen source fixture; never executed'
        self.source_identity = {'version': '0.1.0-rc.2', 'revision': self.commit, 'build_id': self.build,
                                'source_manifest_sha256': update.digest(self.source_manifest),
                                'source_archive_sha256': update.digest(self.source_archive)}
        files = {'bin/control': (b'ELF fixture; probe is injected', 0o755),
                 'share/applications/linux-controlpanel.desktop': (f'Exec={self.prefix}/bin/control\n'.encode(), 0o644)}
        self.payload = {'schema': 1, 'prefix': str(self.prefix), 'source': self.source_identity,
                        'entries': {name: {'type': 'file', 'sha256': update.digest(data), 'bytes': len(data), 'mode': mode}
                                    for name, (data, mode) in files.items()},
                        'external_dependencies': [{'path': str(self.dependency), 'resolved_path': str(self.dependency),
                                                   'image': rc_bundle.fingerprint(self.dependency)}]}
        payload_bytes = encoded(self.payload)
        self.members = {**{'payload/' + name: (data, mode) for name, (data, mode) in files.items()},
                        'payload-manifest.json': (payload_bytes, 0o644),
                        'payload-manifest.sha256': ((update.digest(payload_bytes) + '\n').encode(), 0o644),
                        'source/source-manifest.json': (self.source_manifest, 0o644),
                        'source/source.tar.gz': (self.source_archive, 0o644),
                        'source/identity.json': (encoded(self.source_identity), 0o644),
                        'source/build-identity.cmake': (b'# frozen identity\n', 0o644)}
        self.archive = self.make_archive(self.members)
        self.descriptor = {'schema': 1, 'repository': update.REPOSITORY, 'repository_id': 123, 'channel': update.CHANNEL,
                           'sequence': 1, 'tag': self.tag, 'commit': self.commit, 'build_id': self.build,
                           'source_manifest_sha256': update.digest(self.source_manifest),
                           'payload_manifest_sha256': update.digest(payload_bytes),
                           'archive_sha256': update.digest(self.archive), 'archive_size': len(self.archive),
                           'target': {**self.machine, 'prefix': str(self.prefix), 'external_dependencies': [
                               {'path': str(self.dependency), 'sha256': update.file_digest(self.dependency)}]},
                           'minimum_updater_version': 1}
        self.release = {'id': 10, 'tag_name': self.tag, 'target_commitish': self.commit,
                        'draft': False, 'published_at': '2026-10-25T00:00:00Z', 'immutable': True,
                        'assets': [{'id': 1, 'name': 'release.json', 'size': 1, 'state': 'uploaded'},
                                   {'id': 2, 'name': 'control-panel-bundle.tar.gz', 'size': len(self.archive), 'state': 'uploaded'}]}
        self.refresh()
        self.github = FakeGitHub(self)
        self.installs, self.probes, self.locks = [], [], []
        self.fail_install, self.fail_repair, self.fail_post_probe = False, False, False
        self.consumer = update.Consumer(home=self.home, github=self.github, machine=lambda: dict(self.machine),
                                        installer=self.install, probe=self.probe, locker=self.lock)

    @staticmethod
    def make_archive(members):
        output = io.BytesIO()
        with tarfile.open(fileobj=output, mode='w:gz', format=tarfile.USTAR_FORMAT) as archive:
            for name, (data, mode) in members.items():
                info = tarfile.TarInfo(name)
                info.size, info.mode = len(data), mode
                archive.addfile(info, io.BytesIO(data))
        return output.getvalue()

    def refresh(self):
        self.descriptor_bytes = encoded(self.descriptor)
        self.release['assets'][0]['size'] = len(self.descriptor_bytes)
        self.release['assets'][1]['size'] = len(self.archive)

    def install(self, operation, bundle, config):
        self.installs.append(operation)
        if (operation == 'install' and self.fail_install) or (operation == 'repair' and self.fail_repair):
            raise update.Refusal('fixture mechanical failure')

    def probe(self, bundle, descriptor, prefix=None):
        self.probes.append(prefix)
        if prefix and self.fail_post_probe:
            raise update.Refusal('fixture post-install identity failure')

    @contextlib.contextmanager
    def lock(self, destination, exclusive, wait_seconds):
        self.locks.append((destination, exclusive, wait_seconds))
        yield

    def prepare(self):
        return self.consumer.check('old-build', self.tag, prepare=True)


class ConsumerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='control-update-fixture-')
        self.root = Path(self.temp.name)
        self.f = Fixture(self.root)
        self.c = self.f.consumer

    def tearDown(self):
        self.temp.cleanup()

    def test_production_runtime_pins_private_siblings_and_refuses_missing_module(self):
        runtime = self.root / 'runtime'
        runtime.mkdir(mode=0o700)
        for name in ('control_panel_update.py', 'install_lock.py', 'rc_bundle.py'):
            target = runtime / name
            target.write_bytes((TOOLS / name).read_bytes())
            target.chmod(0o600)
        with mock.patch.object(update, '__file__', str(runtime / 'control_panel_update.py')):
            with mock.patch.dict(sys.modules):
                self.c.production_runtime()
                self.assertEqual(sys.modules['rc_bundle'].__file__, str(runtime / 'rc_bundle.py'))
                self.assertEqual(sys.modules['install_lock'].__file__, str(runtime / 'install_lock.py'))
            (runtime / 'rc_bundle.py').unlink()
            with mock.patch.dict(sys.modules), self.assertRaises(FileNotFoundError):
                self.c.production_runtime()
        runtime.chmod(0o755)
        with mock.patch.object(update, '__file__', str(runtime / 'control_panel_update.py')):
            with self.assertRaisesRegex(update.Refusal, 'permissions'):
                self.c.production_runtime()

    def test_status_is_local_and_unconfigured_is_honest(self):
        self.assertEqual(self.c.status('old')['status'], 'idle')
        self.assertEqual(self.f.github.calls, [])
        self.c.config_path.unlink()
        with self.assertRaises(update.Refusal) as caught:
            self.c.status('old')
        self.assertEqual(caught.exception.status, 'not_configured')

    def test_check_available_attests_descriptor_never_extracts_or_probes(self):
        with mock.patch.object(update, 'safe_extract', side_effect=AssertionError('unexpected extraction')):
            result = self.c.check('old-build')
        self.assertEqual(result['status'], 'available')
        self.assertEqual(self.f.probes, [])
        self.assertIn(('verify_asset', 'release.json'), self.f.github.calls)
        self.assertNotIn(('download', 'control-panel-bundle.tar.gz'), self.f.github.calls)
        self.assertEqual(self.c.status('old-build'), result)

    def test_current_build(self):
        self.assertEqual(self.c.check(self.f.build)['status'], 'up_to_date')

    def test_prepare_apply_exact_bytes_and_replay_refusal(self):
        result = self.f.prepare()
        self.assertEqual(result['status'], 'ready')
        self.assertTrue(Path(result['receipt']).is_absolute())
        self.assertEqual(self.f.installs, [])
        installed = self.c.apply(result['receipt'], 30)
        self.assertEqual(installed['status'], 'installed')
        self.assertEqual(self.f.installs, ['install'])
        self.assertEqual(self.f.locks, [(str(self.f.prefix), True, 30)])
        self.assertEqual(self.c.status(self.f.build), installed)
        with self.assertRaisesRegex(update.Refusal, 'already consumed'):
            self.c.apply(result['receipt'], 30)
        self.assertFalse(self.f.prefix.exists())

    def test_real_rc_install_transaction_in_private_home(self):
        result = self.f.prepare()
        self.c.installer = self.c.locker = None
        with mock.patch.object(Path, 'home', return_value=self.f.home):
            installed = self.c.apply(result['receipt'], 0)
        self.assertEqual(installed['status'], 'installed')
        self.assertEqual((self.f.prefix / 'bin/control').read_bytes(), self.f.members['payload/bin/control'][0])
        state = update.read_private(Path(self.f.config['deployment_state']) / 'state.json')
        self.assertEqual(state['source']['build_id'], self.f.build)
        self.assertEqual(len(state['history']), 1)

    def test_real_rc_failure_restores_original_bytes(self):
        original = self.f.prefix / 'bin/control'
        original.parent.mkdir(parents=True)
        original.write_bytes(b'original application bytes')
        original.chmod(0o755)
        result = self.f.prepare()
        self.c.installer = self.c.locker = None
        self.f.fail_post_probe = True
        with mock.patch.object(Path, 'home', return_value=self.f.home):
            outcome = self.c.apply(result['receipt'], 0)
        self.assertEqual(outcome['status'], 'rolled_back')
        self.assertEqual(original.read_bytes(), b'original application bytes')
        self.assertFalse((self.f.prefix / 'share/applications/linux-controlpanel.desktop').exists())
        self.assertEqual(self.c.memory()['highwater'], 1)

    def test_preexisting_pending_is_never_recovered_by_new_attempt(self):
        result = self.f.prepare()
        state = Path(self.f.config['deployment_state'])
        state.mkdir(mode=0o700)
        pending = state / 'pending.json'
        pending.write_text('{"existing":"interrupted operation"}')
        with self.assertRaisesRegex(update.Refusal, 'earlier interrupted'):
            self.c.apply(result['receipt'], 0)
        self.assertEqual(self.f.installs, [])
        self.assertEqual(pending.read_text(), '{"existing":"interrupted operation"}')
        self.assertFalse(update.read_private(Path(result['receipt']))['used'])

    def test_detached_failure_is_persisted_including_incompatible(self):
        result = self.f.prepare()
        self.f.dependency.write_bytes(b'changed')
        output = io.StringIO()
        with mock.patch.object(update, 'Consumer', return_value=self.c), mock.patch.object(self.c, 'production_runtime'), contextlib.redirect_stdout(output):
            update.main(['apply', '--receipt', result['receipt'], '--wait-seconds', '0'])
        response = json.loads(output.getvalue())
        self.assertEqual(response['status'], 'incompatible')
        self.assertEqual(self.c.status('old'), response)
        self.assertFalse(update.read_private(Path(result['receipt']))['used'])

    def test_independent_rollback_status_preserves_highwater_and_quarantine(self):
        result = self.f.prepare()
        self.c.installer = self.c.locker = None
        with mock.patch.object(Path, 'home', return_value=self.f.home):
            self.assertEqual(self.c.apply(result['receipt'], 0)['status'], 'installed')
            with contextlib.redirect_stdout(io.StringIO()):
                rc_bundle.rollback_or_remove(type('Arguments', (), {
                    'state': self.f.config['deployment_state'], 'command': 'rollback'})())
        self.assertEqual(self.c.status('original-build')['status'], 'rolled_back')
        self.assertEqual(self.c.memory()['highwater'], 1)
        self.assertIn(self.f.build, self.c.memory()['quarantine'])
        self.assertEqual(self.f.github.calls.count(('verify', self.f.tag)), 2)

    def test_consumed_receipt_interruption_reconciles_status(self):
        result = self.f.prepare()
        path = Path(result['receipt'])
        receipt = update.read_private(path)
        receipt['used'] = True
        update.atomic(path, receipt)
        status = self.c.status('old')
        self.assertEqual(status['status'], 'manual_repair')
        self.assertNotIn('receipt', status)
        self.assertIn(self.f.build, self.c.memory()['quarantine'])

    def test_failed_post_install_quarantines_and_preserves_highwater(self):
        receipt = self.f.prepare()['receipt']
        self.f.fail_post_probe = True
        self.assertEqual(self.c.apply(receipt, 0)['status'], 'rolled_back')
        self.assertEqual(self.f.installs, ['install', 'repair'])
        self.assertEqual(self.c.memory()['highwater'], 1)
        with self.assertRaisesRegex(update.Refusal, 'quarantined'):
            self.f.prepare()

    def test_repair_failure_is_visible_and_persisted(self):
        receipt = self.f.prepare()['receipt']
        self.f.fail_install = self.f.fail_repair = True
        self.assertEqual(self.c.apply(receipt, 0)['status'], 'manual_repair')
        self.assertEqual(self.c.status('old')['status'], 'manual_repair')

    def test_bad_attestation_never_extracts_or_executes(self):
        for asset in ('release.json', 'control-panel-bundle.tar.gz'):
            with self.subTest(asset=asset):
                self.f.github.fail_asset = asset
                with mock.patch.object(update, 'safe_extract', side_effect=AssertionError('unsafe extraction')):
                    with self.assertRaisesRegex(update.Refusal, 'attestation'):
                        self.f.prepare()
                self.assertEqual(self.f.probes, [])

    def test_wrong_repository_and_tag_ancestry_refused(self):
        for flag in ('bad_repository', 'bad_tag', 'bad_ancestry'):
            with self.subTest(flag=flag):
                setattr(self.f.github, flag, True)
                with self.assertRaises(update.Refusal):
                    self.c.check('old')
                setattr(self.f.github, flag, False)

    def test_nonimmutable_draft_and_unpublished_refused(self):
        for key, bad in (('immutable', False), ('draft', True), ('published_at', None)):
            with self.subTest(key=key):
                prior = self.f.release[key]
                self.f.release[key] = bad
                with self.assertRaises(update.Refusal):
                    self.c.check('old', self.f.tag)
                self.f.release[key] = prior

    def test_missing_assets_and_duplicate_assets_refused(self):
        saved = copy.deepcopy(self.f.release['assets'])
        for assets in (saved[:1], saved + saved[:1]):
            self.f.release['assets'] = assets
            with self.assertRaises(update.Refusal):
                self.f.prepare()

    def test_descriptor_wrong_schema_unknown_keys_and_minimum_version(self):
        for key, bad in (('schema', 2), ('repository_id', 456), ('channel', 'latest'),
                         ('minimum_updater_version', 2), ('sequence', True), ('unexpected', 'field')):
            with self.subTest(key=key):
                saved = copy.deepcopy(self.f.descriptor)
                self.f.descriptor[key] = bad
                self.f.refresh()
                with self.assertRaises(update.Refusal):
                    self.f.prepare()
                self.f.descriptor = saved
                self.f.refresh()

    def test_actual_machine_mismatch_and_dependency_drift(self):
        self.f.machine['session_type'] = 'x11'
        with self.assertRaises(update.Refusal):
            self.f.prepare()
        self.f.machine['session_type'] = 'wayland'
        self.f.dependency.write_bytes(b'drift')
        with self.assertRaisesRegex(update.Refusal, 'dependency'):
            self.f.prepare()
        self.assertEqual(self.f.installs, [])

    def test_archive_hash_and_size_refused_before_extract(self):
        self.f.archive += b'corrupted'
        self.f.refresh()
        self.f.descriptor['archive_size'] = len(self.f.archive)
        self.f.refresh()
        with mock.patch.object(update, 'safe_extract', side_effect=AssertionError('unexpected extraction')):
            with self.assertRaisesRegex(update.Refusal, 'hash/size'):
                self.f.prepare()

    def test_descriptor_and_archive_substitution_refused(self):
        receipt = Path(self.f.prepare()['receipt'])
        descriptor = receipt.parent / 'release.json'
        original = descriptor.read_bytes()
        descriptor.write_bytes(b'{}')
        with self.assertRaisesRegex(update.Refusal, 'substituted'):
            self.c.apply(str(receipt), 0)
        descriptor.write_bytes(original)
        archive = receipt.parent / 'control-panel-bundle.tar.gz'
        archive.write_bytes(b'forged')
        with self.assertRaisesRegex(update.Refusal, 'hash/size'):
            self.c.apply(str(receipt), 0)
        self.assertEqual(self.f.installs, [])

    def test_apply_reextracts_verified_archive_ignoring_mutated_tree(self):
        result = self.f.prepare()
        candidate = Path(result['receipt']).parent / 'bundle/payload/bin/control'
        candidate.write_bytes(b'substituted previously extracted bytes')
        self.assertEqual(self.c.apply(result['receipt'], 0)['status'], 'installed')

    def test_receipt_outside_cache_or_symlink_refused(self):
        result = self.f.prepare()
        real = Path(result['receipt'])
        alias = real.parent / 'alias.json'
        alias.symlink_to(real)
        for path in (alias, self.root / 'receipt.json'):
            with self.assertRaises(update.Refusal):
                self.c.apply(str(path), 0)
        real.unlink()
        real.symlink_to(alias)
        with self.assertRaises(update.Refusal):
            self.c.apply(str(real), 0)

    def test_release_identity_must_match_receipt(self):
        receipt = self.f.prepare()['receipt']
        self.f.release['id'] = 11
        with self.assertRaisesRegex(update.Refusal, 'identity changed'):
            self.c.apply(receipt, 0)

    def test_sequence_downgrade_and_same_sequence_rebinding(self):
        self.c.check('old')
        state = self.c.memory()
        state['highwater'] = 2
        self.c.save(state)
        with self.assertRaisesRegex(update.Refusal, 'high-water'):
            self.c.check('old')
        state['highwater'] = 1
        state['identity']['archive_sha256'] = '0' * 64
        self.c.save(state)
        with self.assertRaisesRegex(update.Refusal, 'reused'):
            self.c.check('old')

    def test_private_config_and_state_permissions(self):
        self.c.config_path.chmod(0o644)
        with self.assertRaisesRegex(update.Refusal, 'permissions'):
            self.c.status('old')
        self.c.config_path.chmod(0o600)
        self.c.root.mkdir(mode=0o755)
        with self.assertRaisesRegex(update.Refusal, 'permissions'):
            self.c.status('old')

    def test_journal_is_canonical_and_pending_is_visible(self):
        changed = dict(self.f.config, deployment_state=str(self.root / 'different-journal'))
        update.atomic(self.c.config_path, changed)
        with self.assertRaisesRegex(update.Refusal, 'Noncanonical'):
            self.c.status('old')
        update.atomic(self.c.config_path, self.f.config)
        journal = Path(self.f.config['deployment_state'])
        journal.mkdir(mode=0o700)
        (journal / 'pending.json').write_text('{}')
        self.assertEqual(self.c.status('old')['status'], 'manual_repair')
        self.assertEqual(self.f.github.calls, [])

    def test_no_releases_honest(self):
        self.f.github.empty = True
        with self.assertRaises(update.Refusal) as caught:
            self.c.check('old')
        self.assertEqual(caught.exception.status, 'no_releases')

    def test_apply_busy_preserves_receipt_for_retry(self):
        result = self.f.prepare()
        @contextlib.contextmanager
        def busy(*args, **kwargs):
            raise update.Refusal('App is still open')
            yield
        self.c.locker = busy
        with self.assertRaisesRegex(update.Refusal, 'still open'):
            self.c.apply(result['receipt'], 0)
        self.assertFalse(update.read_private(Path(result['receipt']))['used'])
        self.assertEqual(self.f.installs, [])

    def test_canonical_busy_is_deferred_but_unsafe_lock_is_error(self):
        result = self.f.prepare()
        @contextlib.contextmanager
        def busy(*args, **kwargs):
            raise install_lock.Busy('fixture app remains open')
            yield
        self.c.locker = busy
        outcome = self.c.apply(result['receipt'], 0)
        self.assertEqual(outcome['status'], 'ready')
        self.assertIn('deferred', outcome['message'])
        @contextlib.contextmanager
        def unsafe(*args, **kwargs):
            raise install_lock.Refusal('unsafe lock path')
            yield
        self.c.locker = unsafe
        with self.assertRaisesRegex(install_lock.Refusal, 'unsafe lock'):
            self.c.apply(result['receipt'], 0)
        self.assertEqual(self.f.installs, [])
        self.assertFalse(update.read_private(Path(result['receipt']))['used'])

    def test_apply_rechecks_dependencies_before_install(self):
        result = self.f.prepare()
        self.f.dependency.write_bytes(b'changed')
        with self.assertRaisesRegex(update.Refusal, 'dependency'):
            self.c.apply(result['receipt'], 0)
        self.assertEqual(self.f.installs, [])


class HeadlessProbeTests(unittest.TestCase):
    def test_staged_probe_uses_only_verified_target_library_directory(self):
        with tempfile.TemporaryDirectory(prefix='update-loader-fixture-') as temporary:
            root = Path(temporary)
            bundle, prefix = root / 'bundle', root / 'home/.local'
            staged = bundle / 'payload/bin/control'
            installed = prefix / 'bin/control'
            # A valid ELF identity header reaches the execution seam, without
            # executing code or loading a library on the host.
            header = bytearray(64)
            header[:6] = b'\x7fELF\x02\x01'
            header[16:18], header[18:20] = (3).to_bytes(2, 'little'), (62).to_bytes(2, 'little')
            for binary in (staged, installed):
                binary.parent.mkdir(parents=True)
                binary.write_bytes(header)
            dependency = prefix / 'lib/libAeroQt.so.0'
            dependency.parent.mkdir()
            dependency.write_bytes(b'verified external runtime fixture')
            descriptor = {'build_id': 'fixture-build', 'commit': 'a' * 40,
                          'source_manifest_sha256': 'b' * 64,
                          'target': {'prefix': str(prefix), 'external_dependencies': [
                              {'path': str(dependency), 'sha256': update.file_digest(dependency)}]}}
            output = (f'Build: {descriptor["build_id"]}\nSource revision: {descriptor["commit"]}\n'
                      f'Source manifest SHA-256: {descriptor["source_manifest_sha256"]}\n').encode()
            with mock.patch.dict(os.environ, LD_LIBRARY_PATH='/untrusted/build/lib', LD_PRELOAD='/untrusted/hook.so'):
                with mock.patch.object(update, 'bounded_run', return_value=output) as execute:
                    update.headless_probe(bundle, descriptor)
                    args, kwargs = execute.call_args
                    self.assertEqual(args[0], [str(staged), '--version'])
                    self.assertEqual(kwargs['env']['LD_LIBRARY_PATH'], str(prefix / 'lib'))
                    self.assertNotIn('LD_PRELOAD', kwargs['env'])
                    self.assertEqual(kwargs['timeout'], 10)
                with mock.patch.object(update, 'bounded_run', return_value=output) as execute:
                    update.headless_probe(bundle, descriptor, prefix=str(prefix))
                    args, kwargs = execute.call_args
                    self.assertEqual(args[0], [str(installed), '--version'])
                    self.assertNotIn('LD_LIBRARY_PATH', kwargs['env'])
                    self.assertNotIn('LD_PRELOAD', kwargs['env'])
            dependency.write_bytes(b'drift after preparation')
            with mock.patch.object(update, 'bounded_run') as execute:
                with self.assertRaisesRegex(update.Refusal, 'changed before staged'):
                    update.headless_probe(bundle, descriptor)
                execute.assert_not_called()


class ArchiveTests(unittest.TestCase):
    def extract(self, info, second=None):
        with tempfile.TemporaryDirectory(prefix='update-archive-fixture-') as temporary:
            root = Path(temporary)
            path = root / 'archive.tar.gz'
            with tarfile.open(path, 'w:gz', format=tarfile.USTAR_FORMAT) as archive:
                archive.addfile(info, io.BytesIO(b'x' * info.size) if info.isfile() else None)
                if second:
                    archive.addfile(second, io.BytesIO(b'x' * second.size) if second.isfile() else None)
            update.safe_extract(path, root / 'bundle')

    def test_traversal_absolute_aliases_and_unexpected_layout(self):
        for name in ('../escape', '/etc/passwd', './payload/x', 'payload//x', 'payload/../x',
                     'hooks/install.sh', 'source/extra', 'payload\\evil'):
            with self.subTest(name=name), self.assertRaises(update.Refusal):
                self.extract(tarfile.TarInfo(name))

    def test_links_special_duplicates_privileged_modes_and_limits(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.FIFOTYPE):
            info = tarfile.TarInfo('payload/x')
            info.type = kind
            info.linkname = '/tmp/unsafe'
            with self.subTest(kind=kind), self.assertRaises(update.Refusal):
                self.extract(info)
        info = tarfile.TarInfo('payload/x')
        with self.assertRaisesRegex(update.Refusal, 'Duplicate'):
            self.extract(info, info)
        info.mode = 0o4755
        with self.assertRaisesRegex(update.Refusal, 'privileged'):
            self.extract(info)
        info.mode, info.size = 0o644, 10
        with mock.patch.object(update, 'MAX_EXPANDED', 5), self.assertRaisesRegex(update.Refusal, 'exceeds'):
            self.extract(info)

    def test_extended_metadata_and_oversized_headers_refuse_without_reading_payload(self):
        for kind in (tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.GNUTYPE_LONGNAME, tarfile.REGTYPE):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory(prefix='update-header-fixture-') as temporary:
                root = Path(temporary)
                info = tarfile.TarInfo('payload/x')
                info.type, info.size = kind, update.MAX_EXPANDED + 1
                archive = root / 'archive.tar.gz'
                archive.write_bytes(gzip.compress(info.tobuf(format=tarfile.USTAR_FORMAT) + bytes(1024)))
                with self.assertRaises(update.Refusal):
                    update.safe_extract(archive, root / 'out')

    def test_json_duplicate_keys_size_and_nonfinite(self):
        for data in (b'{"schema":1,"schema":1}', b'{"number":NaN}', b'x' * (update.MAX_JSON + 1)):
            with self.assertRaises(update.Refusal):
                update.strict_json(data)

    def test_subprocess_output_and_timeout_bounds(self):
        with self.assertRaisesRegex(update.Refusal, 'output limit'):
            update.bounded_run(['/usr/bin/python3', '-c', 'print("x"*1000)'], limit=100)
        with self.assertRaisesRegex(update.Refusal, 'timed out'):
            update.bounded_run(['/usr/bin/python3', '-c', 'import time; time.sleep(2)'], timeout=0.05)

    def test_cli_errors_are_one_bounded_json_object_without_stderr(self):
        result = subprocess.run(['/usr/bin/python3', str(TOOLS / 'control_panel_update.py'), 'invalid'],
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b'')
        self.assertLess(len(result.stdout), update.MAX_JSON)
        self.assertEqual(json.loads(result.stdout)['status'], 'error')


if __name__ == '__main__':
    unittest.main()
