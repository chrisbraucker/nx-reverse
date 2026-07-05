#pragma once

#include <string>
#include <vector>

#include "runtime.hpp"

namespace requester {

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

std::vector<ScenarioResult> RunScenarios(AppContext& ctx);

} // namespace requester
