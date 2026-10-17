# Batch 21: native Screen Locking

The Aero Control Panel now has a native Screen Locking page, reached from
Appearance and Personalization, search, both icon views and Power Options'
wake-password link. It owns the five visible scalar policies from the installed
KScreenLocker 6.6.5 KCM, hosted by KWin 6.6.6 on Wayland with KConfig 6.24.0.
This is bounded implementation coverage. Complete screen-locker appearance,
real-session acceptance and full installed-KDE parity remain open.

## Native policy and preservation

The passive adapter reads `kscreenlockerrc` with native FullConfig semantics,
including inherited defaults, local/global provenance and immutable policy:

| Group | Key | Native type and built-in default | Editor scope |
| --- | --- | --- | --- |
| Daemon | Autolock | Bool true | Owned |
| Daemon | Timeout | Double 5, minutes | Owned |
| Daemon | LockGrace | Int 5, seconds | Owned |
| Daemon | RequirePassword | Bool true | Owned |
| Daemon | LockOnResume | Bool true | Owned |
| Daemon | Lock | Bool true | Hidden legacy companion, preserved |
| Daemon | LockOnStart | Bool false | Hidden companion, preserved |
| Greeter | Theme | String empty | Preserved; no theme selector |
| Greeter | WallpaperPlugin | String org.kde.image | Preserved; not an Aero background setter |

Choosing a timeout stages Timeout and the needed Autolock change atomically.
Choosing a password grace stages LockGrace and the needed RequirePassword=true
change atomically. Unchanged companions, including protected or absent defaults,
remain untouched; a companion returned to its loaded baseline clears its intent.
Never require a password changes only RequirePassword and retains the dormant
grace. The page discloses that this policy affects other native lock paths too.
An inactive automatic-lock policy shows Never while retaining its saved timeout.

Interactive custom values follow native whole-number input: 1–9999 minutes for
timeout and 1–9999 seconds or minutes for grace. Existing fractional, negative,
nonfinite or larger native values are displayed without silently rounding or
rewriting them. Custom input opens a bounded Aero dialog; accepting it requires
the saved configuration, draft, policy and runtime to still match its context.

Restore Defaults reviews only the five owned keys and uses native skeleton
semantics. Already-default values stay unchanged, inherited defaults become
explicit typed local values, and resets without inherited entries use native
revertToDefault (including masks or removal of a global-only override). Native
defaults outside the interactive range retain their type and precision. An
unknown default is not guessed. Hidden keys and unrelated groups are preserved.
Stable snapshot comparison handles retained NaN separately from native default
comparison, where NaN is not equal to itself.

## Reload safety and persistent jobs

The complete resulting active tuple is checked before saving and again at the
configure boundary. KScreenLocker converts a positive timeout only when Autolock
is true. KF IdleTime 6.24 then narrows milliseconds to a signed int; the adapter
checks that exact conversion range, including safe truncation below a millisecond.
Dormant values need not fit the interactive range. The hidden Lock=true branch
always multiplies LockGrace by 1000, even when automatic locking or password
requirements are off; signed overflow is blocked. Unknown active companions
block Apply rather than being invented.

Arithmetic-safe negative grace and Lock=false can produce indefinite native idle
grace. They are retained with an explicit warning; negative grace is never offered
as a new choice or confused with the UI's Never sentinel.

Discovery uses existing-name GetNameOwner, strict configure introspection,
KWin supportInformation and protected installed-package/process identity. It pins
org.kde.screensaver, org.freedesktop.ScreenSaver and org.kde.KWin to the same unique
owner and checks owner epochs. Where CAP_SYS_NICE makes /proc/PID/exe
unreadable, compatibility checks fall back to that D-Bus PID's comm/cmdline,
self-reported native support information and protected package hashes. This is
weaker evidence than executable attestation and does not prove which library
bytes are already loaded. No live-write acceptance is inferred from it.
It does not instantiate native
KScreenSaverSettings, whose constructor registers a global shortcut. No discovery
call activates a service or invokes configure, Lock, inhibition or a shortcut.
Confirmed absence of both locker names permits an offline save; transport failure
or an unverified running owner does not.

Apply rereads provider, sources and policy, saves only explicit intents and checks
fresh typed readback. A second owner-pinned check and fresh file guard precede
`/ScreenSaver` org.kde.screensaver.configure(). Its no-argument, void reply means
the handler returned; it does not prove future physical locking, password prompts
or sign-in persistence. Configure may update idle timers and resume inhibition.
No KConfig Notify substitute is used. A missing service is left stopped and reload
is reported as deferred. Timeouts do not cancel an already queued native call.
KConfig provides no atomic compare-and-swap against another process. Native sync currently runs on
the UI thread; the deliberate lock-contention fixture took about 45 seconds.
Moving this bounded filesystem transaction off the UI thread without permitting
a late write after timeout remains a responsiveness obligation; the fake held-job
navigation tests do not prove responsiveness during a contended native sync.

