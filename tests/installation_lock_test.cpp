#include "release/InstallationLock.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
static void check(bool okay, const std::string &message)
{
    if (!okay)
        throw std::runtime_error(message);
}

static int pythonLock(const std::string &tools, const fs::path &prefix, bool exclusive)
{
    const pid_t process = ::fork();
    check(process >= 0, "fork failed");
    if (process == 0) {
        const char *script =
            "import sys;sys.path.insert(0,sys.argv[1]);from install_lock import installation_lock,Refusal\n"
            "try:\n with installation_lock(sys.argv[2],exclusive=sys.argv[3]=='yes'): pass\n"
            "except Refusal: sys.exit(3)\n";
        ::execlp("python3", "python3", "-c", script, tools.c_str(), prefix.c_str(),
                 exclusive ? "yes" : "no", static_cast<char *>(nullptr));
        ::_exit(100);
    }
    int status = 0;
    check(::waitpid(process, &status, 0) == process && WIFEXITED(status), "Python lock fixture failed");
    return WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "Usage: installation_lock_test <tools-directory>\n";
        return 2;
    }
    char pattern[] = "/tmp/control-installation-lock-XXXXXX";
    char *temporary = ::mkdtemp(pattern);
    if (!temporary)
        return 1;
    const fs::path root(temporary), prefix = root / "home/.local";
    const fs::path state = prefix.parent_path() / ".control-panel-rc-deployment";
    const fs::path lock = state / "application.lock";
    try {
        std::string error;
        {
            release::InstallationLock development;
            check(development.acquireForInstalledExecutable((root / "development-prefix").string(), error), error);
            check(!development.isHeld() && !fs::exists(state), "Development binary touched the journal");
        }
        {
            release::InstallationLock first, second;
            check(first.acquireShared(prefix.string(), error), error);
            check(second.acquireShared(prefix.string(), error), "Two apps cannot share the lock: " + error);
            check(pythonLock(argv[1], prefix, false) == 0, "Python shared lock did not interoperate");
            check(pythonLock(argv[1], prefix, true) == 3, "Python deployment bypassed running apps");
        }
        struct stat original {};
        check(::lstat(lock.c_str(), &original) == 0, "Lock inode was removed on app exit");
        check(pythonLock(argv[1], prefix, true) == 0, "App exit did not release lock");
        struct stat after {};
        check(::lstat(lock.c_str(), &after) == 0 && original.st_ino == after.st_ino,
              "Python replaced the shared lock inode");

        // A Python writer takes the exclusive lock, signals readiness, then
        // waits on a pipe. C++ startup must fail before it can start providers.
        int ready[2], finish[2];
        check(::pipe(ready) == 0 && ::pipe(finish) == 0, "pipe failed");
        const pid_t writer = ::fork();
        check(writer >= 0, "fork writer failed");
        if (writer == 0) {
            ::dup2(ready[1], STDOUT_FILENO);
            ::dup2(finish[0], STDIN_FILENO);
            ::close(ready[0]); ::close(ready[1]); ::close(finish[0]); ::close(finish[1]);
            const char *script =
                "import sys;sys.path.insert(0,sys.argv[1]);from install_lock import installation_lock\n"
                "with installation_lock(sys.argv[2]):\n print('R',flush=True);sys.stdin.read(1)\n";
            ::execlp("python3", "python3", "-c", script, argv[1], prefix.c_str(), static_cast<char *>(nullptr));
            ::_exit(100);
        }
        ::close(ready[1]); ::close(finish[0]);
        char signal;
        const bool writerReady = ::read(ready[0], &signal, 1) == 1 && signal == 'R';
        bool refused = false;
        if (writerReady) {
            release::InstallationLock contender;
            refused = !contender.acquireShared(prefix.string(), error) && error.find("deployment is active") != std::string::npos;
        }
        ::close(ready[0]); ::close(finish[1]);
        int status;
        check(::waitpid(writer, &status, 0) == writer && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Python writer failed");
        check(writerReady && refused, "C++ startup bypassed Python deployment lock: " + error);

        std::ofstream(state / "pending.json") << "{interrupted fixture}";
        {
            release::InstallationLock interrupted;
            check(!interrupted.acquireShared(prefix.string(), error)
                  && error.find("recover --state") != std::string::npos,
                  "Pending transaction did not refuse startup with repair guidance");
        }
        fs::remove(state / "pending.json");
        ::chmod(lock.c_str(), 0644);
        {
            release::InstallationLock unsafe;
            check(!unsafe.acquireShared(prefix.string(), error), "Unsafe lock mode accepted");
        }
        ::chmod(lock.c_str(), 0600);
        fs::remove(lock);
        std::ofstream(root / "outside") << "untouched";
        fs::create_symlink(root / "outside", lock);
        {
            release::InstallationLock unsafe;
            check(!unsafe.acquireShared(prefix.string(), error), "Symlink lock accepted");
        }
        fs::remove(lock);
        fs::create_hard_link(root / "outside", lock);
        ::chmod(lock.c_str(), 0600);
        {
            release::InstallationLock unsafe;
            check(!unsafe.acquireShared(prefix.string(), error), "Hardlinked lock accepted");
        }
        fs::remove(lock);
        ::chmod(state.c_str(), 0755);
        {
            release::InstallationLock unsafe;
            check(!unsafe.acquireShared(prefix.string(), error), "Unsafe directory mode accepted");
        }
        fs::remove_all(root);
        std::cout << "Installation lock process and protocol fixtures passed\n";
        return 0;
    } catch (const std::exception &failure) {
        fs::remove_all(root);
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
