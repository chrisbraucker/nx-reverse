#pragma once

#include "runtime.hpp"
#include "runtime_config.hpp"

namespace toolbox {

int RunAppUi(AppContext& context, const RuntimeConfig& defaults, const ConfigLoadReport& loaded_config);

} // namespace toolbox
