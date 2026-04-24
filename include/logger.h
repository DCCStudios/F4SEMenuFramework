
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = F4SE::log;

void SetupLog() {
    auto logsFolder = F4SE::log::log_directory();
    if (!logsFolder) F4SE::stl::report_and_fail("F4SE log_directory not provided, logs disabled.");
    auto logFilePath = *logsFolder / "F4SEMenuFramework.log";
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
#else
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
#endif
    logger::info("F4SE Menu Framework - log initialized.");
}

