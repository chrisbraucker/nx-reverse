#include <stratosphere.hpp>
#include <switch.h>

#include "logger.hpp"
#include "mitm_service.hpp"

namespace ams {

void Main() {
    wgnx::net_probe::logger::Initialize();
    wgnx::net_probe::logger::Log("net-probe main entered");
    wgnx::net_probe::logger::Log("Build: %s-%s", VERSION, BUILD_ID);
    wgnx::net_probe::logger::Log(
        "process heap: addr=0x%016llx size=0x%zx",
        static_cast<unsigned long long>(::ams::os::GetMemoryHeapAddress()),
        ::ams::os::GetMemoryHeapSize());

    if (!wgnx::net_probe::mitm::StartServers()) {
        wgnx::net_probe::logger::Log("Failed to start passive MITM services; exiting net-probe");
        wgnx::net_probe::logger::Shutdown();
        return;
    }
    wgnx::net_probe::logger::Log("Passive MITM active; legacy probe entrypoint disabled");

    // Legacy active probing stays in-tree for reference, but is disabled while
    // net-probe is acting as the passive MITM collector.
    // wgnx::net_probe::RunProbe();

    wgnx::net_probe::mitm::WaitForShutdownRequest();
    wgnx::net_probe::logger::Log("Shutdown requested; stopping servers");
    wgnx::net_probe::mitm::StopServers();
    wgnx::net_probe::logger::Log("Shutdown complete; exiting net-probe");
    wgnx::net_probe::logger::Shutdown();
}

} // namespace ams
