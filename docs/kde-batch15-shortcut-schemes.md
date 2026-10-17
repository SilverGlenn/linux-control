# Batch 15: native KDE shortcut schemes

The native Aero shortcut editor can review and export KDE `.kksrc` schemes
across global and standard application shortcut scopes. Import produces a
typed draft plan; it does not write preferences, register or invoke commands,
activate contexts, record keys, reload a keyboard configuration, or launch a
KCM. The shared controller owns conflict review and Apply. The custom-command
session separately owns definition staging and its reviewed native Apply.

## Exact primary-source audit

The following official tagged sources were fetched read-only on 2026-10-17
into `/tmp/batch15-codec-source`. The earlier `/tmp/batch13-kde-source` cache
was absent in this environment; the audit used freshly retrieved exact tags.

- [Plasma Desktop 6.6.6 global shortcut model](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/globalaccelmodel.cpp),
  `exportToConfig` / `importConfig`, lines 294–342: each selected component
  uses `[component][Global Shortcuts]`, with native action IDs as keys and
  `QKeySequence::listToString` / `listFromString` values. The native model
  reduces alternatives to a set; this codec preserves their original order,
  all alternatives and every chord. The source format has no context field.
- [Plasma Desktop 6.6.6 standard shortcut model](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/standardshortcutsmodel.cpp),
  lines 69–141: `[StandardShortcuts]` is a flat map of stable standard action
  names to Qt sequence lists. Native `findByName` and catalog categories
  resolve those names; category labels are not serialized into the scheme.
