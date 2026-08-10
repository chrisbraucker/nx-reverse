#pragma once

#include "runtime_config.hpp"

#include <string_view>

namespace toolbox {

bool ParseRuntimeScenario(std::string_view value, RuntimeScenario* scenario);

} // namespace toolbox
