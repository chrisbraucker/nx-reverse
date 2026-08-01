#pragma once

#include "runtime_config.hpp"
#include "scenarios.hpp"

namespace requester {

ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx, const TunnelUdpWorkloadConfig& config);
ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx);
ScenarioResult RunWgnxTunnelContractValidation(
    AppContext& ctx, const TunnelUdpWorkloadConfig& workload, const TunnelContractValidationConfig& config
);

} // namespace requester
