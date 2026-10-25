"""Private updater bootstrap fixtures. No GitHub access or actual user prefix."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import bootstrap_manual_updater as bootstrap
import control_panel_update as update
import install_lock
import rc_bundle as rc


class BootstrapFixtures(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='control-bootstrap-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / 'home'
        self.home.mkdir(mode=0o700)
        self.verifier = self.root / 'fixture-gh'
        self.verifier.write_bytes(b'\x7fELF' + b'fixture verifier bytes never executed')
        self.verifier.chmod(0o500)

    def test_private_complete_runtime_and_configuration_without_app_ownership(self):
        result = bootstrap.bootstrap(self.home, self.verifier)
        runtime = Path(result['runtime'])
        self.assertEqual(runtime.stat().st_mode & 0o777, 0o700)
        for name in ('control_panel_update.py', 'rc_bundle.py', 'install_lock.py'):
            self.assertEqual((runtime / name).stat().st_mode & 0o777, 0o600)
        self.assertEqual((runtime / 'gh').stat().st_mode & 0o777, 0o500)
        client = update.Consumer(home=self.home)
        config = client.configure()
        self.assertEqual(config['repository_id'], 1421857804)
        self.assertEqual(config['gh'], str(runtime / 'gh'))
        self.assertFalse((self.home / '.local').exists())
        self.assertFalse((self.home / '.control-panel-rc-deployment/state.json').exists())
        before = rc.fingerprint(runtime / 'gh')
        bootstrap.bootstrap(self.home, self.verifier)
        self.assertEqual(before, rc.fingerprint(runtime / 'gh'))

    def test_intervening_runtime_edit_rejected_before_other_runtime_changes(self):
        result = bootstrap.bootstrap(self.home, self.verifier)
        runtime = Path(result['runtime'])
        target = runtime / 'control_panel_update.py'
        target.write_bytes(b'user runtime edit')
        other = rc.fingerprint(runtime / 'rc_bundle.py')
        with self.assertRaisesRegex(rc.Refusal, 'differs'):
            bootstrap.bootstrap(self.home, self.verifier)
        self.assertEqual(target.read_bytes(), b'user runtime edit')
        self.assertEqual(other, rc.fingerprint(runtime / 'rc_bundle.py'))

    def test_symlink_or_nonprivate_runtime_refused(self):
        state = self.home / '.control-panel-updater'
        state.symlink_to(self.root)
        with self.assertRaises(rc.Refusal):
            bootstrap.bootstrap(self.home, self.verifier)
        state.unlink()
        state.mkdir(mode=0o755)
        with self.assertRaisesRegex(rc.Refusal, '0700'):
            bootstrap.bootstrap(self.home, self.verifier)

    def test_bootstrap_waits_for_app_and_active_manual_operation(self):
        with install_lock.installation_lock(self.home / '.local', exclusive=False):
            with self.assertRaises(install_lock.Refusal):
                bootstrap.bootstrap(self.home, self.verifier)
        with update.Consumer(home=self.home).state():
            with self.assertRaises(update.Refusal):
                bootstrap.bootstrap(self.home, self.verifier)

    def test_unsafe_verifier_is_not_blessed_by_bootstrap(self):
        self.verifier.chmod(0o777)
        with self.assertRaisesRegex(rc.Refusal, 'writable'):
            bootstrap.bootstrap(self.home, self.verifier)
        self.verifier.chmod(0o500)
        directory = self.root / 'writable-source';directory.mkdir(mode=0o777);directory.chmod(0o777)
        verifier = directory / 'gh';verifier.write_bytes(self.verifier.read_bytes());verifier.chmod(0o500)
        with self.assertRaisesRegex(rc.Refusal, 'reviewed'):
            bootstrap.bootstrap(self.home, verifier)
        with self.assertRaisesRegex(rc.Refusal, 'differ'):
            bootstrap.bootstrap(self.home, verifier, reviewed_sha256='0' * 64)
        bootstrap.bootstrap(self.home, verifier, reviewed_sha256=rc.sha(verifier.read_bytes()))


if __name__ == '__main__':
    unittest.main()
