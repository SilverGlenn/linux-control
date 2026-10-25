#!/usr/bin/env python3
"""Private release-driver fixtures, including a real miniature frozen-source build."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import build_local_release as driver
import rc_bundle as rc


class LocalReleaseFixtures(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='control-local-release-fixture-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / 'private-home'
        self.home.mkdir()
        patch = mock.patch.dict(os.environ, {'HOME': str(self.home), 'XDG_CONFIG_HOME': str(self.home / 'config'),
            'XDG_DATA_HOME': str(self.home / 'data'), 'XDG_CACHE_HOME': str(self.home / 'cache'),
            'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/nonexistent/local-release-fixture',
            'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent/local-release-fixture',
            'GIT_CONFIG_GLOBAL': '/dev/null', 'GIT_CONFIG_NOSYSTEM': '1', 'PYTHONDONTWRITEBYTECODE': '1'})
        patch.start()
        self.addCleanup(patch.stop)
        self.checkout = self.root / 'checkout'
        self.checkout.mkdir()
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', 'Private Fixture')
        self.git('config', 'user.email', 'fixture@invalid.example')
        (self.checkout / '.gitignore').write_text('build/\n')
        (self.checkout / 'input.txt').write_text('frozen input\n')
        self.git('add', '.')
        self.git('commit', '-m', 'Fixture baseline')
        self.config = self.root / 'config.json'
        self.definition = {'schema': 1, 'cmake_definitions': {}, 'development_inputs': [], 'runtime_dependencies': []}
        self.write_config()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.checkout), *args], stderr=subprocess.DEVNULL)

    def write_config(self):
        self.config.write_bytes(rc.encoded(self.definition))

    def freeze(self):
        source = self.root / 'source'
        with contextlib.redirect_stdout(io.StringIO()):
            rc.source(argparse.Namespace(root=self.checkout, output=source, version='fixture-1'))
        return source

    def arguments(self):
        return argparse.Namespace(root=self.checkout, output=self.root / 'result', config=self.config,
                                  version='fixture-1', prefix='/home/fixture/.local', jobs=2, sequence=None,
                                  rehearsal=False)

    def test_directory_pin_preserves_link_identity_without_following_directory_links(self):
        tree = self.root / 'development'
        tree.mkdir()
        (tree / 'header.h').write_text('header\n')
        (tree / 'internal-link').symlink_to('.', target_is_directory=True)
        before = driver.input_image(tree)
        (self.home / 'unrelated').write_text('not a pinned tree member')
        self.assertEqual(driver.input_image(tree), before)
        (tree / 'header.h').chmod(0o700)
        self.assertNotEqual(driver.input_image(tree)['sha256'], before['sha256'])
        (tree / 'outside-link').symlink_to(self.home, target_is_directory=True)
        with self.assertRaisesRegex(rc.Refusal, 'Escaping'):
            driver.input_image(tree)

    def test_config_rejects_general_cmake_overrides_missing_pins_and_drift(self):
        dependency = self.root / 'dev'
        dependency.mkdir()
        (dependency / 'config.cmake').write_text('input')
        pin = {'path': str(dependency), 'sha256': driver.input_image(dependency)['sha256']}
        self.definition['cmake_definitions'] = {'CMAKE_PREFIX_PATH': str(dependency)}
        self.write_config()
        with self.assertRaisesRegex(rc.Refusal, 'Unpinned'):
            driver.configuration(self.config)
        self.definition['development_inputs'] = [pin]
        self.write_config()
        driver.configuration(self.config)
        self.definition['cmake_definitions']['BUILD_TESTING'] = 'OFF'
        self.write_config()
        with self.assertRaisesRegex(rc.Refusal, 'Only dependency'):
            driver.configuration(self.config)
        del self.definition['cmake_definitions']['BUILD_TESTING']
        (dependency / 'config.cmake').write_text('drift')
        self.write_config()
        with self.assertRaisesRegex(rc.Refusal, 'Input pin differs'):
            driver.configuration(self.config)

    def test_safe_extraction_matches_bytes_modes_and_internal_links(self):
        (self.checkout / 'safe-link').symlink_to('input.txt')
        (self.checkout / 'input.txt').chmod(0o750)
        source = self.freeze()
        destination = self.root / 'extracted'
        identity, entries = driver.extract_source(source, destination)
        self.assertEqual((destination / 'safe-link').read_text(), 'frozen input\n')
        self.assertEqual((destination / 'input.txt').stat().st_mode & 0o777, 0o750)
        self.assertEqual(identity, rc.read_json(source / 'identity.json'))
        driver.verify_tree(destination, entries)
        (destination / 'input.txt').write_text('mutation')
        with self.assertRaisesRegex(rc.Refusal, 'changed'):
            driver.verify_tree(destination, entries)

    def test_escape_link_refused_before_extraction(self):
        (self.checkout / 'unsafe-link').symlink_to('../outside')
        source = self.freeze()
        destination = self.root / 'extracted'
        with self.assertRaisesRegex(rc.Refusal, 'Escaping'):
            driver.extract_source(source, destination)
        self.assertFalse(destination.exists())

    def test_archive_duplicate_or_changed_metadata_refused(self):
        source = self.freeze()
        archive = source / 'source.tar.gz'
        original = []
        with tarfile.open(archive, 'r:gz') as stream:
            for member in stream:
                original.append((member, stream.extractfile(member).read()))
        with tarfile.open(archive, 'w:gz') as stream:
            for member, data in original:
                stream.addfile(member, io.BytesIO(data))
            stream.addfile(original[0][0], io.BytesIO(original[0][1]))
        identity = rc.read_json(source / 'identity.json')
        identity['source_archive_sha256'] = rc.fingerprint(archive)['sha256']
        (source / 'identity.json').write_bytes(rc.encoded(identity))
        with self.assertRaisesRegex(rc.Refusal, 'duplicate'):
            driver.extract_source(source, self.root / 'extracted')

    def test_complete_ctest_requires_exact_names_and_no_disabled_or_skipped_suites(self):
        discovery = self.root / 'discovery.json'
        discovery.write_text(json.dumps({'tests': [{'name': 'one'}, {'name': 'two'}]}))
        junit = self.root / 'results.xml'
        good = '<testsuite tests="2" failures="0"><testcase name="one" status="run"/><testcase name="two" status="run"/></testsuite>'
        junit.write_text(good)
        self.assertEqual(driver.fixture_result(discovery, junit)['passed'], 2)
        for change in [good.replace('name="two"', 'name="another"'), good.replace('status="run"', 'status="notrun"', 1),
                       good.replace('failures="0"', 'skipped="1"'), good.replace('/></testsuite>', '><skipped/></testcase></testsuite>')]:
            junit.write_text(change)
            with self.assertRaises(rc.Refusal):
                driver.fixture_result(discovery, junit)
        junit.write_text(good)
        discovery.write_text(json.dumps({'tests': [{'name': 'one', 'properties': [{'name': 'DISABLED', 'value': True}]}, {'name': 'two'}]}))
        with self.assertRaisesRegex(rc.Refusal, 'Disabled'):
            driver.fixture_result(discovery, junit)

    def test_existing_or_nonignored_output_and_high_parallelism_refused(self):
        args = self.arguments()
        args.output.mkdir()
        with self.assertRaisesRegex(rc.Refusal, 'must be new'):
            driver.build(args)
        args.output = self.checkout / 'not-ignored'
        with self.assertRaisesRegex(rc.Refusal, 'ignored'):
            driver.build(args)
        args.output = self.root / 'new'
        args.jobs = 3
        with self.assertRaisesRegex(rc.Refusal, 'parallelism'):
            driver.build(args)
        self.assertFalse(args.output.exists())

    def test_runner_cleans_environment_and_preserves_failed_command_logs(self):
        output = self.root / 'runner'
        output.mkdir()
        runner = driver.Runner(output)
        probe = 'import os,sys; assert "DISPLAY" not in os.environ; assert "LD_PRELOAD" not in os.environ; print(os.environ["HOME"]); sys.exit(7)'
        with mock.patch.dict(os.environ, {'DISPLAY': ':0', 'LD_PRELOAD': '/untrusted.so'}), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(rc.Refusal, 'failed'):
                runner.run('failure', [sys.executable, '-c', probe])
        self.assertIn(str(output / 'private-environment/home'), (output / 'failure.log').read_text())
        self.assertEqual(rc.read_json(output / 'commands.json')[0]['returncode'], 7)

    def test_default_refuses_dirty_or_non_main_checkout_and_rehearsal_cannot_package(self):
        args = self.arguments()
        (self.checkout / 'input.txt').write_text('dirty')
        with self.assertRaisesRegex(rc.Refusal, 'clean'):
            driver.build(args)
        (self.checkout / 'input.txt').write_text('frozen input\n')
        self.git('checkout', '-b', 'development')
        with self.assertRaisesRegex(rc.Refusal, 'main branch'):
            driver.build(args)
        args.rehearsal, args.sequence = True, 1
        with self.assertRaisesRegex(rc.Refusal, 'Rehearsal cannot'):
            driver.build(args)
        self.assertFalse(args.output.exists())

    def miniature_project(self):
        tools = self.checkout / 'tools'
        tools.mkdir()
        source_tools = Path(__file__).resolve().parents[1] / 'tools'
        for name in ('build_local_release.py', 'rc_bundle.py', 'install_lock.py', 'control_panel_update.py',
                     'run_isolated_tests.py', 'make_update_release.py', 'validate_release_payload.py'):
            shutil.copyfile(source_tools / name, tools / name)
        (self.checkout / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(PrivateFixture C)
include(CTest)
configure_file(identity.h.in identity.h @ONLY)
add_executable(control main.c)
target_include_directories(control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
install(TARGETS control DESTINATION bin)
configure_file(desktop.in fixture.desktop @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fixture.desktop" DESTINATION share/applications RENAME linux-controlpanel.desktop)
add_test(NAME isolated_fixture COMMAND /usr/bin/python3 "${CMAKE_CURRENT_SOURCE_DIR}/isolation.py")
''')
        (self.checkout / 'identity.h.in').write_text('''#define BUILD "@CONTROL_BUILD_ID@"
#define REVISION "@CONTROL_SOURCE_REVISION@"
#define SOURCE "@CONTROL_SOURCE_MANIFEST_SHA256@"
''')
        (self.checkout / 'main.c').write_text('''#include <stdio.h>
#include <string.h>
#include "identity.h"
int main(int argc, char **argv) {
 if (argc != 2 || strcmp(argv[1], "--version")) return 2;
 puts("Build: " BUILD); puts("Source revision: " REVISION);
 puts("Source manifest SHA-256: " SOURCE); return 0;
}
''')
        (self.checkout / 'desktop.in').write_text('[Desktop Entry]\nExec=@CMAKE_INSTALL_PREFIX@/bin/control\n')
        (self.checkout / 'isolation.py').write_text('''import os
assert "DISPLAY" not in os.environ
assert "WAYLAND_DISPLAY" not in os.environ
assert os.environ["DBUS_SYSTEM_BUS_ADDRESS"] == os.environ["DBUS_SESSION_BUS_ADDRESS"]
assert "test-environment" in os.environ["HOME"]
''')
        self.git('add', '.')
        self.git('commit', '-m', 'Miniature build fixture')

    def test_real_frozen_source_build_complete_suite_staging_and_optional_packaging(self):
        self.miniature_project()
        args = self.arguments()
        args.sequence = 9
        with contextlib.redirect_stdout(io.StringIO()):
            report = driver.build(args)
        self.assertEqual(report['status'], 'passed')
        validation = rc.read_json(args.output / 'validation.json')
        self.assertEqual(validation['complete_fixture_suite'], {'total': 1, 'passed': 1, 'failed': 0, 'not_run': 0})
        self.assertEqual(validation['qualification'], 'candidate for acceptance')
        self.assertTrue(validation['staged_deployment_passed'])
        self.assertTrue((args.output / 'release/control-panel-bundle.tar.gz').is_file())
        cache = (args.output / 'CMakeCache.txt').read_text()
        self.assertIn(str(args.output / 'frozen-source'), cache)
        self.assertFalse((self.home / '.local').exists())
        commands = rc.read_json(args.output / 'commands.json')
        self.assertTrue(any(item['phase'] == 'staged-validation' for item in commands))
        self.assertTrue(all(item['returncode'] == 0 for item in commands))

    def test_failed_full_suite_never_emits_validation_or_bundle(self):
        self.miniature_project()
        (self.checkout / 'isolation.py').write_text('raise SystemExit(4)\n')
        args = self.arguments()
        args.rehearsal = True
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(rc.Refusal, 'ctest failed'):
                driver.build(args)
        self.assertEqual(rc.read_json(args.output / 'build-report.json')['status'], 'failed')
        self.assertFalse((args.output / 'validation.json').exists())
        self.assertFalse((args.output / 'bundle').exists())
        self.assertFalse((self.home / '.local').exists())

    def test_successful_rehearsal_cannot_be_mistaken_for_release_validation(self):
        self.miniature_project()
        (self.checkout / 'input.txt').write_text('explicit development draft\n')
        args = self.arguments()
        args.rehearsal = True
        with contextlib.redirect_stdout(io.StringIO()):
            report = driver.build(args)
        self.assertEqual(report['mode'], 'rehearsal')
        self.assertEqual(report['qualification'], 'development rehearsal')
        self.assertTrue(report['initial_checkout_status'])
        self.assertFalse((args.output / 'validation.json').exists())
        self.assertFalse((args.output / 'release').exists())
        self.assertEqual(rc.read_json(args.output / 'rehearsal-validation.json')['qualification'], 'development rehearsal')

    def test_dependency_drift_during_staged_validation_blocks_final_evidence(self):
        self.miniature_project()
        dependency = self.root / 'pinned-header.h'
        dependency.write_text('original')
        self.definition['development_inputs'] = [{'path': str(dependency), 'sha256': driver.input_image(dependency)['sha256']}]
        self.write_config()
        original_run = driver.Runner.run

        def run_and_drift(runner, name, *args, **kwargs):
            result = original_run(runner, name, *args, **kwargs)
            if name == 'staged-validation':
                dependency.write_text('changed during probe')
            return result

        args = self.arguments()
        with mock.patch.object(driver.Runner, 'run', run_and_drift), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(rc.Refusal, 'Input pin differs'):
                driver.build(args)
        self.assertFalse((args.output / 'validation.json').exists())
        self.assertEqual(rc.read_json(args.output / 'build-report.json')['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
