#!/usr/bin/env python3
"""Private packaging fixtures; recorded validation is synthetic, never hardware evidence."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_update_release as producer
import rc_bundle as rc
import control_panel_update as consumer


class MakeReleaseFixtures(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='control-release-fixture-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / 'home'
        self.home.mkdir()
        environment = mock.patch.dict(os.environ, {'HOME': str(self.home), 'XDG_CONFIG_HOME': str(self.home / 'config'),
            'XDG_DATA_HOME': str(self.home / 'data'), 'XDG_CACHE_HOME': str(self.home / 'cache'),
            'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/release-fixture',
            'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/release-fixture',
            'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_CONFIG_NOSYSTEM': '1'})
        environment.start()
        self.addCleanup(environment.stop)
        self.checkout = self.root / 'checkout'
        self.checkout.mkdir()
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', 'Private Fixture')
        self.git('config', 'user.email', 'fixture@invalid.example')
        (self.checkout / 'main.cpp').write_text('fixture source\n')
        (self.checkout / 'CMakeLists.txt').write_text('project(Fixture)\n')
        self.git('add', '.')
        self.git('commit', '-m', 'Fixture source')
        self.source = self.root / 'source'
        self.quiet(rc.source, root=self.checkout, output=self.source, version='0.1.0-rc.2')
        self.prefix = '/home/fixture/.local'
        stage = self.root / 'stage'
        rc.write_atomic(stage / 'bin/control', b'Fixture candidate; never executed', 0o755)
        rc.write_atomic(stage / 'share/applications/linux-controlpanel.desktop',
                        f'[Desktop Entry]\nExec={self.prefix}/bin/control\n'.encode(), 0o644)
        dependency = self.root / 'external.so'
        dependency.write_bytes(b'External dependency fixture')
        self.bundle = self.root / 'bundle'
        self.quiet(rc.bundle, stage=stage, prefix=self.prefix, source=self.source,
                   output=self.bundle, dependency=[str(dependency)])
        self.manifest = json.loads((self.bundle / 'payload-manifest.json').read_bytes())
        self.validation = self.root / 'recorded-validation.json'
        self.evidence = {'schema': 1, 'source_manifest_sha256': self.manifest['source']['source_manifest_sha256'],
            'payload_manifest_sha256': consumer.file_digest(self.bundle / 'payload-manifest.json'),
            'complete_fixture_suite': {'total': 4, 'passed': 4, 'failed': 0, 'not_run': 0},
            'staged_deployment_passed': True, 'runtime_check_passed': True,
            'qualification': 'candidate for acceptance'}
        self.write_evidence()
        self.output = self.root / 'release'

    def git(self, *arguments):
        return subprocess.check_output(['git', '-C', str(self.checkout), *arguments], stderr=subprocess.DEVNULL)

    def quiet(self, function, **arguments):
        with contextlib.redirect_stdout(io.StringIO()):
            return function(argparse.Namespace(**arguments))

    def write_evidence(self):
        self.validation.write_bytes(rc.encoded(self.evidence))

    def package(self, output=None, sequence=2):
        return producer.package(argparse.Namespace(bundle=self.bundle, checkout=self.checkout, sequence=sequence,
                                                  output=output or self.output, validation=self.validation))

    def rewrite_manifest(self):
        data = rc.encoded(self.manifest)
        (self.bundle / 'payload-manifest.json').write_bytes(data)
        (self.bundle / 'payload-manifest.sha256').write_text(rc.sha(data) + '\n')
        self.evidence['payload_manifest_sha256'] = rc.sha(data)
        self.write_evidence()

    def test_consumable_reproducible_assets_without_network_or_runtime(self):
        with mock.patch.object(consumer, 'GitHub', side_effect=AssertionError('Network forbidden')), \
             mock.patch.object(consumer, 'actual_machine', side_effect=AssertionError('Host probe forbidden')), \
             mock.patch.object(consumer, 'headless_probe', side_effect=AssertionError('Payload execution forbidden')):
            descriptor = self.package()
            second = self.root / 'second'
            self.package(second)
        self.assertEqual(set(path.name for path in self.output.iterdir()),
                         {'control-panel-bundle.tar.gz', 'release.json', 'validation.json'})
        for name in ('control-panel-bundle.tar.gz', 'release.json', 'validation.json'):
            self.assertEqual((self.output / name).read_bytes(), (second / name).read_bytes())
        self.assertEqual((self.output / 'validation.json').read_bytes(), self.validation.read_bytes())
        self.assertEqual(descriptor['repository_id'], 1421857804)
        self.assertEqual(descriptor['tag'], 'laptop-candidate-2-' + self.git('rev-parse', 'HEAD').decode().strip()[:12])
        self.assertEqual(descriptor['target']['prefix'], self.prefix)
        extracted = self.root / 'extracted'
        consumer.safe_extract(self.output / 'control-panel-bundle.tar.gz', extracted)
        client = consumer.Consumer(home=self.home, machine=lambda: dict(producer.TARGET))
        client.validate_bundle(extracted, descriptor)
        self.assertFalse((self.home / '.control-panel-updater').exists())

    def test_evidence_gates_and_artifact_binding(self):
        changes = [('failed fixtures', {'complete_fixture_suite': {'total': 4, 'passed': 3, 'failed': 1, 'not_run': 0}}),
                   ('skipped fixtures', {'complete_fixture_suite': {'total': 4, 'passed': 3, 'failed': 0, 'not_run': 1}}),
                   ('empty fixtures', {'complete_fixture_suite': {'total': 0, 'passed': 0, 'failed': 0, 'not_run': 0}}),
                   ('bool count', {'complete_fixture_suite': {'total': True, 'passed': True, 'failed': 0, 'not_run': 0}}),
                   ('deployment false', {'staged_deployment_passed': False}),
                   ('runtime false', {'runtime_check_passed': False}),
                   ('qualified claim', {'qualification': 'qualified'}),
                   ('wrong source', {'source_manifest_sha256': '0' * 64}),
                   ('wrong payload', {'payload_manifest_sha256': '0' * 64})]
        original = dict(self.evidence)
        for label, change in changes:
            with self.subTest(label=label):
                self.evidence = {**original, **change}
                self.write_evidence()
                with self.assertRaises(rc.Refusal):
                    self.package()
                self.assertFalse(self.output.exists())

    def test_dirty_or_untracked_checkout_refused(self):
        (self.checkout / 'main.cpp').write_text('dirty source\n')
        with self.assertRaisesRegex(rc.Refusal, 'clean'):
            self.package()
        (self.checkout / 'main.cpp').write_text('fixture source\n')
        (self.checkout / 'untracked.txt').write_text('untracked\n')
        with self.assertRaisesRegex(rc.Refusal, 'clean'):
            self.package()
        self.assertFalse(self.output.exists())

    def test_assume_unchanged_or_partial_checkout_cannot_hide_dirty_source(self):
        self.git('update-index', '--assume-unchanged', 'main.cpp')
        (self.checkout / 'main.cpp').write_text('hidden dirty source\n')
        with self.assertRaisesRegex(rc.Refusal, 'Assume-unchanged'):
            self.package()
        self.git('update-index', '--no-assume-unchanged', 'main.cpp')
        (self.checkout / 'main.cpp').write_text('fixture source\n')
        subset = self.checkout / 'subdirectory'
        subset.mkdir()
        with self.assertRaisesRegex(rc.Refusal, 'complete repository root'):
            producer.clean_source(subset)

    def test_head_mismatch_refused(self):
        (self.checkout / 'main.cpp').write_text('next source\n')
        self.git('add', '.')
        self.git('commit', '-m', 'Next fixture source')
        with self.assertRaisesRegex(rc.Refusal, 'revision'):
            self.package()

    def test_commit_outside_local_main_refused(self):
        self.git('checkout', '-b', 'unreviewed')
        (self.checkout / 'main.cpp').write_text('unreviewed source\n')
        self.git('add', '.')
        self.git('commit', '-m', 'Unreviewed fixture')
        with self.assertRaisesRegex(rc.Refusal, 'Git validation'):
            self.package()

    def test_spoofed_frozen_inputs_refused_even_with_updated_hashes(self):
        frozen_path = self.bundle / 'source/source-manifest.json'
        frozen = json.loads(frozen_path.read_bytes())
        frozen['entries']['main.cpp']['sha256'] = '0' * 64
        frozen_path.write_bytes(rc.encoded(frozen))
        identity = self.manifest['source']
        identity['source_manifest_sha256'] = consumer.file_digest(frozen_path)
        identity['build_id'] = identity['version'] + '+' + identity['source_manifest_sha256'][:16]
        (self.bundle / 'source/identity.json').write_bytes(rc.encoded(identity))
        self.evidence['source_manifest_sha256'] = identity['source_manifest_sha256']
        self.rewrite_manifest()
        with self.assertRaisesRegex(rc.Refusal, 'inputs differ'):
            self.package()

    def test_source_archive_hash_and_content_refused(self):
        path = self.bundle / 'source/source.tar.gz'
        path.write_bytes(b'not a source archive')
        with self.assertRaisesRegex(rc.Refusal, 'archive hash'):
            self.package()
        self.manifest['source']['source_archive_sha256'] = consumer.file_digest(path)
        (self.bundle / 'source/identity.json').write_bytes(rc.encoded(self.manifest['source']))
        self.rewrite_manifest()
        with self.assertRaises((rc.Refusal, producer.tarfile.TarError)):
            self.package()

    def test_empty_or_duplicate_dependencies_refused(self):
        dependency = self.manifest['external_dependencies'][0]
        for entries in ([], [dependency, dependency]):
            with self.subTest(entries=len(entries)):
                self.manifest['external_dependencies'] = entries
                self.rewrite_manifest()
                with self.assertRaises(rc.Refusal):
                    self.package()

    def test_extra_files_links_and_unsupported_ustar_paths_refused(self):
        extra = self.bundle / 'unexpected'
        extra.write_text('not allowed')
        with self.assertRaisesRegex(rc.Refusal, 'Unexpected'):
            self.package()
        extra.unlink()
        extra.symlink_to(self.validation)
        with self.assertRaisesRegex(rc.Refusal, 'links'):
            self.package()
        extra.unlink()
        name = 'share/linux-control/' + 'x' * 110
        rc.write_atomic(self.bundle / 'payload' / name, b'long basename', 0o644)
        self.manifest['entries'][name] = rc.fingerprint(self.bundle / 'payload' / name)
        self.rewrite_manifest()
        with self.assertRaises(ValueError):
            self.package()
        self.assertFalse(self.output.exists())

    def test_explicitly_ignored_output_at_checkout_root_is_supported(self):
        # Staging must not create a new untracked sibling inside the checkout.
        (self.checkout / '.git/info/exclude').write_text('release-output/\n')
        target = self.checkout / 'release-output'
        self.package(target)
        self.assertTrue((target / 'release.json').is_file())
        self.assertFalse(self.git('status', '--porcelain'))

    def test_existing_output_is_preserved_and_sequence_bounded(self):
        self.output.mkdir()
        keep = self.output / 'keep'
        keep.write_bytes(b'preserve')
        with self.assertRaisesRegex(rc.Refusal, 'new'):
            self.package()
        self.assertEqual(keep.read_bytes(), b'preserve')
        for sequence in (0, -1, True, 10 ** 16):
            with self.subTest(sequence=sequence):
                with self.assertRaisesRegex(rc.Refusal, 'Sequence'):
                    self.package(self.root / 'new', sequence)


if __name__ == '__main__':
    unittest.main()
