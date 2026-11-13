#include <gtest/gtest.h>
#include <httplib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include "core/PeekabooHttpServer.h"

namespace {

constexpr int kTestPort = 17474;

bool WaitUntilReady(httplib::Client& client)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (auto response = client.Get("/api/status"); response && response->status == 200)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST(PeekabooHttpServerTest, LoadEndpointGatesOriginAndContentType)
{
    adm::core::PeekabooHttpServer server(kTestPort);
    server.Start();

    httplib::Client client("127.0.0.1", kTestPort);
    ASSERT_TRUE(WaitUntilReady(client));

    const std::string body = R"({"fileUrl":"https://files.example/demo.adf"})";

    // A cross-origin simple POST executes server-side even though CORS blocks the response
    // read, so an unknown web origin must be rejected outright.
    httplib::Headers evilOrigin = {{"Origin", "https://evil.example"}};
    auto rejected = client.Post("/api/load", evilOrigin, body, "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 403);
    EXPECT_FALSE(server.PopPendingLoad().has_value());

    // text/plain is the CORS "simple request" shape a hostile page would use.
    auto wrongType = client.Post("/api/load", body, "text/plain");
    ASSERT_TRUE(wrongType);
    EXPECT_EQ(wrongType->status, 415);
    EXPECT_FALSE(server.PopPendingLoad().has_value());

    // The extension's background service worker.
    httplib::Headers extensionOrigin = {{"Origin", "chrome-extension://abcdefghijklmnop"}};
    auto fromExtension = client.Post("/api/load", extensionOrigin, body, "application/json");
    ASSERT_TRUE(fromExtension);
    EXPECT_EQ(fromExtension->status, 200);
    EXPECT_TRUE(server.PopPendingLoad().has_value());

    // Content scripts on the allow-listed demoscene sites.
    httplib::Headers demozooOrigin = {{"Origin", "https://demozoo.org"}};
    auto fromDemozoo = client.Post("/api/load", demozooOrigin, body, "application/json");
    ASSERT_TRUE(fromDemozoo);
    EXPECT_EQ(fromDemozoo->status, 200);
    EXPECT_TRUE(server.PopPendingLoad().has_value());

    // Local tools send no Origin at all.
    auto fromLocal = client.Post("/api/load", body, "application/json");
    ASSERT_TRUE(fromLocal);
    EXPECT_EQ(fromLocal->status, 200);
    auto ev = server.PopPendingLoad();
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->filename, "demo.adf");

    server.Stop();
}

TEST(PeekabooHttpServerTest, AcceptsOnlyValidLocalRunRequests)
{
    adm::core::PeekabooHttpServer server(kTestPort);
    server.Start();

    httplib::Client client("127.0.0.1", kTestPort);
    ASSERT_TRUE(WaitUntilReady(client));

    auto missingProgram = client.Post("/api/run", R"({})", "application/json");
    ASSERT_TRUE(missingProgram);
    EXPECT_EQ(missingProgram->status, 400);

    auto invalidFocus =
        client.Post("/api/run", R"({"program":"C:\\demo.exe","focus":"yes"})", "application/json");
    ASSERT_TRUE(invalidFocus);
    EXPECT_EQ(invalidFocus->status, 400);

    httplib::Headers browserHeaders = {{"Origin", "https://example.com"}};
    auto browserRequest = client.Post("/api/run", browserHeaders, R"({"program":"C:\\demo.exe"})",
                                      "application/json");
    ASSERT_TRUE(browserRequest);
    EXPECT_EQ(browserRequest->status, 403);

    const auto program = std::filesystem::temp_directory_path() / "peekaboo-http-test.exe";
    {
        std::ofstream output(program, std::ios::binary);
        output << "HUNK";
    }

    const std::string body = std::string{"{\"program\":"} +
                             nlohmann::json(program.string()).dump() + ",\"focus\":false}";
    auto accepted = client.Post("/api/run", body, "application/json");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->status, 202);

    auto request = server.PopPendingRun();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->program, program.string());
    EXPECT_FALSE(request->focus);
    EXPECT_FALSE(server.PopPendingRun().has_value());

    std::error_code ec;
    std::filesystem::remove(program, ec);
    server.Stop();
}
