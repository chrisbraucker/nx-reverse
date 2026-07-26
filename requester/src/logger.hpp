#pragma once

#include <functional>
#include <string>
#include <vector>

#include "runtime.hpp"

namespace requester::logger {

void Bootstrap(const char *fmt, ...);
bool OpenLog(AppContext& ctx);
void CloseLog(AppContext& ctx);
void Log(AppContext& ctx, const char *fmt, ...);
void Status(AppContext& ctx, const char *fmt, ...);
void SetUiSink(std::function<void(const std::string&)> sink);
std::vector<std::string> RecentLines();

} // namespace requester::logger
