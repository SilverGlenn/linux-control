# Batch 20 — native Day and Night

This batch adds a native Aero editor for KDE's shared Day and Night schedule,
linked from Night Light and available through Hardware and Sound, search and
both icon views. It targets Plasma Workspace 6.6.6, KNightTime 6.6.5, KConfig
6.24.0 and Qt 6.10.2. Full installed-KDE parity and real-session acceptance
remain incomplete.

## Configuration and native defaults

The seven preferences in `knighttimerc` are General/Source (`Location` or
`Times`), Location/Automatic, Latitude and Longitude, and Times/SunriseStart,
SunsetStart and TransitionDuration. Times use native QTime serialization;
duration uses an unsigned count of seconds. Interactive edits offer one to
600 minutes, while untouched native values retain their exact seconds and
unsupported values remain visible. Native QTime writes drop milliseconds; changed
Set/inherited-default operations show that persisted precision in the draft,
while untouched values and native default no-ops retain fractional seconds. The audited built-in defaults are location,
automatic positioning, coordinates zero, 06:00/18:00 and 1800 seconds.

The native editor opens FullConfig, including `kdeglobals`; the daemon opens
NoGlobals. The page keeps both views, defaults, source revisions and immutable
policy. A global-only Automatic=false can therefore be the editor's saved
choice while the daemon's input remains automatic=true. Privacy and forecast
reviews use the projected daemon view. Removing a global-only value does not
pretend to change that daemon input or prove a reload. Policy from either view
can prevent an edit.

Restore Defaults follows native skeleton semantics: an already-default entry
is a no-op, inherited defaults become typed local values, and no-inherited
resets use native revertToDefault, including global-only removal or local
masks hiding a global value. Each changed operation is explicit and reviewed;
unknown defaults are not guessed. Readback verifies both typed views and
untouched companions, rather than equating file persistence with adoption.

Native time validation adjusts only the edited start time; a duration edit
can adjust the evening start as a coupled operation. The controller stages
coupled edits atomically and rejects an immutable companion. Selecting a map
position similarly stages latitude, longitude and manual mode together.
Source switches preserve dormant data. Malformed tuples can be repaired one
explicit field at a time without guessing unknown companions; Apply stays
blocked until the resulting active tuple is valid. Before Notify, the complete resulting
active daemon branch must be valid. In particular, an unknown source is
blocked because the audited daemon's reload path can dereference an absent
scheduler. Automatic mode also validates the custom-time fallback. Manual
solar mode can retain unsupported dormant custom times without rewriting them.

Apply preflights provider identity, policy and both source views, saves only
requested keys with native KConfig Notify, and checks fresh readback. Changed
sources/policy/owner epochs, partial or uncertain saves and failed Revert retain
the draft. The persistent session survives page destruction and participates
in navigation, close and background-work guards. Native KConfig provides no
atomic compare-and-swap against other writers.

## Forecast, offline map and explicit location

Forecasts call the installed KNightTime library, isolated behind a C++20
adapter; the rest of the project remains C++17. These are local computations
with a displayed reference/time zone, not a running-service schedule getter.
Times are passed as seconds, correcting the native KCM preview's seconds-as-
milliseconds discrepancy. Solar-unavailable/polar cases use the daemon's
compiled 06:00/18:00/30-minute fallback; automatic mode without a usable
coordinate uses its custom-time tuple. Safe native durations outside the
interactive editor range can still be forecast without normalizing them.

The equirectangular map is bundled from KDE and works offline. Mouse and
keyboard coordinate selection, panning and zooming do not request location.
The map's original TUBS/Wikimedia CC BY-SA 3.0 attribution is retained in the
page and [asset notice](../src/ui/assets/worldmap-attribution.txt).

Saved automatic coordinates may be read from `knighttimestaterc`; that state
contains no timestamp, so the page describes their age as unknown. The daemon
only recreates its solar scheduler after sufficiently large movement, so a
forecast from its latest cache is an estimate, not proof of its loaded schedule.
Opening the page, reading configuration, staging defaults and running a forecast
never construct a location provider.

