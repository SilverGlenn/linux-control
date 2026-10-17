# Batch 9: native Sound controls

Sound now uses one application-owned AudioSettings session, replacing blocking
tool calls and dialog-owned capture processes. Playback, Recording and
Applications use native Aero controls. Closing and reopening Sound preserves
an active operation and its confirmed outcome. The main window's work strip,
Details and exit guard include audio operations.

## Delivered controls

- Default playback and recording devices, endpoint volume and mute.
- Reported playback/recording ports and sound-card profiles.
- Per-channel levels for balance and multichannel adjustment.
- Playback and recording application volume, mute and device routing.
- External refresh, stable selection, disappeared-device handling and explicit
  unavailable states.

Changes take effect through explicit immediate actions; Close does not undo
them. There is no decorative global Apply/Revert. Volume is limited to 0–150%
in this editor; increasing it preserves the existing channel balance. Existing
higher external levels are not changed until the user explicitly sets a level.
Unavailable hardware choices remain visible with an explanation.

Sound events, automatic call ducking, peak meters/test sounds, custom channel
maps/formats, latency offsets and session policy remain open parity work.
The Sounds and Communications tabs explain their unavailable state without
offering fake writable preferences. Audio discovery neither plays nor captures
audio; the shell retains its existing optional Windows navigation sound.

## Provider and outcomes

The provider invokes pactl directly with fixed argument lists and its JSON
format. It reads server information, sinks, sources, cards, sink inputs and
source outputs, checking the server cookie before and after discovery. Parsing
validates object IDs, channel order, numeric volumes, advertised choices and
availability. Raw PulseAudio volume units preserve per-channel precision.

Every write gets a fresh preflight. Cookie, stable object identity and the
targeted field must still match the observed baseline. Stream routing also
checks the destination identity. PipeWire object serials protect against reused
indices; objects without sufficient lifetime identity stay read-only. Unrelated
fields are not overwritten. Profile changes may replace endpoints; verification
uses the surviving card identity and its reported active profile.

Successful outcomes require targeted readback. A timed-out/crashed tool, server
replacement, disappeared object or contradictory post-write state produces an
uncertain outcome rather than fabricated success. Further writes remain disabled
for that server cookie, including after Refresh. A verified replacement server
clears the lock. Failed commands with verified unchanged state remain retryable.
Errors and results remain visible across dialog recreation.

Each pactl invocation establishes a separate server connection. Preflight and
readback cannot atomically bind a command to the previous connection's cookie;
a server replacement or external edit in that interval can race with a write.
This provider does not claim transactions or atomic compare-and-swap. A future
long-lived libpulse adapter can reduce the server-generation race.

Process time and output are bounded. Only child environments receive the
explicit server selection that disables libpulse autospawn; existing explicit
server selection is honored. Connecting a systemd-managed audio socket can
still activate its service. Global audio configuration is not edited.

Provider references: [pactl command and JSON implementation](https://github.com/pulseaudio/pulseaudio/blob/master/src/utils/pactl.c),
[pactl manual](https://github.com/pulseaudio/pulseaudio/blob/master/man/pactl.1.xml.in),
[libpulse client configuration and autospawn](https://github.com/pulseaudio/pulseaudio/blob/master/src/pulse/client-conf.c).

## Validation

Unit fixtures cover target-specific conflicts, volume balance, channel order,
invalid choices, readback, delayed outcomes, stale tokens and uncertainty locks.
Provider tests execute a private fake pactl against temporary JSON state;
no fixture connects to the user's audio server. Native UI fixtures cover
default/volume/mute, ports/profiles/channel levels, playback and recording
routing, keyboard access, disappearance, failures and operation retention.

The manual audio_backend_probe reads only; it never selects a value, creates a
stream, plays a sound or captures audio. The installed server reports
PulseAudio compatibility on PipeWire 1.6.2, compatibility version 15.0.0. Real audio
writes and persistence/session-manager adoption remain runtime acceptance work.
Before/after fingerprints hash defaults, levels, mute, ports, profiles and
routes without storing application or device names.

Build, test, capture, probe and fingerprint evidence is retained under
build/batch9-validation. The final combined Debug build succeeded and all
**55 CTest suites passed** (27.84 seconds), including the subsequent input
integration. The successful combined log is copied there as
final-successful-tests.log. AeroQt playback, recording, application-routing and
properties captures were inspected, including light controls on a dark host.

The final read-only probe confirmed two playback endpoints, four recording
endpoints and three cards. Defaults, endpoint levels/mute/ports and card
profiles matched their before hashes. One existing playback stream ended
during validation; all surviving stream settings matched, and no new recording
stream appeared. No live audio writes, playback or capture were performed.
