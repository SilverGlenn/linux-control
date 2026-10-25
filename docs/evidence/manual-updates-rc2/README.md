# Manual update RC2 evidence

These results identify the exact frozen artifact in validation-summary.json.
Source was extracted from its archive and its bytes/modes checked before building.
The archive remains in build/manual-updater-work/bundle/source/source.tar.gz.
The installed payload has 12 files; the trusted updater runtime is a separate
private bootstrap, outside payload ownership. Original deployment history remains.

full-ctest.log records 112/112 passing isolated CTest suites (292.92 seconds).
Staged validation exercises actual-payload install/upgrade/rollback/removal, foreign
edit refusal and one deliberate interruption after the third changed object.
It does not claim an interruption at every replacement boundary.

manual-ready-fixture.png is a fake-provider Aero fixture, not a live downloaded
release. installed-manual-check.json is a real read-only request from the installed
helper: no published candidate releases. Real Cloud builds, attestations, a real
release update and installed GUI/hardware acceptance remain unverified. No live
settings writes, session restart or reboot was used. Charging policy was passively
compared: all three policy fingerprints remained unchanged; private raw records
and original app backups are not published here.

This evidence and result-document updates are published after the source freeze.
They are not compiled inputs. Code/tests/build/tool inputs remain identical to the
archived source; historical Batch 25/parity evidence and RC1 results stay historical.
