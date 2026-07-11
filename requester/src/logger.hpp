#pragma once

#include "runtime.hpp"

namespace requester::logger {

void Bootstrap(const char *fmt, ...);
bool OpenLog(AppContext& ctx);
void CloseLog(AppContext& ctx);
void Log(AppContext& ctx, const char *fmt, ...);
void Status(AppContext& ctx, const char *fmt, ...);

} // namespace requester::logger
