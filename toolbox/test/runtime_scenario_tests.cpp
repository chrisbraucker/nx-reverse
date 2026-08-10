#include <cassert>

#include "runtime_scenario.hpp"

int main() {
    using namespace toolbox;
    RuntimeScenario scenario{};
    assert(ParseRuntimeScenario("direct_tunnel_tcp", &scenario) && scenario == RuntimeScenario::DirectTunnelTcp);
    assert(ParseRuntimeScenario("bsd_system_tcp", &scenario) && scenario == RuntimeScenario::BsdSystemTcp);
    assert(!ParseRuntimeScenario("unknown", &scenario));
    assert(!ParseRuntimeScenario("direct_tunnel_tcp", nullptr));
}
