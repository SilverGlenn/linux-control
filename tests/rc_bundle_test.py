#!/usr/bin/env python3
"""Private application-file fixtures. Never writes the real install prefix."""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location("rc_bundle", Path(__file__).resolve().parents[1] / "tools/rc_bundle.py")
rc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(rc)
from install_lock import Busy, installation_lock, canonical_state


class BundleFixtures(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="control-rc-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = Path("/home/fixture/.local")
        self.staging = self.root / "private-root"
        self.destination = self.staging / self.prefix.relative_to("/")
        self.state = self.root / "state"

    def quiet(self, operation, args):
        with contextlib.redirect_stdout(io.StringIO()):
            operation(argparse.Namespace(**args))

    def bundle(self, number, extra=False):
        directory = self.root / f"bundle-{number}"
        payload = directory / "payload"
        entries = {}
        files = {"bin/control": f"candidate {number}".encode(),
                 "share/applications/linux-controlpanel.desktop": f"[Desktop Entry]\nExec={self.prefix}/bin/control\n".encode()}
        if extra:
            files["libexec/controlpanel-display-guard"] = b"fixture guard"
        for name, data in files.items():
            rc.write_atomic(payload / name, data, 0o755 if name.startswith(("bin/", "libexec/")) else 0o644)
            entries[name] = rc.fingerprint(payload / name)
        manifest = {"schema": 1, "prefix": str(self.prefix), "source": {"build_id": str(number)},
                    "entries": entries, "external_dependencies": []}
        data = rc.encoded(manifest)
        rc.write_atomic(directory / "payload-manifest.json", data)
        rc.write_atomic(directory / "payload-manifest.sha256", (rc.sha(data) + "\n").encode())
        return directory

    def install(self, bundle):
        self.quiet(rc.install, {"bundle": bundle, "state": self.state, "staging_root": self.staging})

    def operation(self, name):
        self.quiet(rc.rollback_or_remove, {"state": self.state, "command": name})

    @contextlib.contextmanager
    def other_process_lock(self, exclusive=False):
        script = (
            "import sys;sys.path.insert(0,sys.argv[1]);from install_lock import installation_lock\n"
            "with installation_lock(sys.argv[2],exclusive=sys.argv[3]=='yes'):\n"
            " print('ready',flush=True);sys.stdin.read(1)\n"
        )
        process = subprocess.Popen(
            [sys.executable, "-c", script, str(Path(rc.__file__).parent), str(self.destination),
             "yes" if exclusive else "no"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
            env={**os.environ, "HOME": str(self.root / "private-home")})
        try:
            self.assertEqual(process.stdout.readline(), "ready\n")
            yield
        finally:
            output, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, output + error)

    def test_all_mutations_share_destination_lock_even_with_another_journal(self):
        self.install(self.bundle(1))
        control = self.destination / "bin/control"
        with self.other_process_lock():
            for operation in ("rollback", "remove"):
                with self.assertRaisesRegex(Busy, "deferred"):
                    self.operation(operation)
            for state in (self.state, self.root / "different-journal"):
                with self.assertRaisesRegex(Busy, "deferred"):
                    self.quiet(rc.install, {"bundle": self.bundle(f"locked-{state.name}"),
                                           "state": state, "staging_root": self.staging})
            self.assertEqual(control.read_bytes(), b"candidate 1")
        self.install(self.bundle(2))
        self.assertEqual(control.read_bytes(), b"candidate 2")

    def test_recovery_waits_for_destination_lock_and_keeps_pending_when_busy(self):
        bundle = self.bundle(1)
        real_apply = rc.apply_object
        def interrupt(*args, **kwargs):
            real_apply(*args, **kwargs)
            raise OSError("fixture interruption")
        with mock.patch.object(rc, "apply_object", interrupt):
            with self.assertRaises(OSError):
                self.install(bundle)
        with self.other_process_lock():
            with self.assertRaisesRegex(Busy, "deferred"):
                self.quiet(rc.recover, {"state": self.state})
            self.assertTrue((self.state / "pending.json").exists())
        self.quiet(rc.recover, {"state": self.state})
        self.assertFalse((self.state / "pending.json").exists())

    def test_outer_consumer_lock_is_reentrant_and_shared_upgrade_is_refused(self):
        with installation_lock(self.destination):
            self.install(self.bundle(1))
            self.operation("rollback")
        with installation_lock(self.destination, exclusive=False):
            with self.assertRaisesRegex(rc.Refusal, "Cannot upgrade"):
                self.install(self.bundle(2))

    def test_busy_lock_wait_is_bounded_and_inode_remains_after_release(self):
        with self.other_process_lock():
            before = time.monotonic()
            with self.assertRaisesRegex(Busy, "deferred"):
                with installation_lock(self.destination, wait_seconds=0.05):
                    self.fail("Acquired a busy lock")
            self.assertGreaterEqual(time.monotonic() - before, 0.04)
        lock = canonical_state(self.destination) / "application.lock"
        inode = lock.stat().st_ino
        with installation_lock(self.destination):
            self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual(lock.stat().st_ino, inode)

    def test_lock_file_and_directory_trust_are_checked_without_repair(self):
        directory = canonical_state(self.destination)
        with installation_lock(self.destination):
            pass
        lock = directory / "application.lock"
        lock.chmod(0o644)
        with self.assertRaisesRegex(rc.Refusal, "0600") as caught:
            with installation_lock(self.destination):
                pass
        self.assertNotIsInstance(caught.exception, Busy)
        lock.unlink()
        outside = self.root / "outside"
        outside.write_bytes(b"unchanged")
        lock.symlink_to(outside)
        with self.assertRaises(rc.Refusal) as caught:
            with installation_lock(self.destination):
                pass
        self.assertNotIsInstance(caught.exception, Busy)
        self.assertEqual(outside.read_bytes(), b"unchanged")
        lock.unlink()
        outside.chmod(0o600)
        os.link(outside, lock)
        with self.assertRaises(rc.Refusal) as caught:
            with installation_lock(self.destination):
                pass
        self.assertNotIsInstance(caught.exception, Busy)
        lock.unlink()
        directory.chmod(0o755)
        with self.assertRaisesRegex(rc.Refusal, "0700") as caught:
            with installation_lock(self.destination):
                pass
        self.assertNotIsInstance(caught.exception, Busy)
        self.assertEqual(directory.stat().st_mode & 0o777, 0o755)
        directory.rmdir()
        outside_directory = self.root / "outside-directory"
        outside_directory.mkdir(mode=0o700)
        directory.symlink_to(outside_directory, target_is_directory=True)
        with self.assertRaisesRegex(rc.Refusal, "ancestor"):
            with installation_lock(self.destination):
                pass
        self.assertEqual(list(outside_directory.iterdir()), [])

    def test_production_journal_cannot_be_replaced_across_install_and_rollback(self):
        # Model HOME entirely inside the fixture; never inspect or alter the
        # owner's actual installation/journal.
        home = self.root / "production-home"
        self.prefix = home / ".local"
        bundle = self.bundle(1)
        canonical = canonical_state(self.prefix)
        with mock.patch.object(Path, "home", return_value=home):
            with self.assertRaisesRegex(rc.Refusal, "canonical"):
                self.quiet(rc.install, {"bundle": bundle, "state": self.state, "staging_root": None})
            self.assertFalse(self.prefix.exists())
            self.quiet(rc.install, {"bundle": bundle, "state": canonical, "staging_root": None})
            # Copying a journal must not bypass the production lineage check.
            import shutil
            shutil.copytree(canonical, self.state)
            for operation in ("rollback", "remove"):
                with self.assertRaisesRegex(rc.Refusal, "canonical"):
                    self.operation(operation)
            self.quiet(rc.rollback_or_remove, {"state": canonical, "command": "remove"})
            self.assertFalse((self.prefix / "bin/control").exists())

    def test_upgrade_rollback_remove_restores_bytes_mode_symlink_and_leaves_shared_assets(self):
        control = self.destination / "bin/control"
        desktop = self.destination / "share/applications/linux-controlpanel.desktop"
        rc.write_atomic(control, b"old installed application", 0o751)
        desktop.parent.mkdir(parents=True)
        desktop.symlink_to("../../external-menu-entry")
        shared = self.destination / "lib/libAeroQt.so.0"
        rc.write_atomic(shared, b"external shared dependency", 0o644)
        prefs = self.staging / "home/fixture/.config/kwinrc"
        rc.write_atomic(prefs, b"unrelated preferences")
        baseline = {p: rc.fingerprint(p) for p in (control, desktop, shared, prefs)}
        self.install(self.bundle(1))
        self.install(self.bundle(2, extra=True))
        self.assertEqual(control.read_bytes(), b"candidate 2")
        self.operation("rollback")
        self.assertEqual(control.read_bytes(), b"candidate 1")
        self.assertFalse((self.destination / "libexec/controlpanel-display-guard").exists())
        self.install(self.bundle(3, extra=True))
        self.operation("remove")
        for path, image in baseline.items():
            self.assertEqual(rc.fingerprint(path), image)
        self.assertFalse((self.destination / "libexec/controlpanel-display-guard").exists())

    def test_all_fingerprints_are_checked_before_any_rollback_mutation(self):
        self.install(self.bundle(1))
        control = self.destination / "bin/control"
        desktop = self.destination / "share/applications/linux-controlpanel.desktop"
        desktop.write_bytes(b"intervening user edit")
        for operation in ("rollback", "remove"):
            with self.assertRaisesRegex(rc.Refusal, "Intervening edits"):
                self.operation(operation)
            self.assertEqual(control.read_bytes(), b"candidate 1")
            self.assertEqual(desktop.read_bytes(), b"intervening user edit")
        with self.assertRaisesRegex(rc.Refusal, "Intervening edits"):
            self.install(self.bundle(2))
        self.assertEqual(control.read_bytes(), b"candidate 1")

    def test_missing_obsolete_file_remains_owned_until_remove_and_blocks_new_external_file(self):
        self.install(self.bundle(1, extra=True))
        guard = self.destination / "libexec/controlpanel-display-guard"
        self.install(self.bundle(2))
        self.assertFalse(guard.exists())
        guard.write_bytes(b"new external file")
        with self.assertRaisesRegex(rc.Refusal, "Intervening edits"):
            self.operation("remove")
        self.assertEqual(guard.read_bytes(), b"new external file")

    def test_symlink_ancestor_and_outside_managed_paths_are_refused(self):
        external = self.root / "external"
        external.mkdir()
        self.destination.mkdir(parents=True)
        (self.destination / "bin").symlink_to(external)
        with self.assertRaisesRegex(rc.Refusal, "Symlink ancestor"):
            self.install(self.bundle(1))
        self.assertEqual(list(external.iterdir()), [])
        for name in ("../escape", "/tmp/escape", "bin/../control", "lib/libAeroQt.so.0", "share/icons/theme/file"):
            with self.assertRaises(rc.Refusal):
                rc.managed_path(name)

    def test_payload_mode_content_or_symlink_tampering_is_refused_before_install(self):
        directory = self.bundle(1)
        candidate = directory / "payload/bin/control"
        candidate.chmod(0o700)
        with self.assertRaisesRegex(rc.Refusal, "differs"):
            self.install(directory)
        candidate.unlink()
        candidate.symlink_to("/etc/passwd")
        with self.assertRaisesRegex(rc.Refusal, "symlink"):
            self.install(directory)
        self.assertFalse(self.destination.exists())

    def test_interrupted_write_recovers_exact_original_but_preserves_intervening_edit(self):
        original = self.destination / "bin/control"
        rc.write_atomic(original, b"old executable", 0o751)
        bundle = self.bundle(1)
        real_apply = rc.apply_object
        calls = 0
        def fail_after_first_write(*args, **kwargs):
            nonlocal calls
            real_apply(*args, **kwargs)
            calls += 1
            if calls == 1:
                raise OSError("simulated interruption after replacement")
        with mock.patch.object(rc, "apply_object", fail_after_first_write):
            with self.assertRaises(OSError):
                self.install(bundle)
        self.assertTrue((self.state / "pending.json").exists())
        original.write_bytes(b"intervening edit")
        with self.assertRaisesRegex(rc.Refusal, "Intervening edit"):
            self.quiet(rc.recover, {"state": self.state})
        self.assertEqual(original.read_bytes(), b"intervening edit")
        # Reinstating the interrupted transaction's exact candidate bytes lets
        # recovery safely identify its own replacement, rather than guessing.
        original.write_bytes(b"candidate 1")
        original.chmod(0o755)
        self.quiet(rc.recover, {"state": self.state})
        self.assertEqual(original.read_bytes(), b"old executable")
        self.assertEqual(original.stat().st_mode & 0o777, 0o751)
        self.assertFalse((self.state / "pending.json").exists())

    def test_new_files_are_removed_originals_added_during_upgrade_are_restored(self):
        self.install(self.bundle(1))
        guard = self.destination / "libexec/controlpanel-display-guard"
        rc.write_atomic(guard, b"pre-existing before upgrade", 0o750)
        self.install(self.bundle(2, extra=True))
        self.operation("remove")
        self.assertFalse((self.destination / "bin/control").exists())
        self.assertEqual(guard.read_bytes(), b"pre-existing before upgrade")
        self.assertEqual(guard.stat().st_mode & 0o777, 0o750)

    def test_bundle_records_final_binary_hashes_and_external_dependency_without_ownership(self):
        stage = self.bundle(1) / "payload"
        source = self.root / "source"
        source.mkdir()
        source_bytes, archive_bytes = b'{"fixture": true}\n', b"fixture source archive"
        identity = {"version": "fixture", "revision": "fixture",
                    "source_manifest_sha256": rc.sha(source_bytes),
                    "source_archive_sha256": rc.sha(archive_bytes), "build_id": "fixture"}
        rc.write_atomic(source / "source-manifest.json", source_bytes)
        rc.write_atomic(source / "source.tar.gz", archive_bytes)
        rc.write_atomic(source / "identity.json", rc.encoded(identity))
        rc.write_atomic(source / "build-identity.cmake", b"# fixture\n")
        external = self.root / "libAeroQt.so.0"
        rc.write_atomic(external, b"shared dependency")
        output = self.root / "recorded-bundle"
        self.quiet(rc.bundle, {"stage": stage, "output": output, "source": source,
                              "prefix": str(self.prefix), "dependency": [external]})
        _, manifest = rc.load_bundle(output)
        self.assertEqual(manifest["entries"]["bin/control"]["sha256"], rc.sha(b"candidate 1"))
        self.assertEqual(manifest["external_dependencies"][0]["image"]["sha256"], rc.sha(b"shared dependency"))
        self.assertFalse(any("AeroQt" in name for name in manifest["entries"]))
        desktop = stage / "share/applications/linux-controlpanel.desktop"
        desktop.write_bytes(b"[Desktop Entry]\nExec=control\n")
        with self.assertRaisesRegex(rc.Refusal, "Desktop Exec"):
            self.quiet(rc.bundle, {"stage": stage, "output": self.root / "bad-launcher",
                                  "source": source, "prefix": str(self.prefix), "dependency": []})


class SourceFixtures(unittest.TestCase):
    def test_archive_covers_modified_untracked_inputs_and_is_reproducible(self):
        with tempfile.TemporaryDirectory(prefix="control-rc-source-") as temporary:
            root = Path(temporary) / "repo"
            root.mkdir()
            def git(*args):
                return subprocess.run(["git", "-C", str(root), *args], check=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            git("init", "-q")
            (root / ".gitignore").write_text("build/\n")
            (root / "tracked.cpp").write_text("original\n")
            (root / "deleted.cpp").write_text("deleted from candidate\n")
            git("add", ".")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture")
            (root / "tracked.cpp").write_text("included modification\n")
            (root / "deleted.cpp").unlink()
            (root / "new.cpp").write_text("included untracked input\n")
            outputs = [root / "build/freeze-1", root / "build/freeze-2"]
            for output in outputs:
                with contextlib.redirect_stdout(io.StringIO()):
                    rc.source(argparse.Namespace(root=root, output=output, version="0.1.0-rc.1"))
            self.assertEqual((outputs[0] / "source.tar.gz").read_bytes(), (outputs[1] / "source.tar.gz").read_bytes())
            self.assertEqual((outputs[0] / "source-manifest.json").read_bytes(), (outputs[1] / "source-manifest.json").read_bytes())
            entries = json.loads((outputs[0] / "source-manifest.json").read_bytes())["entries"]
            self.assertIn("new.cpp", entries)
            self.assertEqual(entries["tracked.cpp"]["sha256"], rc.sha(b"included modification\n"))
            self.assertNotIn("deleted.cpp", entries)
            self.assertFalse(any(name.startswith("build/") for name in entries))
            with self.assertRaisesRegex(rc.Refusal, "ignored"):
                rc.source(argparse.Namespace(root=root, output=root / "not-ignored", version="0.1.0-rc.1"))


if __name__ == "__main__":
    unittest.main()
