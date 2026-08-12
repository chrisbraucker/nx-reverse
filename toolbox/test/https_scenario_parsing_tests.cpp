#include <cassert>
#include <string>

#include "https_scenario_parsing.hpp"

int main() {
    toolbox::HttpsTarget target{};
    std::string detail;
    assert(toolbox::ParseHttpsUri("https://api.hac.lp1.ctest.srv.nintendo.net/v1/ip", target, detail));
    assert(target.host == "api.hac.lp1.ctest.srv.nintendo.net" && target.port == 443 && target.path == "/v1/ip");
    assert(toolbox::ParseHttpsUri("https://example.test:8443/check", target, detail));
    assert(target.host == "example.test" && target.port == 8443 && target.path == "/check");
    assert(toolbox::ParseHttpsUri("https://example.test/check", target, detail));
    assert(target.port == 443);
    assert(!toolbox::ParseHttpsUri("http://example.test/check", target, detail));
    assert(!toolbox::ParseHttpsUri("https://example.test:0/check", target, detail));

    std::string global_ip;
    assert(
        toolbox::ParseGlobalIpResponse(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"global_ip\":\"203.0.113.7\"}",
            global_ip,
            detail
        )
    );
    assert(global_ip == "203.0.113.7");
    assert(!toolbox::ParseGlobalIpResponse("HTTP/1.1 404 Not Found\r\n\r\n{}", global_ip, detail));
    assert(!toolbox::ParseGlobalIpResponse("HTTP/1.1 200 OK\r\n\r\n{\"global_ip\":\"not-an-ip\"}", global_ip, detail));
}
