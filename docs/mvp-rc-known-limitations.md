# Local RC candidate: scope and known limitations

This is the intended scope of the candidate under preparation on 25 October
2026, not a qualification verdict. The final build identifier, installed payload
and observed scenario results belong in the qualification record. Do not mark
W1–W7 passed from this list.

## Required workflows remain open

Sound, Mouse/Touchpad, Desktop Background, Night Light, Power Options and Aero
Personalization retain their existing native editors and capability checks. They
are mandatory acceptance workflows, including observed effects, restoration and
the appropriate reopen/sign-in persistence checks. Missing support is a blocker
for the affected mandatory workflow.

The 25 October passive audio inventory found one physical playback sink, already
the default: Ryzen HD Audio Controller Analog Stereo. Headphones and HDMI outputs
were unavailable. W2's default-output **change** therefore cannot be qualified
until a real supported second output is connected and discovered. Selecting the
existing default is insufficient. The recorded baseline was muted at 0%; an
audible trial must be coordinated with the user. No audio setting was changed
during this inventory.

Input and Night Light require the audited KWin 6.6.6 Wayland runtime. Power Policy
requires the supported PowerDevil schema and available power-source/action
capabilities. Aero tuning requires the approved assets and supported running
effect. These checks remain intact. Saved-file readback and reload replies alone
do not establish physical or visible adoption.

## Optional actions available subject to existing guards

| Surface | Available scope |
| --- | --- |
| Display | Read/refresh display inventory, current modes, arrangement, scale and orientation; select an output for inspection |
| Network and Sharing Center | Read/refresh adapter, connection, address and reported connectivity information; navigation to Network Connections |
| Network Connections | Read adapters, access points and saved profiles; request a supported Wi-Fi scan; edit a supported **inactive** saved profile's name, autoconnect and IPv4/IPv6 preferences with existing authorization, version comparison and readback |
| Date and Time | Read date/time, zone and synchronization status; existing system-tool actions for clock/zone, supported synchronization toggles and fresh-measurement requests retain their normal authorization requirements |
| Ubuntu Package Preview | Read-only APT full-upgrade simulation using cached indexes with the existing audited-provider, output/time and cancellation safeguards |
| Secondary editors | Existing supported Font, Keyboard, title-bar action, Default Apps, shortcuts, Screen Locking, Day and Night, account and other editor subsets retain their current capability guards and limitations; modified providers require the extra RC-4 effect/restoration checks |

Inactive network preferences affect a later connection and must still be chosen
carefully. The application coordinates its own work; other KDE clients can race
and produce conflicts or uncertain outcomes. Scanning is a discovery request,
not a connection change. Reported Internet connectivity can be cached when
NetworkManager connectivity checks are disabled or unavailable.

## Optional actions withheld or unavailable

| Action | Reason and enforcement |
| --- | --- |
| Display resolution/refresh-mode, enabled state, scale, rotation, absolute/relative arrangement and Apply/preview | Withheld for this candidate until installed Keep, explicit Revert and timeout recovery are qualified. Editing controls show the reason; the production backend rejects direct preview calls before starting a helper. Discovery and the independent isolated recovery fixtures remain available. |
| Activate a saved connection, disconnect an adapter, connect to Wi-Fi/create a connection, or edit an active profile | Withheld for the same installed-recovery requirement. Controls and the settings session reject these routes; the production NetworkManager backend independently rejects them. Profile updates are classified again from fresh inventory, so an empty caller-supplied device path cannot make an active profile eligible. |
| Internet-time server editing | A user-local installation cannot provide the trusted root-owned helper and system authorization policy. The existing root-ownership/ancestor trust checks remain intact; copying policies beneath `.local` grants no authorization. Existing system-tool synchronization actions have a separate scope. |
| Aero login-theme options, shared login/lock background editing and privileged recovery | User-local helper/policy/approval files do not satisfy the existing root-owned installation and approved-byte checks. The page explains the separate system-install requirement and retains read-only information. This milestone does not install those components system-wide. |
| Programs and Features package mutations, Linux Update installation/checks and Installed Updates history on this Ubuntu target | The existing pages require their pacman/checkupdates/yay providers. They must report unavailable when those providers are missing. APT **mutations** remain follow-up work; the APT preview performs no installation, removal, download or index refresh and does not establish security/update status. |
| Unimplemented catalog actions and unsupported devices/provider versions | Existing availability reasons remain visible in search/navigation. Clearly labelled KDE launchers remain development bridges, not native parity claims. |

Recovery is not enabled by an environment variable, a search result, a sidebar,
restored history or an About dialog. Isolated network recovery tests explicitly
use a synthetic `org.example.*` service plus a fixture option; the real
`org.freedesktop.NetworkManager` service cannot use that exemption. Changing the
candidate's fixed withholding scope requires a new candidate and affected
qualification evidence.

## Feedback and visual checks

The title identifies the build. The `?` button beside search opens selectable
version, source revision and source-manifest details for feedback. Shell Apply,
Discard and Cancel preserve the established draft/conflict behavior; a running
mutation prevents application close. Sound retains immediate actions.

The passive display inventory reports a 1920 × 1080 eDP screen at scale 1.
Primary-page captures, readable error/action controls, tab focus, keyboard use and
scroll reachability at that actual scale remain to be checked on the integrated
candidate. A screenshot of a catalog or a successful offscreen render does not
pass all primary-page usability checks.

Application-file rollback does not undo preferences changed during use, and does
not own shared AeroQt or theme assets. Installation, sign-in/reboot and disruptive
live acceptance remain separate coordinated activities. Record failures before
starting the five-working-day usage trial.
