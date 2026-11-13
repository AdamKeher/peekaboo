#include <gtest/gtest.h>

#include "core/Logger.h"

namespace {

class LoggerCallbackReset
{
public:
    ~LoggerCallbackReset() { adm::core::Logger::SetCallback(nullptr); }
};

TEST(LoggerTest, RuntimeDebugMessageReachesCallbackAtDebugLevel)
{
    LoggerCallbackReset reset;
    adm::core::LogLevel receivedLevel = adm::core::LogLevel::Info;
    std::string receivedMessage;
    adm::core::Logger::SetCallback([&](adm::core::LogLevel level, const std::string& message) {
        receivedLevel = level;
        receivedMessage = message;
    });

    adm::core::Logger::LogMessage(adm::core::LogLevel::Debug, "KPrintF: hello");

    EXPECT_EQ(receivedLevel, adm::core::LogLevel::Debug);
    EXPECT_EQ(receivedMessage, "KPrintF: hello");
}

} // namespace
