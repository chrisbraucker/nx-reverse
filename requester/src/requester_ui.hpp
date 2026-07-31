#pragma once

#include "runtime.hpp"
#include "runtime_config.hpp"

namespace requester {

int RunRequesterUi(AppContext& context, RuntimeConfig defaults, ConfigLoadReport loaded_config);

} // namespace requester