The application-owned session survives page destruction and participates in
navigation, close and background-work guards. Failed saves, changed sources or
owners, unacknowledged reloads and failed Revert preserve the draft. The shortcut
button opens the existing global shortcut editor and selects the discovered
registered ksmserver/Lock Session action when unambiguous. It creates no action,
invokes no shortcut and preserves all alternatives until an explicit edit and
Apply in that shared shortcut session.

## Fixed Aero appearance

The approved io.gitgud.wackyideas.desktop shell reads its background through
Aero's custom SDDM provider, not KDE's Greeter/Wallpaper image settings. The passive
reader pins the approved global theme/shell/API and audited AuthUI, LockScreen and
provider bytes, then follows the provider's bounded file resolution. Its result
is file evidence, not image rendering or active greeter adoption. It does not load
QML, a plugin or a native settings object, and offers no alternate theme.

The background shares privileged SDDM assets. The locally inspected existing
Aero helper removes the old file before copying a caller-selected path and lacks
the required atomic, fixed-path transaction. It is not reused. A future native
background editor needs an approved-path/provider contract, bounded image
validation, symlink/race protection, administrative authorization for a concrete
change, atomic replacement, preservation/readback and recovery. The provider only
watches the SDDM configuration file; editing an image does not establish live
reload. Binary hashes pin the inspected installation; equivalence to the local
custom-provider source checkout has not been established.

## Verification

All **88 CTest suites passed in 166.42 seconds** after the final build. The earlier
focused run passed all eight relevant suites in 52.22 seconds. Five offscreen Aero
captures were inspected: normal policy, staged changes, passwordless policy,
retained native legacy values and uncertain Apply. A subsequent build reported no
work; the five-file DESTDIR install stage passed. No live installation occurred.

Four inert production snapshots match exactly: KScreenLocker 6.6.5, existing
owner :1.924, nine known native fields, five writable preferences, a safe current
reload projection, and matching approved Aero shell/provider/background file
evidence. No configure or physical test is inferred from discovery. Both later
probe intervals left all 51 monitored file fingerprints unchanged. A targeted
file-operation trace of the fourth probe and its package-query child recorded no
writable file opens or mutation calls; all 54 paths in its expanded inventory
stayed unchanged. That trace does not exclude effects in other processes.

Across the complete interrupted build/validation interval, 47 of 51 final
fingerprints match baseline. Two service-cache files, kwinoutputconfig.json and
knighttimestaterc differ; the location-state drift was already present before
building, while the other drift was first observed across the interrupted build
and first probe without an intervening fingerprint. A transient kglobalshortcutsrc
change affected two labels with matching observed binding/default prefixes and
returned to baseline. Before/after bytes are preserved privately. Causes remain
unattributed; indirect runtime effects are not excluded and no live-write credit
is awarded. Final inventory also found two previously unmonitored service-cache files and
org.kde.plasma-systemmonitor.desktop. Their first observation was at final
inventory; creation time and attribution are unverified. Their bytes and hashes
are preserved privately too.

Artifacts are under `build/batch21-validation`, including source contracts, build
and test logs, native reads, captures, install staging, fingerprints and audit
invariants. Validation invoked no live preference setters, configure/reloads, Lock,
inhibition, shortcut registration/invocation, location requests, application
launches, cache rebuilds or privileged appearance helpers. Live locking,
authentication, resume behavior, privileged appearance editing and persistence
acceptance remain unverified.

## Primary source contracts

- [KScreenLocker 6.6.5 schema](https://github.com/KDE/kscreenlocker/blob/v6.6.5/settings/kscreenlockersettings.kcfg), [native form](https://github.com/KDE/kscreenlocker/blob/v6.6.5/kcm/ui/main.qml) and [custom duration input](https://github.com/KDE/kscreenlocker/blob/v6.6.5/kcm/ui/DurationPromptDialog.qml).
- [Native settings and shortcut constructor](https://github.com/KDE/kscreenlocker/blob/v6.6.5/settings/kscreensaversettings.cpp), [KCM save](https://github.com/KDE/kscreenlocker/blob/v6.6.5/kcm/kcm.cpp), [configure and idle/grace paths](https://github.com/KDE/kscreenlocker/blob/v6.6.5/ksldapp.cpp), [service interface](https://github.com/KDE/kscreenlocker/blob/v6.6.5/interface.cpp) and [D-Bus schema](https://github.com/KDE/kscreenlocker/blob/v6.6.5/dbus/org.kde.screensaver.xml).
- [KWin 6.6.6 locker host](https://github.com/KDE/kwin/blob/v6.6.6/src/wayland_server.cpp), [IdleTime 6.24 narrowing](https://github.com/KDE/kidletime/blob/v6.24.0/src/kidletime.h), [KConfig skeleton defaults](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kcoreconfigskeleton.h) and [typed item reads](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kcoreconfigskeleton.cpp).
- [Greeter package initialization](https://github.com/KDE/kscreenlocker/blob/v6.6.5/greeter/greeterapp.cpp). Local custom-provider evidence is separately pinned in ScreenLockAppearance.cpp; it is not official KDE behavior.
