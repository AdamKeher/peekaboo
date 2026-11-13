#include <gtest/gtest.h>
#include <httplib.h>
#include <chrono>
#include <string>
#include <thread>
#include "core/McpServer.h"

// A page in a browser (a local HTML file sends Origin "null") may use the MCP API with the
// token, and only with it: the preflight is answered for anyone, but only an authorized
// response is readable cross-origin.

namespace {

constexpr int kTestPort = 17475;
const std::string kToken = "test-token";

bool WaitUntilReady(httplib::Client& client)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (auto response = client.Get("/tools"); response && response->status == 401)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST(McpServerCorsTest, TokenHoldingPageCanReadAndOthersCannot)
{
    adm::core::McpServer server;
    server.SetManifestProvider([] { return nlohmann::json::array({{{"name", "x"}}}); });
    server.Start(kTestPort, kToken);

    httplib::Client client("127.0.0.1", kTestPort);
    ASSERT_TRUE(WaitUntilReady(client));

    // The preflight: no token (a browser never sends one), answered for the page's origin,
    // with the token header allowed and Chrome's private-network opt-in when asked for.
    httplib::Headers pre = {{"Origin", "null"},
                            {"Access-Control-Request-Method", "POST"},
                            {"Access-Control-Request-Headers", "content-type,x-peekaboo-token"},
                            {"Access-Control-Request-Private-Network", "true"}};
    auto options = client.Options("/tool", pre);
    ASSERT_TRUE(options);
    EXPECT_EQ(options->status, 204);
    EXPECT_EQ(options->get_header_value("Access-Control-Allow-Origin"), "null");
    EXPECT_NE(options->get_header_value("Access-Control-Allow-Headers").find("X-Peekaboo-Token"),
              std::string::npos);
    EXPECT_EQ(options->get_header_value("Access-Control-Allow-Private-Network"), "true");

    // With the token: readable from the page.
    httplib::Headers withToken = {{"Origin", "null"}, {"X-Peekaboo-Token", kToken}};
    auto ok = client.Get("/tools", withToken);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(ok->get_header_value("Access-Control-Allow-Origin"), "null");

    // Without it, or with the wrong one: refused, and not readable cross-origin either.
    httplib::Headers wrong = {{"Origin", "https://evil.example"}, {"X-Peekaboo-Token", "nope"}};
    auto refused = client.Get("/tools", wrong);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 401);
    EXPECT_FALSE(refused->has_header("Access-Control-Allow-Origin"));
    auto refusedPost = client.Post("/tool", wrong, R"({"name":"x"})", "application/json");
    ASSERT_TRUE(refusedPost);
    EXPECT_EQ(refusedPost->status, 401);
    EXPECT_FALSE(refusedPost->has_header("Access-Control-Allow-Origin"));

    // A local tool sends no Origin and gets no CORS headers - as before.
    httplib::Headers local = {{"X-Peekaboo-Token", kToken}};
    auto plain = client.Get("/tools", local);
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->status, 200);
    EXPECT_FALSE(plain->has_header("Access-Control-Allow-Origin"));

    server.Stop();
}