An explicit request creates a Qt Positioning provider under this app's
`linux-controlpanel` desktop identity. The privacy text explains that the system
provider may use GPS, cell, Wi-Fi or IP/network services. Acquisition is an
app-bounded one-shot using startUpdates/stopUpdates, avoiding the audited
GeoClue plugin's non-cancellable requestUpdate timer path. Pre-request cached
samples are ignored; generation tokens reject late results. Timeout, denial,
unavailable provider and invalid samples are reported. Leaving the page
cancels acquisition and releases the provider; Stop is requested but the public
API provides no physical-stop acknowledgement. The sample is temporary unless
an explicit Use manually action stages it, followed by Apply.

Applying a permanent automatic configuration can itself make an already
running daemon request location. That is reviewed separately from temporary
page acquisition. No temporary coordinate is silently persisted.

## Inert discovery and adoption limits

Discovery resolves GetNameOwner without activation, checks API Version=1 on the
unique owner, verifies installed package versions and protected executable/library
identity, and rechecks owner/epoch. It never calls Subscribe or Unsubscribe.
The daemon exposes no loaded configuration serial or independent settings getter;
saving and Notify are therefore reported separately from unverified adoption.
A confirmed absent owner permits offline configuration, without starting the
service. An owner-query transport failure does not count as confirmed absence.
On later activation, native legacy NightColor migration can precede loading the
saved settings; the offline result discloses that limitation.

The program must never be queried with `knighttimed --version`: its audited main
has no safe version-only path and starts manager/migration/location work before
acquiring its unique service. Production validation uses only our inert probe.
Its JSON redacts all coordinate values.

## Build and verification

Development packages now include `knighttime-dev`, `libkf6holidays-dev` and
`qt6-positioning-dev`. This checkout's validation extracts exact matching Ubuntu
packages under ignored `build/deps/nighttime/root`, without installing packages
or changing the live session. Normal builds can use installed development
packages. Runtime KNightTime, KHolidays and Qt Positioning libraries are required.

Validation results are recorded after the final build in
`build/batch20-validation`. Private-bus/config fixtures exercise writes and Notify;
location tests use injected fakes. No live setter, Notify/reload, location request,
NightTime subscription, live shortcut registration/invocation, service-cache rebuild or live
installation is part of this validation. Physical behavior, authorization with
the actual provider, daemon reload adoption, migration and sign-in persistence
remain unverified.

## Primary source contracts

The source audit uses the exact installed versions:

- [Plasma Nighttime editor and schema](https://invent.kde.org/plasma/plasma-workspace/-/tree/v6.6.6/kcms/nighttime): settings, time validator, preview, position UI and map.
- [KNightTime daemon](https://invent.kde.org/plasma/knighttime/-/tree/v6.6.5/src/daemon): NoGlobals, migration, automatic provider, scheduler/fallback and notification behavior.
- [Native forecast API](https://invent.kde.org/plasma/knighttime/-/blob/v6.6.5/src/kdarklightschedule.h): pure forecast, transitions and cycles.
- [KF Config 6.24](https://invent.kde.org/frameworks/kconfig/-/tree/v6.24.0/src/core): native types, cascading, revertToDefault and Notify.
- [Qt Positioning 6.10.2 GeoClue plugin](https://code.qt.io/cgit/qt/qtpositioning.git/tree/src/plugins/position/geoclue2?h=v6.10.2): desktop identity, cached fixes and start/stop/request lifetime.


Final validation: all **83 CTest suites pass in 116.91 seconds**. The focused
page runner passes 26 QtTest results, and six final Aero captures were inspected.
The final build reports no work remaining. DESTDIR staging contains the expected
five files; nothing was installed into the live prefix.

Two final inert reports match exactly: existing owner `:1.17`, KNightTime6.6.5,
API1, and seven known/default-known/writable preferences in each configuration
view. A cached automatic coordinate is available, with its value redacted and
age unknown. This proves saved discovery and provider identity, not the loaded
schedule or a location acquisition. An earlier report helped expose an
application-specific state-path mistake; the adapter now uses KDE's generic
state directory and has a native private-state regression fixture.

Final fingerprints show **32/34 monitored paths unchanged**. A service-cache
file and the automatic-location state had already changed before the first
build, test or native probe. The latter's Latitude entry changed; coordinate
values and private before/after copies are not published. Both changes remain
unattributed and were preserved; indirect runtime effects are not excluded.
No accepted live-write credit is inferred. No new application/service-cache
files were found in the monitored directories.
