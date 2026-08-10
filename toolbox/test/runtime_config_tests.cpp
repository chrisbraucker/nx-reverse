#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "runtime_config.hpp"

namespace {

toolbox::RuntimeConfig Defaults() {
    return {
        .scenario = toolbox::RuntimeScenario::DirectTunnelUdp,
        .next_workload_id = 100,
        .active_profile = 0,
        .profiles = {{"Default", "10.1.0.2", "192.168.1.2", 29000, 28080}},
        .udp = {.payload_bytes = 48, .datagram_count = 1, .pacing_ms = 0, .concurrent_flows = 1, .receive_deadline_ms = 5000},
        .tcp = {.receive_deadline_ms = 5000},
        .tunnel_contract = {},
        .bsd_system_udp = {},
    };
}

void Write(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream file(path);
    assert(file.good());
    file << contents;
    assert(file.good());
}

} // namespace

int main() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "toolbox-runtime-config-tests.ini";
    const toolbox::RuntimeConfig defaults = Defaults();

    Write(
        path,
        "run.scenario=direct_tunnel_tcp\n"
        "run.next_workload_id=201\n"
        "run.active_profile=1\n"
        "profiles.count=2\n"
        "profile.1.name=Tunnel\n"
        "profile.1.tunnel_destination_ipv4=10.1.0.3\n"
        "profile.1.bsd_destination_ipv4=192.168.1.3\n"
        "profile.1.udp_destination_port=29001\n"
        "profile.1.tcp_destination_port=28082\n"
        "tcp.receive_deadline_ms=2500\n"
    );
    const toolbox::ConfigLoadReport loaded = toolbox::LoadRuntimeConfig(defaults, path.c_str());
    assert(loaded.loaded_from_file);
    assert(loaded.diagnostics.empty());
    assert(loaded.config.scenario == toolbox::RuntimeScenario::DirectTunnelTcp);
    assert(loaded.config.next_workload_id == 201);
    assert(loaded.config.active_profile == 1);
    const toolbox::RuntimeProfile* profile = toolbox::ActiveRuntimeProfile(loaded.config);
    assert(profile != nullptr && profile->name == "Tunnel" && profile->tcp_destination_port == 28082);
    assert(loaded.config.tcp.receive_deadline_ms == 2500);

    Write(path, "run.scenario=direct_tunnel_tcp\ntcp.receive_deadline_ms=0\n");
    const toolbox::ConfigLoadReport invalid = toolbox::LoadRuntimeConfig(defaults, path.c_str());
    assert(!invalid.diagnostics.empty());
    assert(invalid.config.scenario == defaults.scenario);
    assert(invalid.config.tcp.receive_deadline_ms == defaults.tcp.receive_deadline_ms);

    std::filesystem::remove(path);
}
