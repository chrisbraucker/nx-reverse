#pragma once

#include "runtime_config.hpp"
#include "scenarios.hpp"

namespace requester {

// Exercises the production bsd:s client path without opening wgnx:tun.
// The requester-only MITM may reroute this traffic when a covering peer is
// active.
ScenarioResult RunBsdSystemUdpWorkload(AppContext &ctx,
                                       const TunnelUdpWorkloadConfig &config,
                                       const BsdSystemUdpWorkloadConfig &bsd_config);

} // namespace requester
