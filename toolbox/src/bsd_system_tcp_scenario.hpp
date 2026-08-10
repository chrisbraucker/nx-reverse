#pragma once

#include <cstdint>

#include "runtime_config.hpp"
#include "scenarios.hpp"

namespace toolbox {

// Exercises one ordinary bsd:s TCP connection through the selected profile.
ScenarioResult RunBsdSystemTcpExchange(
    AppContext& ctx, const RuntimeProfile& profile, std::uint32_t workload_id, const TcpScenarioConfig& config
);

} // namespace toolbox
