#include "logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace requester::logger {

namespace {

constexpr const char *BootstrapLogPath = "sdmc:/nxrv/requester/requester-bootstrap.log";

std::mutex g_log_mutex;
std::deque<std::string> g_recent_lines;
std::function<void(const std::string&)> g_ui_sink;

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

void WriteFormatted(AppContext& ctx, const char *fmt, va_list args) {
    char buffer[1024];
    va_list args_copy;
    va_copy(args_copy, args);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args_copy);
    va_end(args_copy);

    std::function<void(const std::string&)> sink;
    const std::string line(buffer);
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        g_recent_lines.push_back(line);
        if (ctx.log_file != nullptr) {
            std::fprintf(
                ctx.log_file,
                "[%s][%s] %s\n",
                TimestampUtc().c_str(),
                MonotonicTimestampNs().c_str(),
                buffer);
            std::fflush(ctx.log_file);
        }
        sink = g_ui_sink;
    }
    if (sink) {
        sink(line);
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
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (ctx.log_file != nullptr) {
        std::fflush(ctx.log_file);
        std::fclose(ctx.log_file);
        ctx.log_file = nullptr;
    }
}

void Log(AppContext& ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteFormatted(ctx, fmt, args);
    va_end(args);
}

void Status(AppContext& ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteFormatted(ctx, fmt, args);
    va_end(args);
}

void SetUiSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_ui_sink = std::move(sink);
}

std::vector<std::string> RecentLines() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return {g_recent_lines.begin(), g_recent_lines.end()};
}

} // namespace requester::logger
