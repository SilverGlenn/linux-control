# Batch 4: native KDE integration proofs

This batch adds three Windows 7 styled editors using the current installation's
real configuration interfaces. It proves specific paths; it does not complete
KDE System Settings parity. The frozen module inventory remains unchanged.

## Implemented boundaries

| Domain | Native control | Authoritative state | Apply and verification |
| --- | --- | --- | --- |
| Display | Screen Resolution: advertised resolution/refresh mode on one connected, enabled, fixed-mode output | `kscreen-doctor --json`, using the installed libkscreen 6.6.4 serializer schema | A separate unprivileged guard performs preflight, mode-only apply, readback, confirmation, and verified rollback. |
| Power | Plugged-in, battery and low-battery idle display/sleep timeouts | PowerDevil 6.6.6 `powerdevilrc` nested profile groups, loaded through KF6 KConfig schemas | Preflight, modified-key save, `refreshStatus()` reply plus `configurationReloaded`, and persisted-value readback. |
| Window behavior | Title-bar double-click action | KWin 6.6.6 `kwinrc` / Windows / TitlebarDoubleClickCommand | Preflight, one-key KConfig save, no-reply `reconfigure`, bounded polling of KWin's active diagnostic option, and persisted-value readback. |

Each settings session belongs to the application, survives view replacement,
preserves staged selections on external-change conflicts, and participates in
navigation and exit guards. Missing capabilities remain visibly unavailable.
The existing immediate performance-profile controls remain independent of the
staged PowerDevil editor.

## Evidence and compatibility

Read-only probes of the installed session confirmed a connected eDP output with
advertised modes, the PowerDevil support/reload methods and signal, system battery
capability, and KWin 6.6.6's `operationTitlebarDblClick` diagnostic field. The
manual `kde_backend_probe` exercises the actual C++ readers without writing.

Display command/schema behavior is based on KDE's
[libkscreen 6.6.4 doctor implementation](https://raw.githubusercontent.com/KDE/libkscreen/v6.6.4/src/doctor/doctor.cpp).
A zero process exit is insufficient: actual output state must match the requested
mode. Only mode identifiers advertised by a validated snapshot are accepted;
commands use fixed executable paths and argument lists without a shell.
The guard refuses automatic preferred-mode and multiple-display configurations.
Brightness and color changes do not prevent a mode-only rollback; identity,
topology, available modes, and layout changes do. Its notification pipe is
nonblocking so an unresponsive application cannot stall restoration. Its deadline
starts before apply, but bounded compositor/tool calls can delay completion of
rollback. An external conflict or an unreadable state is reported explicitly.

Power keys and defaults follow KDE's
[PowerDevil 6.6.6 profile schema](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/PowerDevilProfileSettings.kcfg)
and [daemon implementation](https://raw.githubusercontent.com/KDE/powerdevil/v6.6.6/daemon/powerdevilcore.cpp).
The default inputs use the same tablet-mode portal and systemd virtualization
information as this release. Unknown inputs disable affected absent-default
fields. UPower's system battery and suspend/display capability checks gate edits.
Inherited defaults and immutable KConfig policy are preserved. A reload
acknowledgement is distinct from proof that an idle action will run: activity
policy and application inhibitors still apply.

KWin's key/property mapping and live diagnostic serialization are verified against
[options.h](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/options.h),
[options.cpp](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/options.cpp),
[workspace.cpp](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/workspace.cpp),
and [the D-Bus interface](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/dbusinterface.h).
The reload method has no reply, and `reloadConfig` is not a completion signal.
Verification therefore reads the active `operationTitlebarDblClick` field after
reload. That diagnostic format and enum mapping are a version-pinned dependency;
an unsupported runtime version disables Apply. Failure after saving retains an
explicit saved-versus-live distinction rather than claiming an automatic undo.

## Validation and remaining scope

The application and helpers build successfully. All 29 test suites passed; the
private D-Bus suite was run outside the execution sandbox's socket restriction.
The real read-only C++ probe reported all three domains ready, a usable display
rollback helper, and editable timeout fields for all three power profiles.
Windows-style page/confirmation screenshots were checked, and a DESTDIR install
contained the application and both helpers. No live display, power, or window
preference was changed during validation.

Automated tests use injected backends, temporary KConfig files, fake doctor
processes, and a private D-Bus daemon. They exercise staged Apply/Revert, external
conflicts, policy immutability, unavailable providers, readback failures, reload
cancellation, view lifetime, navigation/exit guards, and actual watchdog
Keep/timeout/EOF/backpressure behavior. Optional screenshots cover all three
native pages and the display confirmation state. Tests do not change the working
desktop's modes, power policy, or title-bar behavior.

Display arrangement/scaling/rotation, multi-output recovery, full PowerDevil
policies, broader window management and appearance, authorized account/network
operations, nested Aero settings, and APT-provider simulation remain on the plan.
Before extending either version-pinned backend to another KDE release, review the
matching schemas, reload behavior, capability sources, and runtime verification.
