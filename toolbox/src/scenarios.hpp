#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "runtime.hpp"

namespace toolbox {

struct ScenarioResult {
    std::string name;
    bool success = false;
    bool skipped = false;
    Result rc = 0;
    int err = 0;
    std::size_t bytes_sent = 0;
    std::size_t bytes_received = 0;
    std::string detail;
};

struct ScenarioDescriptor {
    std::string_view name;
    std::string_view description;
    std::string_view compiled_defaults;
};

std::span<const ScenarioDescriptor> AvailableScenarios();
ScenarioResult RunScenario(AppContext& ctx, std::string_view name);
std::vector<ScenarioResult> RunScenarios(AppContext& ctx);

} // namespace toolbox
