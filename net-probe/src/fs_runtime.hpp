#pragma once

#include <span>
#include <string_view>

#include <stratosphere.hpp>

namespace wgnx::net_probe::fs_runtime {

ams::Result EnsureReady();
ams::Result ResolveSdPath(std::span<char> out_path, std::string_view path);
ams::Result EnsureDirectoryExists(std::string_view path);
ams::Result WriteTextFile(std::string_view path, std::string_view src);

} // namespace wgnx::net_probe::fs_runtime
