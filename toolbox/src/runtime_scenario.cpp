#include "runtime_scenario.hpp"

namespace toolbox {

bool ParseRuntimeScenario(std::string_view value, RuntimeScenario* scenario) {
    if (scenario == nullptr) {
        return false;
    }
    if (value == "direct_tunnel_udp") {
        *scenario = RuntimeScenario::DirectTunnelUdp;
        return true;
    }
    if (value == "direct_tunnel_tcp") {
        *scenario = RuntimeScenario::DirectTunnelTcp;
        return true;
    }
    if (value == "bsd_system_udp") {
        *scenario = RuntimeScenario::BsdSystemUdp;
        return true;
    }
    if (value == "bsd_system_tcp") {
        *scenario = RuntimeScenario::BsdSystemTcp;
        return true;
    }
    if (value == "tunnel_contract_validation") {
        *scenario = RuntimeScenario::TunnelContractValidation;
        return true;
    }
    return false;
}

} // namespace toolbox
