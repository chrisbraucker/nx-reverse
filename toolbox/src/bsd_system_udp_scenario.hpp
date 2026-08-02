#pragma once

#include "runtime_config.hpp"
#include "scenarios.hpp"

namespace toolbox {

// Exercises the production bsd:s client path without opening wgnx:tun.
// The toolbox-only MITM may reroute this traffic when a covering peer is
// active.
ScenarioResult RunBsdSystemUdpWorkload(
    AppContext& ctx, const TunnelUdpWorkloadConfig& config, const BsdSystemUdpWorkloadConfig& bsd_config
);

} // namespace toolbox
