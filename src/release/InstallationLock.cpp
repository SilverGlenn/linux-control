#include "InstallationLock.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;

bool ancestors(const fs::path &path, std::string &error)
{
    fs::path current;
    for (const auto &part : path) {
        current /= part;
        struct stat image {};
        if (::lstat(current.c_str(), &image) != 0) {
            if (errno == ENOENT)
                continue;
            error = "Cannot inspect installation lock ancestor: " + current.string();
            return false;
        }
        if (!S_ISDIR(image.st_mode)) {
            error = "Symlink/non-directory installation lock ancestor refused: " + current.string();
            return false;
        }
    }
    return true;
}

bool valid(const fs::path &directory, const fs::path &path, int descriptor, std::string &error)
{
    struct stat dir {}, file {}, named {};
    if (::lstat(directory.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode)
        || dir.st_uid != ::getuid() || (dir.st_mode & 07777) != 0700) {
        error = "Installation lock directory must be owned by this user with mode 0700: " + directory.string();
        return false;
    }
    if (::fstat(descriptor, &file) != 0 || ::lstat(path.c_str(), &named) != 0
        || !S_ISREG(file.st_mode) || !S_ISREG(named.st_mode) || file.st_uid != ::getuid()
        || (file.st_mode & 07777) != 0600 || file.st_nlink != 1
        || file.st_dev != named.st_dev || file.st_ino != named.st_ino) {
        error = "Installation lock must be a private mode 0600 ordinary file with stable identity: " + path.string();
        return false;
    }
    return true;
}
}

namespace release {
InstallationLock::~InstallationLock()
{
    if (m_descriptor >= 0)
        ::close(m_descriptor); // Never unlink the inode shared with waiting processes.
}

bool InstallationLock::acquireShared(const std::string &prefix, std::string &error)
{
    if (isHeld()) {
        error = "Installation lock is already held";
        return false;
    }
    fs::path destination(prefix);
    if (!destination.is_absolute() || destination != destination.lexically_normal()) {
        error = "Installation prefix must be an absolute normalized path";
        return false;
    }
    const fs::path directory = destination.parent_path() / ".control-panel-rc-deployment";
    const fs::path path = directory / "application.lock";
    if (!ancestors(destination, error) || !ancestors(directory, error))
        return false;
    std::error_code failure;
    fs::create_directories(directory.parent_path(), failure);
    if (failure || (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)) {
        error = "Cannot create installation lock directory: " + directory.string();
        return false;
    }
    // Refuse untrusted directory modes before creating anything within it.
    struct stat dir {};
    if (::lstat(directory.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode)
        || dir.st_uid != ::getuid() || (dir.st_mode & 07777) != 0700) {
        error = "Installation lock directory must be owned by this user with mode 0700: " + directory.string();
        return false;
    }
    const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (descriptor < 0) {
        error = "Cannot safely open installation lock: " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    bool okay = valid(directory, path, descriptor, error);
    if (okay && ::flock(descriptor, LOCK_SH | LOCK_NB) != 0) {
        error = "A Control Panel deployment is active; wait for it to finish before launching.";
        okay = false;
    }
    if (okay)
        okay = ancestors(directory, error) && valid(directory, path, descriptor, error);
    struct stat pending {};
    if (okay && ::lstat((directory / "pending.json").c_str(), &pending) == 0) {
        error = "An interrupted Control Panel deployment requires independent recovery before launch. "
                "Run the trusted rc_bundle.py recover --state " + directory.string()
                + " command with Control Panel closed; settings providers were not started.";
        okay = false;
    } else if (okay && errno != ENOENT) {
        error = "Cannot inspect the pending deployment journal; settings providers were not started.";
        okay = false;
    }
    if (!okay) {
        ::close(descriptor);
        return false;
    }
    m_descriptor = descriptor;
    return true;
}

bool InstallationLock::acquireForInstalledExecutable(const std::string &compiledPrefix, std::string &error)
{
    std::vector<char> executable(65536);
    const auto size = ::readlink("/proc/self/exe", executable.data(), executable.size());
    if (size < 0 || static_cast<size_t>(size) >= executable.size()) {
        error = "Cannot identify the running Control Panel executable before provider startup.";
        return false;
    }
    const fs::path prefix(compiledPrefix);
    std::string runningName(executable.data(), static_cast<size_t>(size));
    // A concurrent atomic replacement marks the already-open executable as
    // deleted. It still belongs to the installed app and must take the lock.
    const std::string deletedSuffix = " (deleted)";
    if (runningName.size() >= deletedSuffix.size()
        && runningName.compare(runningName.size() - deletedSuffix.size(), deletedSuffix.size(), deletedSuffix) == 0)
        runningName.resize(runningName.size() - deletedSuffix.size());
    const fs::path running(runningName);
    if (running != prefix / "bin/control")
        return true; // Development/test/staged executables must not touch the real journal.
    struct passwd account {}, *found = nullptr;
    std::vector<char> buffer(65536);
    if (::getpwuid_r(::getuid(), &account, buffer.data(), buffer.size(), &found) != 0
        || !found || prefix != fs::path(account.pw_dir) / ".local") {
        error = "Installed Control Panel is limited to the current user's ~/.local prefix.";
        return false;
    }
    return acquireShared(compiledPrefix, error);
}
}
