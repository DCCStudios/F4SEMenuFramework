
#include <spdlog/sinks/basic_file_sink.h>

#include <ShlObj_core.h>  // SHGetKnownFolderPath for the Documents log directory

// (namespace logger = spdlog lives in PCH.h; SetupLog() installs the file
// logger as before, and the vendored REX logging layer also routes through
// spdlog's default logger, so CommonLib-internal messages land in the same
// file.)

// Classic F4SE::log::log_directory() replacement:
// Documents/My Games/Fallout4/F4SE (or Fallout4 MS for the Game Pass build).
inline std::optional<std::filesystem::path> GetF4SELogDirectory() {
    wchar_t* docsRaw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docsRaw))) {
        if (docsRaw) ::CoTaskMemFree(docsRaw);
        return std::nullopt;
    }
    std::filesystem::path docs(docsRaw);
    ::CoTaskMemFree(docsRaw);
    return docs / "My Games" / "Fallout4" / "F4SE";
}

void SetupLog() {
    auto logsFolder = GetF4SELogDirectory();
    if (!logsFolder) REX::FAIL("F4SE log directory not found, logs disabled.");
    std::error_code ec;
    std::filesystem::create_directories(*logsFolder, ec);
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
