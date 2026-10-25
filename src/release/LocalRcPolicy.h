#pragma once

// Fixed scope of this local candidate. Qualifying recovery requires a new
// candidate; no environment variable or UI route enables these actions.
namespace release::localRc {
inline constexpr const char *displayEditsWithheldReason()
{
    return "Display changes are unavailable in this local candidate until installed Keep, Revert and timeout recovery have been qualified. Display information can still be read; use KDE System Settings to make changes.";
}
inline constexpr const char *connectivityEditsWithheldReason()
{
    return "Connection changes and active-profile edits are unavailable in this local candidate until installed Keep, Revert and timeout recovery have been qualified. Inactive saved profiles can still be edited; use KDE System Settings to change connections.";
}
}
