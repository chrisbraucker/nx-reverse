#pragma once

#include <stratosphere.hpp>

namespace wgnx::net_probe::logger {

void Initialize();
void Shutdown();
void AppendLine(const char *path, const char *line);
void AppendBytes(const char *path, const void *data, size_t size);
void Log(const char *fmt, ...);

} // namespace wgnx::net_probe::logger
