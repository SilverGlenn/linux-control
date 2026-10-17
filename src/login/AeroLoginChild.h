#pragma once
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace AeroLoginChild {
// Only supervise the unprivileged decoder created by this helper. A pipe EOF
// does not establish process termination, and CPU limits do not bound sleep.
inline bool finish(pid_t pid,int remainingMilliseconds,bool inputSucceeded,int &status)
{
    using Clock=std::chrono::steady_clock;
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(0,remainingMilliseconds));
    while(inputSucceeded&&Clock::now()<deadline){
        const auto ended=::waitpid(pid,&status,WNOHANG);
        if(ended==pid)return WIFEXITED(status)&&WEXITSTATUS(status)==0;
        if(ended<0&&errno!=EINTR)return false;
        ::poll(nullptr,0,20);
    }
    ::kill(pid,SIGKILL);
    const auto cleanup=Clock::now()+std::chrono::seconds(1);
    // Reaping cannot become an unbounded root-side wait either. A pending
    // SIGKILL remains in effect if init must eventually reap a wedged decoder.
    while(Clock::now()<cleanup){
        const auto ended=::waitpid(pid,&status,WNOHANG);
        if(ended==pid||ended<0&&errno!=EINTR)break;
        ::poll(nullptr,0,20);
    }
    return false;
}
}
