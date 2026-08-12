#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace toolbox {

struct HttpsTarget {
    std::string host;
    std::uint16_t port = 443;
    std::string path;
};

bool ParseHttpsUri(const char* source, HttpsTarget& target, std::string& detail);
bool ParseGlobalIpResponse(std::string_view response, std::string& global_ip, std::string& detail);

} // namespace toolbox
