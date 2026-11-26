#include "ChildProcess.h"
#include <cassert>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

using namespace dockpipe::desktop;
using namespace std::chrono_literals;

int main()
{
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        sigset_t original;
        assert(pthread_sigmask(SIG_SETMASK, nullptr, &original) == 0);
        int descriptors[2];
        assert(pipe(descriptors) == 0);
        close(descriptors[0]);
        const std::uint8_t byte = 1;
        assert(!writeBytes(descriptors[1], &byte, 1, std::chrono::steady_clock::now() + 100ms));
        close(descriptors[1]);
        struct sigaction action{};
        assert(sigaction(SIGPIPE, nullptr, &action) == 0 && action.sa_handler == SIG_DFL);
        sigset_t after;
        assert(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
        assert(sigismember(&after, SIGPIPE) == sigismember(&original, SIGPIPE));
        sigset_t pending;
        assert(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 0);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "PASS broken host pipe preserves default SIGPIPE disposition and calling-thread mask\n";
}
