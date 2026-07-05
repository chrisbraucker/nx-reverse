#pragma once

namespace wgnx::net_probe::mitm {

bool StartServers();
void WaitForShutdownRequest();
void RequestShutdown();
void StopServers();

} // namespace wgnx::net_probe::mitm
