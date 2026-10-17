# Batch 8: native Network and Internet

Everyday network actions now open native Windows 7 views. Network and Sharing
Center uses NetworkManager's reported state; Network Connections supplies
adapter, nearby Wi-Fi and saved-connection lists plus IP/DNS property editors.
The existing Aero-only appearance policy is unchanged.

## Supported scope

- Read adapter state, active profiles and IPv4/IPv6 addresses. Unsupported
  device types remain visible and read-only.
- Scan on a managed Wi-Fi adapter; connect to an advertised Open,
  WPA-Personal or WPA3-Personal network with a masked password dialog.
- Activate a compatible saved Ethernet/Wi-Fi profile or disconnect a managed
  Ethernet/Wi-Fi device.
- Stage a saved connection's name, autoconnect, automatic/manual IPv4 and IPv6
  addresses, gateway, DNS servers and automatic-DNS preference.
- Keep or revert connectivity changes with independent daemon recovery;
  retain scans, pending edits and operation results across page navigation.

VPN, proxy, hotspot, cellular, hidden/enterprise/WEP/OWE Wi-Fi, custom route
editing, radio switches, profile creation outside Wi-Fi discovery, profile
deletion and sharing are not implemented. Unsupported IP methods, controller
profiles and unsupported security remain read-only. Existing unrelated settings,
including routes, MTU, security, zone and proxy data, are retained when editing
supported properties. This is scoped coverage, not full KDE networking parity.

## Provider and credentials

The backend uses Qt D-Bus directly against the system NetworkManager service;
it does not launch KDE's editor or invoke a shell. The current target is
NetworkManager **1.54.3**. Capability discovery reads permissions and protocol
support without scanning or creating a checkpoint. Wi-Fi disabled by software
or hardware disables scan/connect controls. All requests are validated again
in the backend, using service ownership, object identity and current state.

Numeric CIDR addresses, prefix lengths, address families, gateway and DNS
values are validated before dispatch. IPv6 spellings are normalized for
readback comparison. Unknown D-Bus settings retain their wire representation;
only supported changed properties are replaced. Equality excludes the audited
GetSettings-injected timestamp and Wi-Fi seen-BSSID history, which are not
editable configuration. Applied verification compares supported preferences;
runtime-conflict checks still compare the complete applied baseline.

Saved passwords are never requested with GetSecrets. Updates omit secrets and
rely on the audited 1.54.3 daemon's preservation of existing system/agent secrets
when replacement settings contain none. Keep uses an empty settings map to
persist the daemon's current connection without supplying credentials again.
New Wi-Fi passwords travel only in the D-Bus request to NetworkManager; they are
not stored in application preferences, command arguments or diagnostics. Secret
copies are cleared after dispatch; this is not a guarantee of locked or securely
erased process memory. Saved-profile activation uses the system secret agent.

