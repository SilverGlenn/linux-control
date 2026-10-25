#pragma once

#include <string>

namespace release {
// This guard has no Qt/provider dependencies and outlives QApplication/jobs.
class InstallationLock {
public:
    InstallationLock() = default;
    ~InstallationLock();
    InstallationLock(const InstallationLock &) = delete;
    InstallationLock &operator=(const InstallationLock &) = delete;

    // Private fixture prefixes can exercise the identical protocol directly.
    bool acquireShared(const std::string &prefix, std::string &error);
    // Development/staged binaries bypass the guard; the installed executable
    // must use the current user's ~/.local prefix and canonical journal.
    bool acquireForInstalledExecutable(const std::string &compiledPrefix, std::string &error);
    bool isHeld() const { return m_descriptor >= 0; }

private:
    int m_descriptor = -1;
};
}
