# Batch 12: native Default Programs

The native Aero page covers all twelve roles in the installed Plasma 6.6.6
`kcm_componentchooser`: web browser, e-mail client, file manager, terminal,
map, dialer, text editor, image viewer, music player, video player, PDF viewer,
and archive manager. It also provides searchable MIME names, descriptions,
extensions and protocol handlers, with a compatible installed-program picker.

## Primary-source audit

Audited the released [Plasma Workspace 6.6.6 source archive](https://download.kde.org/stable/plasma/6.6.6/plasma-workspace-6.6.6.tar.xz),
`kcms/componentchooser/kcm_componentchooser.cpp`, `applicationmodel.cpp`,
`componentchooser.cpp`, all twelve `components/*.cpp`, and
`terminal_settings.kcfg`. The KCM moved into plasma-workspace; it is absent
from the matching plasma-desktop archive. Its exact common MIME lists,
categories and recommended desktop IDs are represented by a typed whitelist
in `DefaultAppsTypes::roles()`.

The installed [KService 6.24 preferred-service implementation](https://invent.kde.org/frameworks/kservice/-/blob/v6.24.0/src/services/kapplicationtrader.cpp)
writes a singleton `Default Applications` list, prepends the selected ID in
`Added Associations`, and disables `filetypesrc` `EmbedSettings/embed-<mime>`.
The [KService MIME parser](https://invent.kde.org/frameworks/kservice/-/blob/v6.24.0/src/sycoca/kmimeassociations.cpp)
uses XDG config then applications directories, desktop-specific names before
generic names, processes low precedence first, removes before adding defaults,
and gives defaults preference 25 above additions. MIME aliases are canonicalized.
The native backend mirrors that saved-preference ordering, accepts historical
forced defaults as current values, and restricts new selections to declared
supported MIME types, including inheritance and scheme handlers.

The [KConfig 6.24 e-mail implementation](https://invent.kde.org/frameworks/kconfig/-/blob/v6.24.0/src/core/kemailsettings.cpp)
uses `emaildefaults`, `Defaults/Profile`, and active `PROFILE_<name>`
`EmailClient` and `TerminalClient`. Its constructor writes, so the read-only
backend deliberately does not instantiate it. KMail clears the legacy client
and terminal flag; other selections save their desktop ID and terminal flag.
The terminal saves `kdeglobals` `General/TerminalService` and the selected
installed entry's `TerminalApplication` command. No user command editor exists.

The [freedesktop MIME application specification](https://specifications.freedesktop.org/mime-apps/latest/)
is the baseline; KDE's additional preference and embedding behavior above is
also preserved. Higher-priority existing desktop-specific defaults are updated
for the affected MIME types. Alias entries for that same semantic MIME type
are merged into the canonical override. Unrelated associations, removals,
profiles, configuration keys and embedding preferences remain intact.

## Apply, defaults and safety

Selections and Restore defaults are staged in an application-owned
`DefaultAppsSettings` session. Destroying or recreating the page does not
cancel a pending job or lose choices. Restore defaults only selects KDE's
recommended desktop entries where installed and compatible; it does not
claim that unavailable applications have been installed or restore every MIME
association. A role changes only its common, supported, writable MIME types.

Inventory directly parses installed desktop entries in XDG data precedence,
including hidden-entry shadowing. It checks application type, a nonempty Exec,
TryExec availability, desktop identity and declared MIME/category support.
Exec strings are never executed. Inventory scans, policy parsing, staging,
atomic writes/fsync, rollback and readback run on one serialized QtConcurrent
worker. The worker captures immutable value inputs, never a backend or view
pointer. Pending reads coalesce; periodic polls skip outstanding work. Watchers
and result delivery remain on the application thread. No KCM, shell, application launcher,
KApplicationTrader query or KSycoca rebuild is invoked, including by the probe.

Apply re-inventories desktop identities and hashes every relevant association
and auxiliary config file, including system policy files. It refuses external
edits and immutable KDE keys/groups. Writability is scoped to individual roles
and MIME types so a protected mail profile does not disable browser changes.
The complete multi-file plan is prepared in private temporary copies before
any write. Original bytes, membership and permissions are checked before
replacement. Directory traversal and file replacement use Linux `openat`,
`O_NOFOLLOW` and `renameat`; linked, hard-linked and world-writable targets
are refused. Ordinary group-writable user settings (directories 0775 and
files 0664) remain editable, with permissions preserved by replacement. If a later file fails, completed writes are rolled back only if
still equal to the just-written bytes. Failed rollback reports a partial
result, preserves staged choices and requires Revert before another Apply.

These are per-file atomic replacements, not a filesystem-wide atomic
transaction. There is no crash-recovery journal and no claim that all files
become visible simultaneously. Readback checks the exact bytes prepared and
validated saved preferences. Success means **saved and verified**. Running
applications and service-cache adoption are explicitly unverified. The backend
never forces a live cache rebuild; later KDE cache refresh/application restart
may be required. When no valid saved preference exists, the UI says System
selection rather than inventing a preferred program from inventory order.

## Verification

`default_apps_settings_test` exercises staging, recommendation defaults,
unsupported selections, external conflicts, stale replies, uncertain results,
and busy session completion. `default_apps_backend_test` uses only private
config/data roots and synthetic desktop entries to verify multi-file effects,
policy, symlinks, external edits, desktop identity changes, hidden shadowing,
KMail handling and preservation of unrelated values. Held worker scans and
commits verify an event-loop heartbeat; deletion during a held read verifies
nonblocking ownership. Additional regressions cover missing desktop IDs before
valid defaults, MIME aliases and rollback after an external edit during Apply.
`default_apps_page_test` covers native role controls, MIME extension search,
compatible pickers, protected types and page recreation during Apply.
`default_apps_probe` is a compiled read-only production diagnostic and reports
saved-file state separately from uninspected service-cache adoption.

No live preference writes, app launches or service-cache rebuilds are part of
verification. Optional page capture uses the existing Aero stylesheet through
`LINUX_CONTROL_UI_CAPTURE_DIR`.

## Recorded validation — 17 October 2026

The final Debug build and staged DESTDIR install passed. All **61 CTest suites
passed** in **28.46 seconds**. The backend suite includes ordinary group-writable
user configuration, retained permissions, rejected world-writable targets,
missing application IDs in preference ordering, held-worker responsiveness,
owner destruction, aliases and rollback after an external edit.

The read-only production probe twice found **205 installed desktop entries**,
**1,018 MIME names** and **69 valid saved MIME preferences** with stable results.
The provider is writable on this installation without permission changes.
Roles without compatible installed applications remain disabled; no missing
application was installed. Running service-cache and application adoption remain
unverified. The native Aero page capture was inspected.

Build/test logs, probe JSON, capture and staged install are recorded under
`build/batch12-validation`. Hashes of **31** keyboard/default-program configuration
paths remained unchanged after all probes and tests. No live preference writes,
application launches or service-cache rebuilds were performed.
