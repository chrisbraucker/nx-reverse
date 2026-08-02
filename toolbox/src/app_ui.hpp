#pragma once

#include "runtime.hpp"
#include "runtime_config.hpp"

namespace toolbox {

int RunAppUi(AppContext& context, RuntimeConfig defaults, ConfigLoadReport loaded_config);

} // namespace toolbox
