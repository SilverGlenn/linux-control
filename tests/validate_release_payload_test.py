#!/usr/bin/env python3
"""Actual compiled ELF/private deployment regressions for staged validation."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import rc_bundle as rc
import validate_release_payload as validate


class StagedValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc') or not Path('/usr/bin/readelf').exists():
            raise RuntimeError('C compiler and readelf are required; staging qualification cannot skip')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='control-stage-validator-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        isolated = mock.patch.dict(os.environ, validate.private_environment(self.root / 'fixture-environment'), clear=True)
        isolated.start()
        self.addCleanup(isolated.stop)
        self.bundle = self.root / 'bundle'
        self.prefix = '/home/private-validator/.local'
        self.source = {'version': 'fixture', 'build_id': 'fixture+abc', 'revision': '1' * 40,
                       'source_manifest_sha256': 'a' * 64}
        self.make_bundle(self.bundle)

    def make_bundle(self, directory, wrong_identity=False, rpath=None, shared=False):
        payload = directory / 'payload'
        control = payload / 'bin/control'
        control.parent.mkdir(parents=True)
        source = self.root / (directory.name + '.c')
        text = ('Build: ' + ('wrong' if wrong_identity else self.source['build_id']) + '\n'
                'Source revision: ' + self.source['revision'] + '\n'
                'Source manifest SHA-256: ' + self.source['source_manifest_sha256'] + '\n')
        function = 'int fixture_external(void);' if shared else ''
        action = 'if(fixture_external()!=7)return 2;' if shared else ''
        source.write_text('#include <stdio.h>\n' + function + '\nint main(void){' + action +
                          'puts(' + json.dumps(text) + ');return 0;}\n')
        command = ['cc', str(source), '-o', str(control)]
        external = None
        if shared:
            library_source = self.root / 'external.c'
            library_source.write_text('int fixture_external(void){return 7;}\n')
            external = self.root / 'external/libfixture.so.1'
            external.parent.mkdir(exist_ok=True)
            subprocess.run(['cc', '-shared', '-fPIC', str(library_source), '-Wl,-soname,libfixture.so.1',
                            '-o', str(external)], check=True, capture_output=True)
            command += [str(external), '-Wl,-rpath,$ORIGIN/../lib']
        if rpath:
            command.append('-Wl,-rpath,' + str(rpath))
        subprocess.run(command, check=True, capture_output=True)
        rc.write_atomic(payload / 'share/applications/linux-controlpanel.desktop',
                        ('[Desktop Entry]\nExec=' + self.prefix + '/bin/control\n').encode(), 0o644)
        environment = {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}
        if shared:
            environment['LD_LIBRARY_PATH'] = str(external.parent)
        loader = subprocess.check_output(['/usr/bin/ldd', str(control)], env=environment, text=True)
        dependencies = []
        for _, path in validate.parse_loader(loader):
            resolved = path.resolve()
            dependencies.append({'path': str(path), 'resolved_path': str(resolved),
                                 'image': rc.fingerprint(resolved), 'ownership': 'external'})
        manifest = {'schema': 1, 'prefix': self.prefix, 'source': self.source,
                    'entries': rc.inventory(payload, bounded=True), 'external_dependencies': dependencies}
        self.write_manifest(directory, manifest)
        return external

    def write_manifest(self, directory, manifest):
        data = rc.encoded(manifest)
        rc.write_atomic(directory / 'payload-manifest.json', data)
        rc.write_atomic(directory / 'payload-manifest.sha256', (rc.sha(data) + '\n').encode())

    def run_validator(self, bundle=None, extra=(), success=True):
        output = self.root / ('evidence-' + str(len(list(self.root.glob('evidence-*')))))
        args = ['--bundle', str(bundle or self.bundle), '--output', str(output), *map(str, extra)]
        with contextlib.redirect_stderr(io.StringIO()):
            result = validate.main(args)
        report = json.loads((output / 'evidence.json').read_text())
        self.assertEqual(result, 0 if success else 1, report)
        self.assertEqual(report['status'], 'passed' if success else 'failed')
        return report, output

    def test_actual_payload_transactions_runtime_and_original_inputs(self):
        before = rc.inventory(self.bundle)
        report, output = self.run_validator()
        self.assertTrue(report['staged_deployment_passed'])
        self.assertTrue(report['runtime_check_passed'])
        self.assertEqual(report['predecessor']['kind'], 'synthetic metadata fixture')
        self.assertIn('one boundary', report['interruption']['scope'])
        self.assertGreater(report['runtime']['dependency_count'], 0)
        for dependency in report['runtime']['resolved_dependencies']:
            self.assertEqual(len(dependency['image']['sha256']), 64)
        self.assertEqual(rc.inventory(self.bundle), before)
        events = json.loads((output / 'transactions.json').read_text())
        self.assertEqual(len([e for e in events if 'expected_refusal' in e]), 3)
        self.assertTrue((output / 'staged-version.log').exists())

    def test_distinct_previous_bundle_and_candidate_only_files(self):
        previous = self.root / 'previous'
        self.make_bundle(previous)
        marker = previous / 'payload/share/linux-control/previous-only.txt'
        rc.write_atomic(marker, b'previous release marker', 0o640)
        old = rc.read_json(previous / 'payload-manifest.json')
        old['entries'] = rc.inventory(previous / 'payload', bounded=True)
        self.write_manifest(previous, old)
        report, _ = self.run_validator(extra=['--previous-bundle', previous])
        self.assertEqual(report['predecessor']['kind'], 'provided release bundle')
        self.assertIn(report['interruption']['after_changed_path'],
                      {'bin/control', 'share/linux-control/previous-only.txt'})

    def test_identical_previous_bundle_is_not_bytes_changing_upgrade(self):
        report, _ = self.run_validator(extra=['--previous-bundle', self.bundle], success=False)
        self.assertIn('differing payload', report['error'])

    def test_identity_mismatch_fails_and_retains_logs(self):
        wrong = self.root / 'wrong'
        self.make_bundle(wrong, wrong_identity=True)
        report, output = self.run_validator(wrong, success=False)
        self.assertIn('identity mismatch', report['error'])
        self.assertFalse(report['runtime_check_passed'])
        self.assertTrue((output / 'staged-version.log').exists())

    def test_loader_inventory_must_be_full_and_pinned(self):
        manifest = rc.read_json(self.bundle / 'payload-manifest.json')
        manifest['external_dependencies'] = manifest['external_dependencies'][:1]
        self.write_manifest(self.bundle, manifest)
        report, _ = self.run_validator(success=False)
        self.assertIn('missing from pinned bundle', report['error'])

    def test_pinned_library_drift_refuses_before_runtime(self):
        manifest = rc.read_json(self.bundle / 'payload-manifest.json')
        manifest['external_dependencies'][0]['image']['sha256'] = 'f' * 64
        self.write_manifest(self.bundle, manifest)
        report, _ = self.run_validator(success=False)
        self.assertIn('dependency drift', report['error'])

    def test_unapproved_copy_never_becomes_fixture_runtime(self):
        library = self.root / 'libunapproved.so.1'
        library.write_bytes(b'unapproved')
        report, _ = self.run_validator(extra=['--runtime-dependency', library], success=False)
        self.assertIn('not pinned', report['error'])

    def test_explicit_pinned_external_library_is_separate_and_preserved(self):
        candidate = self.root / 'shared'
        library = self.make_bundle(candidate, shared=True)
        original = rc.fingerprint(library)
        report, _ = self.run_validator(candidate, extra=['--runtime-dependency', library,
                                                        '--forbid-root', self.root])
        self.assertEqual(rc.fingerprint(library), original)
        self.assertEqual(report['runtime_copies'][0]['image'], original)
        self.assertTrue(any(d['external_origin'] == str(library)
                            for d in report['runtime']['resolved_dependencies']))
        self.assertNotIn('lib/libfixture.so.1', rc.load_bundle(candidate)[1]['entries'])

    def test_development_runpath_fails_even_when_no_dependency_uses_it(self):
        candidate = self.root / 'runpath'
        self.make_bundle(candidate, rpath=self.root / 'development')
        report, _ = self.run_validator(candidate, success=False)
        self.assertIn('runtime search path refused', report['error'])

    def test_forbidden_system_resolution_is_independently_rejected(self):
        report, _ = self.run_validator(extra=['--forbid-root', '/usr/lib'], success=False)
        self.assertIn('checkout/development root', report['error'])

    def test_loader_environment_is_not_inherited(self):
        with mock.patch.dict(os.environ, {'LD_PRELOAD': '/no/such/injection.so',
                                          'LD_LIBRARY_PATH': '/no/such/development',
                                          'QT_PLUGIN_PATH': '/real/desktop/plugins',
                                          'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/real/system'}):
            report, _ = self.run_validator()
        self.assertTrue(report['runtime_check_passed'])

    def test_payload_tampering_refuses_and_records_failure(self):
        (self.bundle / 'payload/bin/control').write_bytes(b'tampered')
        report, _ = self.run_validator(success=False)
        self.assertIn('differs from its manifest', report['error'])
        self.assertFalse(report['staged_deployment_passed'])

    def test_installer_noop_cannot_produce_passing_evidence(self):
        with mock.patch.object(rc, 'install', return_value=None):
            report, _ = self.run_validator(success=False)
        self.assertIn('fingerprint mismatch', report['error'])

    def test_failed_loader_subprocess_propagates_failure(self):
        actual = subprocess.run
        def fail_loader(command, **kwargs):
            if command[0] == '/usr/bin/ldd':
                return subprocess.CompletedProcess(command, 23, 'partial output', 'fixture failure')
            return actual(command, **kwargs)
        with mock.patch.object(subprocess, 'run', side_effect=fail_loader):
            report, output = self.run_validator(success=False)
        self.assertIn('exit 23', report['error'])
        self.assertIn('fixture failure', (output / 'loader-bin-control.log').read_text())

    def test_existing_output_is_not_overwritten(self):
        output = self.root / 'existing'
        output.mkdir()
        marker = output / 'keep'
        marker.write_text('preserve')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(validate.main(['--bundle', str(self.bundle), '--output', str(output)]), 1)
        self.assertEqual(marker.read_text(), 'preserve')
        self.assertFalse((output / 'evidence.json').exists())

    def test_missing_loader_lines_do_not_silently_skip_inventory(self):
        for output in ('', 'libfixture.so.1 => not found', 'unexpected arbitrary output'):
            with self.assertRaises(rc.Refusal):
                validate.parse_loader(output)


if __name__ == '__main__':
    unittest.main()
