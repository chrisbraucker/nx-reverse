#pragma once

#include "runtime.hpp"

namespace requester::logger {

bool OpenLog(AppContext& ctx);
void CloseLog(AppContext& ctx);
void Log(AppContext& ctx, const char *fmt, ...);
void Status(AppContext& ctx, const char *fmt, ...);

} // namespace requester::logger
