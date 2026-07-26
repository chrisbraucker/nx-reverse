#pragma once

#include "scenarios.hpp"
#include "runtime_config.hpp"

namespace requester {

ScenarioResult RunWgnxTunnelUdpWorkload(
    AppContext &ctx, const TunnelUdpWorkloadConfig &config);
ScenarioResult RunWgnxTunnelUdpWorkload(AppContext &ctx);

} // namespace requester
