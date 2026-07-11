#include "logger.hpp"

#include <cstdarg>
#include <cstdio>

namespace requester::logger {

namespace {

constexpr const char *BootstrapLogPath = "sdmc:/nxrv/requester/requester-bootstrap.log";

void WriteBootstrapLine(const char *line) {
    FILE *file = std::fopen(BootstrapLogPath, "a");
    if (file == nullptr) {
        return;
    }

    std::fprintf(
        file,
        "[%s][%s] %s\n",
        TimestampUtc().c_str(),
        MonotonicTimestampNs().c_str(),
        line);
    std::fflush(file);
    std::fclose(file);
}

void WriteFormatted(AppContext& ctx, bool mirror_to_console, const char *fmt, va_list args) {
    char buffer[1024];
    va_list args_copy;
    va_copy(args_copy, args);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args_copy);
    va_end(args_copy);

    if (mirror_to_console) {
        std::printf("%s\n", buffer);
        consoleUpdate(nullptr);
    }

    if (ctx.log_file != nullptr) {
        std::fprintf(
            ctx.log_file,
            "[%s][%s] %s\n",
            TimestampUtc().c_str(),
            MonotonicTimestampNs().c_str(),
            buffer);
        std::fflush(ctx.log_file);
    }
}

} // namespace

void Bootstrap(const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    WriteBootstrapLine(buffer);
}

bool OpenLog(AppContext& ctx) {
    ctx.run_id = MakeRunId();
    ctx.log_path = MakeLogPath(ctx.run_id);
    ctx.log_file = std::fopen(ctx.log_path.c_str(), "w");
    return ctx.log_file != nullptr;
}

void CloseLog(AppContext& ctx) {
    if (ctx.log_file != nullptr) {
        std::fflush(ctx.log_file);
        std::fclose(ctx.log_file);
        ctx.log_file = nullptr;
    }
}

void Log(AppContext& ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteFormatted(ctx, false, fmt, args);
    va_end(args);
}

void Status(AppContext& ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteFormatted(ctx, true, fmt, args);
    va_end(args);
}

} // namespace requester::logger
