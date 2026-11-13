#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/Subprocess.h"

using adm::core::Subprocess;

namespace {

// A child that starts a grandchild of its own, both running ~30 s; the grandchild inherits the
// output pipe, so Run reads until it, too, has gone
std::vector<std::string> ChildWithAGrandchild()
{
#ifdef _WIN32
    return {"cmd", "/c", "ping -n 30 127.0.0.1 >nul"}; // silent: stderr still holds the pipe
#else
    return {"sh", "-c", "sleep 30; true"};
#endif
}

} // namespace

TEST(SubprocessTest, RunsAndReturnsTheExitCode)
{
#ifdef _WIN32
    const std::vector<std::string> args = {"cmd", "/c", "echo hello& exit 3"};
#else
    const std::vector<std::string> args = {"sh", "-c", "echo hello; exit 3"};
#endif
    Subprocess p;
    std::string out;
    EXPECT_EQ(p.Run(args, [&](const std::string& l) { out += l; }), 3);
    EXPECT_NE(out.find("hello"), std::string::npos);
}

// EXO-08: a cancel ends the child and what it started, so Run returns at once instead of
// waiting on a grandchild that holds the pipe
TEST(SubprocessTest, CancelEndsTheChildAndItsChildren)
{
    Subprocess p;
    std::thread canceller([&p] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        p.Cancel();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = p.Run(ChildWithAGrandchild(), [](const std::string&) {});
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    canceller.join();
    EXPECT_EQ(rc, 1);
    EXPECT_LT(seconds, 5.0);
}
