# Batch 19 — native Night Light

This batch adds an Aero Night Light page to Hardware and Sound, the icon catalog,
search and the Screen Resolution sidebar. It implements the four Night Light
preferences exposed by the installed Plasma Workspace 6.6.6 frontend and KWin
6.6.6 runtime. Full installed-KDE parity remains incomplete. No physical display
effect, live preference write, preview, notification/reload adoption or sign-in
persistence acceptance is claimed by the validation workflow.

## Native settings and draft behavior

The page stages enablement, Constant or Dark/Light mode, and independent day and
night temperatures. Supported replacement temperatures are 1000–6500 K, with
100 K slider steps and an exact numeric editor. Day controls are inactive in
Constant mode; both temperature controls are inactive while disabled. Their
untouched values remain saved. Mode uses native enum names `Constant` and
`DarkLight`, corresponding to runtime values 0 and 1. The installed D-Bus XML's
older four-mode comment is obsolete and is not used as an enum contract.

Each field retains typed saved-value/default knowledge, effective/local/system/
user-global presence, immutable policy and source revisions. Unsupported raw
numeric values are displayed without normalizing them on an unrelated edit.
Supported explicit replacements can repair unknown values and establish a local
override even when the same number was previously only a fallback. Unknown
defaults cannot be reported as restored.

Restore Defaults stages the native editor operation. The frontend clamps loaded
and default temperature values to its editor range before comparing them; a raw
20000 K value can therefore be a native default no-op when its editor value is
already 6500 K. The raw value stays intact in that case. Changed inherited
defaults save a typed local value; no-inherited defaults use KF's
`revertToDefault`, including global-only removal and local deletion masks that
hide a user-global value. Already-default explicit entries remain present.
The review identifies changes and unavailable defaults before staging them.

The app-owned session survives navigation and view destruction. Apply validates
the complete request, rereads provider identity, sources and policy before
saving, changes only explicit operations, and verifies saved values and untouched
companions afterward. Foreign changes, changed defaults/policy, service loss,
malformed requests and uncertain/partial results retain the draft. A failed
Revert retains it as well; only a successful explicit reload discards it. The
shared navigation, close and background-work guards cover the new session.
Native configuration does not provide an atomic compare-and-swap against other
writers.

Complete numeric text stages through the editor locale. Incomplete text, cursor/
selection and nontracking slider gestures survive read-only runtime refreshes.
Deferred input must still match its captured baseline, draft, provider epoch,
mode and enablement before it can stage. Apply/Revert, policy or context changes
reject stale input with an explanation; invalid input is resynchronized to the
actual draft rather than left looking saved. Focus and painting do not establish
an absent override.

## Runtime observations and preview

The adapter first resolves the existing owners of `org.kde.KWin.NightLight` and
`org.kde.KWin`; they must be the same unique owner. It checks the audited KWin
version/Wayland mode, interface signatures and strictly typed properties, then
rechecks ownership. Owner epochs also detect plugin removal/re-registration
under the same KWin process. Calls use that unique owner, not an activation-prone
method call to an unchecked well-known name.

Native saves use per-key `KConfigBase::Notify`, matching the frontend's generated
skeleton. KWin's Night Light manager listens to its KConfig watcher; the broader
`/KWin.reconfigure` method is not substituted for this path. Notification is not
a causal acknowledgement. Fresh saved-file readback and the public enabled,
mode and currently relevant target state are checked where corresponding saved
values are known. KWin exposes neither both loaded temperature preferences nor a
reload serial. The result text states these limits rather than claiming complete
runtime adoption. Current temperature can also differ during transitions,
inhibition or previews and is shown separately.

Explicit Preview buttons request a temporary screen-color change without saving
the draft. The daemon expires previews 15 seconds after execution. Stop, hiding the page
and destroying it request early cleanup against the pinned owner and epoch;
late callbacks cannot start a tracked preview for a replacement service. Failed
or uncertain preview/Stop replies retain a bounded possible-active state and an
explicit Stop retry. A transport timeout does not cancel a queued method; it may
execute later. Local expiry ends only the tracking window and does not prove
physical cleanup or send a delayed global Stop. Native
preview is shared across clients and has no ownership cookie: replacing another
preview or stopping a newer foreign preview cannot be excluded. It is never used
as read/reload/adoption proof.

Runtime status distinguishes configured enablement, reported enabled/running/
inhibited state, daytime/mode, current and target temperatures. Transition start
timestamps are Unix seconds; durations are milliseconds. The page renders exact
local times with offsets and bounds invalid/overflowing inputs. These are
reported transitions, not an inferred location or fabricated schedule.

## Separate Day and Night coverage

