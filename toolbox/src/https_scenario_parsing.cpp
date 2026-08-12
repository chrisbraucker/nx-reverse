#include "https_scenario_parsing.hpp"

#include <charconv>

namespace toolbox {

namespace {

constexpr std::size_t Ipv4StringCapacity = 16;

bool IsIpv4Address(const std::string_view address) {
    unsigned int octets = 0;
    unsigned int value = 0;
    unsigned int digits = 0;
    for (const char ch : address) {
        if (ch == '.') {
            if (digits == 0 || value > 255 || octets == 3) {
                return false;
            }
            ++octets;
            value = 0;
            digits = 0;
            continue;
        }
        if (ch < '0' || ch > '9') {
            return false;
        }
        if (++digits > 3) {
            return false;
        }
        value = value * 10 + static_cast<unsigned int>(ch - '0');
    }
    return octets == 3 && digits > 0 && value <= 255;
}

} // namespace

bool ParseHttpsUri(const char* source, HttpsTarget& target, std::string& detail) {
    constexpr std::string_view scheme = "https://";
    const std::string_view uri(source);
    target = {};
    if (!uri.starts_with(scheme)) {
        detail = "target URI must start with https://";
        return false;
    }

    const std::size_t authority_begin = scheme.size();
    const std::size_t path_begin = uri.find('/', authority_begin);
    const std::string_view authority = uri.substr(authority_begin, path_begin - authority_begin);
    if (authority.empty() || authority.find_first_of("@?#") != std::string_view::npos) {
        detail = "target URI has an invalid authority";
        return false;
    }

    std::string_view host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        unsigned int port = 0;
        const std::string_view port_text = authority.substr(colon + 1);
        const auto [parse_end, parse_error] = std::from_chars(port_text.begin(), port_text.end(), port);
        if (host.empty() || parse_error != std::errc{} || parse_end != port_text.end() || port == 0 || port > 65535) {
            detail = "target URI has an invalid port";
            return false;
        }
        target.port = static_cast<std::uint16_t>(port);
    }

    if (host.empty()) {
        detail = "target URI has an empty host";
        return false;
    }

    target.host = host;
    target.path = path_begin == std::string_view::npos ? "/" : uri.substr(path_begin);
    return true;
}

bool ParseGlobalIpResponse(const std::string_view response, std::string& global_ip, std::string& detail) {
    const std::size_t status_end = response.find("\r\n");
    if (status_end == std::string_view::npos || !response.substr(0, status_end).starts_with("HTTP/1.") ||
        response.substr(0, status_end).find(" 200 ") == std::string_view::npos) {
        detail = "expected HTTP status 200";
        return false;
    }

    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        detail = "response headers exceed the read buffer";
        return false;
    }

    constexpr std::string_view json_prefix = "{\"global_ip\":\"";
    const std::string_view body = response.substr(header_end + 4);
    if (!body.starts_with(json_prefix)) {
        detail = "response body does not contain global_ip";
        return false;
    }

    const std::size_t ip_begin = json_prefix.size();
    const std::size_t ip_end = body.find('"', ip_begin);
    if (ip_end == std::string_view::npos || ip_end == ip_begin || ip_end - ip_begin >= Ipv4StringCapacity) {
        detail = "response global_ip is invalid";
        return false;
    }

    const std::string_view ip = body.substr(ip_begin, ip_end - ip_begin);
    if (!IsIpv4Address(ip)) {
        detail = "response global_ip is not IPv4";
        return false;
    }

    global_ip = ip;
    return true;
}

} // namespace toolbox
