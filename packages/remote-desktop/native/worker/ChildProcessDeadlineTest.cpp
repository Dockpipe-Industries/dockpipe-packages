#include "ChildProcess.h"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

using namespace dockpipe::desktop;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
int fixture(const std::string &mode)
{
    if (mode == "environment") {
        const auto value = std::getenv("TMPDIR");
        const auto line = std::string(value ? value : "<unset>") + "\n";
        assert(write(STDOUT_FILENO, line.data(), line.size()) == static_cast<ssize_t>(line.size()));
        return 0;
    }
    if (mode == "ignore-term")
        signal(SIGTERM, SIG_IGN);
    const auto ready = std::to_string(getpid()) + "\n";
    assert(write(STDOUT_FILENO, ready.data(), ready.size()) == static_cast<ssize_t>(ready.size()));
    if (mode == "exit")
        return 0;
    if (mode == "cooperate") {
        char byte;
        while (read(STDIN_FILENO, &byte, 1) > 0) {}
        return 0;
    }
    for (;;)
        pause();
}

std::unique_ptr<ChildProcess> readyChild(const std::string &image, const std::string &mode)
{
    auto child = ChildProcess::start({image, "--fixture", mode});
    RecordReader output;
    const auto deadline = Clock::now() + 2s;
    while (Clock::now() < deadline) {
        const auto result = output.poll(child->output(), 128, true);
        if (result == RecordReader::Outcome::Complete) {
            const auto bytes = output.take();
            assert(std::stol(std::string(bytes.begin(), bytes.end())) == child->pid());
            return child;
        }
        assert(result == RecordReader::Outcome::Pending);
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "Owned fixture never became ready");
    return {};
}

std::string childTemporaryDirectory(const std::string &image, const std::string &temporaryDirectory = {})
{
    auto child = ChildProcess::start({image, "--fixture", "environment"}, false, temporaryDirectory);
    RecordReader output;
    const auto deadline = Clock::now() + 2s;
    std::string result;
    while (Clock::now() < deadline) {
        const auto outcome = output.poll(child->output(), 8192, true);
        if (outcome == RecordReader::Outcome::Complete) {
            const auto bytes = output.take();
            result.assign(bytes.begin(), bytes.end());
            assert(!result.empty() && result.back() == '\n');
            result.pop_back();
            assert(child->stop(Clock::now() + 500ms));
            return result;
        }
        assert(outcome == RecordReader::Outcome::Pending);
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "Child environment fixture never responded");
    return {};
}

void temporaryDirectoryIsolation(const std::string &image)
{
    const auto inheritedValue = std::getenv("TMPDIR");
    const std::string inherited = inheritedValue ? inheritedValue : "<unset>";
    assert(childTemporaryDirectory(image) == inherited);
#ifdef __APPLE__
    char pattern[] = "/private/tmp/dockpipe-child-environment-XXXXXX";
#else
    char pattern[] = "/tmp/dockpipe-child-environment-XXXXXX";
#endif
    assert(mkdtemp(pattern));
    const auto directory = std::filesystem::canonical(pattern).string();
    assert(childTemporaryDirectory(image, directory) == directory);
    const auto current = std::getenv("TMPDIR");
    assert(std::string(current ? current : "<unset>") == inherited);
    assert(childTemporaryDirectory(image) == inherited); // Override did not leak.
    auto rejected = [&](const std::string &path) {
        try {
            ChildProcess::start({image, "--fixture", "environment"}, false, path);
            return false;
        } catch (...) {
            return true;
        }
    };
    assert(chmod(directory.c_str(), 0755) == 0);
    assert(rejected(directory));
    assert(chmod(directory.c_str(), 0700) == 0);
    const auto alias = directory + "-alias";
    assert(symlink(directory.c_str(), alias.c_str()) == 0);
    assert(rejected(alias));
    assert(rejected(directory + "/../" + std::filesystem::path(directory).filename().string()));
    assert(rejected("relative"));
    assert(unlink(alias.c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0);
}

void assertReaped(pid_t pid)
{
    int status = 0;
    assert(waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD);
}

void reapIfNeeded(pid_t pid)
{
    int status = 0;
    for (;;) {
        const auto waited = waitpid(pid, &status, 0);
        if (waited == pid || (waited < 0 && errno == ECHILD))
            return;
        assert(waited < 0 && errno == EINTR);
    }
}
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--fixture") == 0)
        return fixture(argv[2]);
    assert(argc == 1 && argv[0][0] == '/');
    const std::string image = argv[0];
    temporaryDirectoryIsolation(image);
    {
        auto child = readyChild(image, "cooperate");
        const auto pid = child->pid();
        const auto begin = Clock::now();
        assert(child->stop(begin + 500ms));
        assert(Clock::now() - begin < 600ms);
        assertReaped(pid);
        assert(child->stop()); // Idempotent known proof, no new wait.
        assert(child->input() == -1 && child->output() == -1);
    }
    {
        auto child = readyChild(image, "ignore-term");
        const auto pid = child->pid();
        const auto begin = Clock::now();
        assert(child->stop(begin + 700ms));
        const auto elapsed = Clock::now() - begin;
        assert(elapsed >= 150ms && elapsed < 800ms);
        assertReaped(pid);
    }
    {
        auto child = readyChild(image, "ignore-term");
        const auto begin = Clock::now();
        assert(child->stop()); // Preserve existing 2.5s graceful default.
        const auto elapsed = Clock::now() - begin;
        assert(elapsed >= 2400ms && elapsed < 3200ms);
        assertReaped(child->pid());
    }
    {
        auto child = readyChild(image, "exit");
        const auto deadline = Clock::now() + 1s;
        while (!child->exited() && Clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        assert(child->exited());
        siginfo_t information{};
        assert(waitid(P_PID, static_cast<id_t>(child->pid()), &information, WEXITED | WNOHANG | WNOWAIT) == 0);
        assert(information.si_pid == child->pid()); // exited() preserved the pin.
        assert(child->stop(Clock::now() + 500ms));
        assertReaped(child->pid());
    }
    {
        auto child = readyChild(image, "exit");
        reapIfNeeded(child->pid()); // Simulate an unexpected external reaper.
        assert(!child->stop(Clock::now() + 500ms)); // No pinned identity, no signal.
    }
    {
        auto child = readyChild(image, "ignore-term");
        const auto pid = child->pid();
        const auto begin = Clock::now();
        assert(!child->stop(Clock::time_point::min()));
        assert(Clock::now() - begin < 100ms);
        // Explicit failure cannot grant another 3s to a default call/destructor.
        assert(!child->stop());
        child.reset();
        assert(Clock::now() - begin < 100ms);
        reapIfNeeded(pid);
    }
    std::cout << "PASS bounded stop, legacy grace, PID pinning, expired-deadline teardown and no destructor renewal, and child-only TMPDIR\n";
}