- [Plasma Desktop 6.6.6 shortcut KCM](https://github.com/KDE/plasma-desktop/blob/v6.6.6/kcms/keys/kcm_keys.cpp),
  lines 128–207: custom definitions use
  `[Custom Commands][component.desktop]` with `Name` and `Exec`; built-ins
  come from every GenericDataLocation `kcmkeys` directory, directly matching
  `*.kksrc`, with `[Settings]Name` and filename fallback for display.
  Its import creates/matches commands and attempts asynchronous reimport.
  This replacement retains definitions and explicit migration identities
  without performing those native actions during review.
- [Qt 6.10.2 key-sequence implementation](https://github.com/qt/qtbase/blob/v6.10.2/src/gui/kernel/qkeysequence.cpp),
  `assign`, `decodeString`, `listFromString`: native portable alternatives are
  separated by `; ` and chords by commas, with special handling for the
  comma key and plus key. Qt stops after four chords; the codec checks the
  complete input first, so a fifth chord is rejected instead of truncated.
  A doubled trailing comma is rejected before reaching Qt's indexing path.
- [KF6 Config 6.24.0 INI implementation](https://github.com/KDE/kconfig/blob/v6.24.0/src/core/kconfigini.cpp),
  `stringToPrintable`, `printableToString`: exported identities and values
  use KConfig escaping. Preflight validates escapes and UTF-8 before asking
  KConfig to interpret the document, including literal retained `\;` and
  `\,` desktop escapes. Hex escapes require exactly two hexadecimal digits.

Source SHA-256 values:

| Source | SHA-256 |
| --- | --- |
| `globalaccelmodel.cpp` | `38ec32335a215caa8c0bae66a58db24655f3acf268791342819de626b0273acd` |
| `standardshortcutsmodel.cpp` | `be648db75d650067aae8f448cf68b328889e5816057b71afed0bb3baf074a401` |
| `kcm_keys.cpp` | `3e256c9280551ca5486efdab568e5b868ee1345c7268b530dc8fbc0f7a99ba86` |
| `qkeysequence.cpp` | `21ec1a8290c6682fe4ff99cabc191a039447f7fcb72d2196c166c8431d5f4fd6` |
| `kconfigini.cpp` | `97d54080daf73903d84a5a5536e4dc3807ac58f32e544890c31620370b1c1be9` |

## Codec and import review

`ShortcutScheme` retains the scheme name, full binding lists, and all custom
command definitions. An empty value or legacy `none` disables a binding.
Values use portable Qt text, preserving punctuation keys, modifier keys,
up to four chords per sequence and up to 64 ordered alternatives per action.
The parser rejects unknown key names, duplicate alternatives, mixed empty
alternatives and extra chords. Shadowing and cross-action conflicts remain
the controller's explicit review decision rather than being silently removed
by the codec.

Global identities remain the exact component, context and action tuple.
The native global sections mean context `default`. Nondefault contexts use
the explicit extension below:

```ini
[LinuxControl Scheme]
ContextVersion=1

[LinuxControl Context Shortcuts][component.desktop][context-id]
action-id=Meta+J; Ctrl+K, Ctrl+J, Ctrl+M, Ctrl+N
```

The version is mandatory for these entries. Unknown versions and a `default`
alias in the extension are errors. Native KDE can read the compatible native
sections; its current importer does not consume this context extension.
Export reports this limitation. The replacement never guesses or activates
a context to make a binding match.

Parsing is bounded to 4 MiB, 65,536 bytes per line, 20,000 entries and 4,096
group headers. UTF-8, NUL bytes, group nesting, named keys, escape syntax,
duplicate decoded keys and duplicate decoded group headers are checked
before KConfig. Localized `Name[...]` metadata is allowed; localized bindings
and KConfig expansion, deletion or immutability flags are refused. Unknown
groups/metadata with entries fail visibly. Missing or malformed command
definitions fail visibly. A rejected document has no valid import plan.
The strict parser deliberately refuses repeated INI sections that KConfig
would otherwise merge.

Import resolves global tuples exactly and standard action names against the
installed scoped snapshots. Duplicate snapshot identities or migration
collisions invalidate the plan. Unknown components, unknown actions,
unmatched contexts and unavailable/read-only assignments remain in the
unresolved list with their complete keys and a reason. Matching immutable
assignments are also reported. No list is truncated to fit the UI. Snapshot
indexes avoid rescanning the whole registry for each unresolved identity.
Successful parsing does not imply that every imported binding can be staged.

## Custom command definitions and migrations

Definitions preserve literal `Name` and `Exec` strings without expanding
environment variables or executing the command. `planImport` retains every
definition and emits `CommandDefinitionReview`. Missing registered command
actions produce `PendingCommand` unresolved bindings. Explicit migration is
a supplied old-component → new-component map; the same map changes both the
definition component and global binding identities. Mappings must refer to
a definition in the document, target a native `.desktop` component, and
produce no identity collisions. Names, command text and installed apps are
never used to guess a replacement ID.

The dialog stages selected definitions through the custom session's atomic
`stageCommands` operation. New definitions still require reviewed Custom
Shortcut Commands Apply, then a global shortcut Refresh and renewed binding
review. Definition staging alone does not resolve an unregistered action.
Binding drafts for the global and standard scopes use the shared atomic
two-session staging operation; their later native Apply operations retain
each backend's existing transaction and partial-result semantics.

There is no complete automatic coordinator that creates definitions,
waits for registration, refreshes all sessions and applies pending bindings
as one transaction. That dependency workflow remains goal work. The codec
does not claim native command creation or successful shortcut invocation.

## Export and installed schemes

Export uses the staged snapshots and supplied staged command definitions.
Each enabled scope supports all entries or the union of selected exact
actions and components. Custom definitions follow the global selection,
including explicitly selected command components without registered
bindings. Stale explicit selections fail instead of disappearing from the
output. Standard categories are flattened to unique native action names;
nondefault standard contexts are refused.

KConfig serializes into memory first. The codec reparses and compares every
binding and command definition before creating the destination. It writes
only an absolute regular `.kksrc` destination with `QSaveFile` atomic replace
and direct-write fallback disabled. Existing symbolic-link destinations and
native preference filenames are refused. Invalid data or selection leaves
an existing export file unchanged. This is atomic replacement of the export
artifact, not an atomic native-settings transaction.

Only portable identity/binding/definition data and optional scheme/context
metadata are exported. Daemon owners, provider signatures, revisions,
configuration baselines and keyboard-layout adapter state are excluded.

Installed discovery follows the audited native `kcmkeys/*.kksrc` locations
without recursively searching other directories. Invalid discovered schemes
remain visible with their issues. Discovery is bounded to 4,096 files and
adds a visible limit error if more are found. Caller-supplied temporary data
roots support isolated tests without changing the real XDG environment.

## Isolated verification and remaining acceptance

`tests/shortcut_scheme_test.cpp` authors native-shaped fixtures with the
independent KDE KConfig API, reads exports back with KConfig, and uses only
temporary files. Cases cover both scopes, legacy built-in metadata, disabled
values, punctuation/modifier keys, escaped identities and literal commands,
ordered alternatives and four chords, explicit context versioning, selection
unions, command migration and collisions, unknown/read-only reports without
truncation, malformed input and size limits, built-in discovery, symlink
refusal, and preservation of an existing export on failure. It also rejects
malformed programmatic documents before planning or serialization.

The codec worker performed the source audit and authored these tests without
running builds or test binaries; the integrating root task owns their actual
execution and reports recorded results. No live preference writes, command
registrations, reloads, shortcut invocations or recordings were used for this
work. Real-session import/Apply behavior, command-registration dependency
completion, application adoption and persistence across sign-in remain
unverified.

## Integrated validation — 17 October 2026

The integrating root built the final Debug tree and ran all **71 CTest suites**
successfully in **28.98 seconds**. The focused nine-suite shortcut/integration
run also passed. Fake-provider Aero captures cover both-scope import,
unresolved/read-only rows, selected context export, pending command definitions
and the custom-shortcut page. The final copy uses the same Custom Keyboard
Shortcuts page name throughout. Install staging under
`build/batch15-validation/stage` and a no-work rebuild passed; no live install
was performed.

Repeated read-only production probes find 205 applications, 12 registered
application components and 277 global actions. The custom backend recognizes
the installed KWin 6.6.6 embedded service and reports writable capability.
All 84 standard bindings match the installed SDK. Final repeated snapshots
match. The earlier global snapshots showed three Spectacle label changes;
every observed binding and default stayed identical. Of 26 monitored paths,
25 remained unchanged, none were added, and `kglobalshortcutsrc` changed.
Complete file attribution is unverified because only its pre-change hash was
retained. Application-file and service-cache inventories stayed unchanged.

Evidence is retained under ignored `build/batch15-validation`: final suite and
detail logs, capture tests/images, probe snapshots/summary/drift, fingerprint
comparison and install/build logs. No live settings write, registration,
cleanup, reload, invocation, command/application launch, recording/inhibition
or cache rebuild was used. The remaining coordination, multi-context removal,
edited-command adoption and real-session acceptance gaps remain unresolved.

[Batch 16](kde-batch16-shortcut-imports.md) extends this historical baseline with
retained dependency coordination and guarded whole-component removal across
contexts. Its separate 72-suite validation does not establish edited-command
execution adoption or full-module live acceptance.
