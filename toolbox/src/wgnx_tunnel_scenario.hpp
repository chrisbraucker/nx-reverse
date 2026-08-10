#pragma once

#include "runtime_config.hpp"
#include "scenarios.hpp"

namespace toolbox {

ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx, const TunnelUdpWorkloadConfig& config);
ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx);
ScenarioResult RunWgnxTunnelTcpExchange(
    AppContext& ctx, const RuntimeProfile& profile, std::uint32_t workload_id, const TcpScenarioConfig& config
);
ScenarioResult RunWgnxTunnelContractValidation(
    AppContext& ctx, const TunnelUdpWorkloadConfig& workload, const TunnelContractValidationConfig& config
);

} // namespace toolbox