References: [NetworkManager D-Bus API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html),
[connection Update2 API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.Settings.Connection.html),
[1.54.3 settings implementation](https://github.com/NetworkManager/NetworkManager/blob/1.54.3/src/core/settings/nm-settings-connection.c),
[1.54.3 checkpoint implementation](https://github.com/NetworkManager/NetworkManager/blob/1.54.3/src/core/nm-checkpoint.c),
[1.54.3 checkpoint device validation](https://github.com/NetworkManager/NetworkManager/blob/1.54.3/src/core/nm-checkpoint-manager.c),
[IPv4 properties](https://networkmanager.dev/docs/api/latest/settings-ipv4.html),
[IPv6 properties](https://networkmanager.dev/docs/api/latest/settings-ipv6.html),
[wireless security properties](https://networkmanager.dev/docs/api/latest/settings-802-11-wireless-security.html).

## Apply and recovery

One application-owned NetworkSettings session serves both pages. Editing
stages values; navigation offers the shared Apply/Discard/Cancel protection.
Busy network mutations protect exit. Scans can continue during navigation and
remain visible in background work. Confirmation controls are present on both
views so a pending change can be resolved wherever it was started.

Inactive saved-profile edits use Update2 with version-id compare-and-swap,
TO_DISK, BLOCK_AUTOCONNECT and NO_REAPPLY. They save properties without
activating the connection or claiming that the current connection changed.
NetworkManager checkpoints back up originally active profiles, so they are
not presented as protection for inactive-profile edits.

Connectivity changes first create a 60-second daemon checkpoint for managed
Ethernet/Wi-Fi devices without replacing an existing checkpoint. Loopback and
Wi-Fi Direct discovery devices remain outside the protected device list;
unsupported controller relationships cannot be edited. Authorization for recovery
occurs before dispatch. Subsequent live mutations require current granted
permissions and request noninteractive D-Bus authorization. The audited daemon's
Update2 authorization can still allow interaction internally: a policy change
between permission read and dispatch is not atomic with the operation. A prompt
or late method can outlive the recovery window; an uncertain reply locks further
writes and leaves daemon recovery intact without claiming guaranteed restoration.
Active-profile property edits are initially in memory and explicitly activated;
saved and device-applied preferences must match before confirmation appears.
The visible countdown reserves read/write time for Keep. Revert or expiry
requests restoration and checks the saved and applied configuration again.
NetworkManager's timer continues independently if the application dies.
Read-only restoration checks retry inconsistent transition snapshots within a
bounded grace period; inconsistent reads never authorize a new write.

New Wi-Fi profiles are volatile until Keep saves them. Recovery removes only
the owned volatile connection through deactivation. The global
DELETE_NEW_CONNECTIONS checkpoint flag is never used, so unrelated newly
created profiles are not swept away. A kept connection is not bound to the
application's lifetime.

Snapshot reads bracket GetSettings with VersionId reads, and active devices
include GetAppliedConnection settings/version. External saved or runtime edits
prevent stale Keep/Revert writes. When a definitive external conflict is found
with no mutation pending, the backend cancels only its own verified checkpoint
and verifies that the newer state remains; it reports neither kept nor restored.

A timed-out write may still finish inside NetworkManager. Such outcomes disable
further writes in that backend session until the daemon owner changes; Refresh
does not pretend to cancel the original request. The backend leaves daemon
recovery intact instead of issuing speculative compensating writes. Denied or
canceled authorization is reread before an unchanged outcome is reported.
Untrusted daemon error bodies are suppressed because they may echo credentials.

NetworkManager checkpoints are not cross-client transactions. An undetected
external edit, or an application failure before conflict handling, can still
be affected by the daemon's restoration. A readback is evidence of observed
configuration, not proof of uninterrupted access or successful Internet traffic.
No fresh connectivity HTTP probe is run. Disabled/unavailable connectivity
checks produce a cached-state warning even when the service reports full access.

## Validation

Tests use typed models, injected sessions, offscreen native pages and a private
fake NetworkManager on a private D-Bus. They cover wire signatures, preservation
of untouched settings and credentials, version conflicts, applied runtime
changes, failed/denied/canceled authorization, late replies, checkpoint expiry,
volatile profile recovery, inactive-save flags, readback, navigation and exit
protection. External-conflict tests wait beyond the original recovery deadline
to check that canceling the owned timer preserves the newer configuration.

The manual network_backend_probe performs reads only: it does not scan,
retrieve stored passwords, create a checkpoint or mutate a connection. Live
write paths have not been exercised against the user's connection. A before/
after fingerprint records hashed profile and applied settings plus device and
radio state without storing names or credentials.

Build, test, probe, fingerprint and capture evidence is retained under
build/batch8-validation in the development checkout.

The final Debug build succeeded and all **49 CTest suites passed** (21.07
seconds). AeroQt offscreen captures of the sharing overview, saved-connection
properties and masked password dialog were inspected. The native page suite
also checks loopback/Wi-Fi Direct exclusion from the active-network overview,
unsupported controls, input stability and confirmation on both views.

The compiled read-only probe successfully read NetworkManager 1.54.3, three
devices and three saved profiles, with checkpoint support available and no
capability errors. The service's connectivity checks were both unavailable and
disabled; the UI therefore warns about cached connectivity. A separate
read-only comparison established the saved/applied seen-BSSID metadata
difference on this installation, with regression coverage for that case.

Before/after hashes and device/radio state matched exactly. No live connection,
profile, credential or radio changes were made. Packaging passed a DESTDIR
staging install under the evidence directory; no live installation was done.
Real connect/disconnect, credential-agent interaction, policy authorization and
recovery on this machine remain explicit runtime acceptance work.