The frontend's Configure action points to the separate `kcm_nighttime` module.
That module stores seven preferences in `knighttimerc`, backed here by KNightTime
6.6.5: source (Location/Times), automatic location, manual latitude/longitude,
sunrise/sunset start times and transition duration. Native defaults are Location,
automatic location, coordinates 0/0, 06:00/18:00 and 1800 seconds. The frontend
edits duration from 60 to 36000 seconds and validates it against both day/night
intervals. Its FullConfig defaults differ from the daemon's NoGlobals inputs.
This batch
does not implement those forms or launch an embedded KDE screen; the Night Light
page explains that it follows the separately configured KDE day/night schedule.
The corresponding inventory and linked-control obligations remain incomplete.

Constructing `KDarkLightScheduleProvider` can subscribe to or activate the
Nighttime daemon; startup can migrate old NightColor keys and start location
updates. The inert read-only probe does not construct that provider, subscribe,
request location, preview, inhibit or invoke any preference/reload method.

## Primary source contracts

- [Night Light frontend](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/nightlight/ui/main.qml)
- [Frontend schema and bounds](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/nightlight/nightlightsettings.kcfg)
- [Frontend generated notifier configuration](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/nightlight/nightlightsettings.kcfgc)
- [Native KWin manager and watcher](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/plugins/nightlight/nightlightmanager.cpp)
- [Actual mode enum](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/plugins/nightlight/nightlightmanager.h)
- [D-Bus properties and timestamp units](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/plugins/nightlight/nightlightdbusinterface.cpp)
- [Native runtime schema](https://raw.githubusercontent.com/KDE/kwin/v6.6.6/src/plugins/nightlight/nightlightsettings.kcfg)
- [KF native defaults and enum storage](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kcoreconfigskeleton.cpp)
- [KF native default operation branches](https://raw.githubusercontent.com/KDE/kconfig/v6.24.0/src/core/kcoreconfigskeleton.h)
- [Separate Nighttime frontend](https://raw.githubusercontent.com/KDE/plasma-workspace/v6.6.6/kcms/nighttime/ui/main.qml)

## Validation

The final Debug build passes all **77 CTest suites in 110.55 seconds**. The
separate 76-suite regression and corrected page suite pass as well. Final
no-work build and five-file DESTDIR staging pass; no live install was performed.
Five fake-provider Aero captures were inspected: scheduled draft, Constant mode,
unsupported/raw values, runtime/timeline and uncertain Apply. Text, disabled
controls, the numeric-unit-only unknown editor, exact timestamps and error
messages remain readable. The initial blank-editor assertion was corrected to
check numeric text rather than its harmless K suffix; no fabricated visible
number or production-code correction was involved. Earlier logs are retained.

The fixtures use temporary configuration/cache/data/runtime directories and
private session buses. Native cascade tests run under an additional private
bus so KConfig Notify cannot target the live compositor. Coverage includes
first/last group keys, enum storage, raw/default clamping, unknown defaults,
global origins/deletion masks, equal-number explicit replacements, immutable
companions, exact default-source changes, foreign changes, service/epoch loss,
malformed schema/types/version reports, timeouts/uncertain outcomes, failed
Revert, retained jobs, navigation and keyboard/locale/input refresh races. The
preview lifetime test checks that local expiry sends no global Stop. All preview,
Stop, persistence and notification operations exercised by tests target fakes.

Two production getter-only probes return identical reports: KWin 6.6.6 owner
`:1.15`, four known/default-known and writable keys, enabled Dark/Light mode,
day 6500 K/night 4000 K and current/target 6500 K during daytime. The public state
matches known saved values. This is discovery and observation evidence, not
live write, preview, reload adoption, inactive-temperature or physical/signon
acceptance. No Nighttime provider was constructed or subscribed, and no
location request, preference setter/reload, preview/Stop, inhibition,
registration or cache rebuild was invoked by this workflow.

Final byte fingerprints find **29 of 31 monitored paths unchanged**, with no new
application entries or service-cache files. Before automated build/tests/probe
execution, `kwinoutputconfig.json` stored brightness changed from 0.70 to 1.0
(mtime 07:08:45 UTC). After the final tests and second probe, `kdeglobals` moved
the existing `BrowserApplication=firefox-geckium.desktop` line within General
without changing its value (mtime 07:30:06 UTC). Both changes remain unattributed;
indirect runtime effects are not excluded. Both live files were preserved.
Owner-only before/intermediate/after copies and exact differences retain
evidence. This drift does not count as accepted live-write verification.

Ignored `build/batch19-validation` retains configure/build/focused/full logs
and full detail, five inspected captures, repeat probe reports/summary, the
private fixture driver, byte fingerprints/differences and staged install. The
capability audit retains all 662 prior row IDs, evidence and module statuses,
adds one bounded cross-key workflow (663 total/148 implemented scopes), and
records zero whole-module completions, accepted live writes or hardware
exclusions. Separate scheduling, other installed modules and real acceptance
continue to block full KDE parity.
