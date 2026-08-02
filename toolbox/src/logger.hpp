#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "runtime.hpp"

namespace toolbox::logger {

void Bootstrap(const char* fmt, ...);
bool OpenLog(AppContext& ctx);
void CloseLog(AppContext& ctx);
void Log(AppContext& ctx, const char* fmt, ...);
void Status(AppContext& ctx, const char* fmt, ...);
void SetUiSink(std::function<void(const std::string&)> sink);
std::vector<std::string> RecentLines(std::size_t maximum_line_count = 0);

} // namespace toolbox::logger

// Keep packet-path diagnostics out of release workload builds entirely.
// In particular, this avoids formatting payload previews for every datagram.
#if TOOLBOX_PACKET_DIAGNOSTICS
#define TOOLBOX_LOG_PACKET(context, ...) ::toolbox::logger::Log((context), __VA_ARGS__)
#else
#define TOOLBOX_LOG_PACKET(context, ...)                                                                                                      \
    do {                                                                                                                                      \
    } while (false)
#endif
