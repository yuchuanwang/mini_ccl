#include "logger.h"
#include "types.h"

void test_logger()
{
    Logger::Instance().SetLogLevel(LogLevel::DEBUG);
    LOG_DEBUG("Log Debug without format");
    LOG_INFO("Log Info with format {}-{}-{}", 1, 2.0f, "text");
    LOG_WARN("Log Warn with format {}-{}-{}", 1, 2.0f, "text");
    LOG_ERROR("Log Error with format {}-{}-{}", 1, 2.0f, "text");
}

int main()
{
    test_logger();
    return 0;
}
