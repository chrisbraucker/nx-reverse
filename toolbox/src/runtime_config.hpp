#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bsd_system_udp_outcome.hpp"

namespace toolbox {

struct TunnelUdpWorkloadConfig {
    std::string destination_ipv4;
    std::uint16_t destination_port{};
    std::uint32_t workload_id{};
    std::size_t payload_bytes{};
    std::uint32_t datagram_count{};
    std::uint32_t pacing_ms{};
    std::uint32_t concurrent_flows{};
    std::uint32_t receive_deadline_ms{};
    std::uint32_t payload_seed{};
    bool echo_replies{true};
};

struct TunnelContractValidationConfig {
    bool verify_cloned_session_lifetime{true};
    bool verify_mixed_batch{true};
};

struct BsdSystemUdpWorkloadConfig {
    bool verify_post_route_rejection{false};
    BsdSystemUdpExpectedOutcome expected_outcome{BsdSystemUdpExpectedOutcome::EchoReply};
    bool require_writable_recovery{false};
};

enum class RuntimeScenario : std::uint8_t {
    DirectTunnelUdp,
    BsdSystemUdp,
    TunnelContractValidation,
};

struct RuntimeProfile {
    std::string name;
    std::string tunnel_destination_ipv4;
    std::string bsd_destination_ipv4;
    std::uint16_t udp_destination_port{};
};

struct UdpScenarioConfig {
    std::size_t payload_bytes{};
    std::uint32_t datagram_count{};
    std::uint32_t pacing_ms{};
    std::uint32_t concurrent_flows{};
    std::uint32_t receive_deadline_ms{};
    std::uint32_t payload_seed{};
    bool echo_replies{true};
};

struct RuntimeConfig {
    RuntimeScenario scenario{RuntimeScenario::DirectTunnelUdp};
    std::uint32_t next_workload_id{};
    std::size_t active_profile{};
    std::vector<RuntimeProfile> profiles;
    UdpScenarioConfig udp;
    TunnelContractValidationConfig tunnel_contract;
    BsdSystemUdpWorkloadConfig bsd_system_udp;
};

struct ConfigLoadReport {
    RuntimeConfig config;
    bool loaded_from_file{false};
    std::vector<std::string> diagnostics;
};

constexpr char RuntimeConfigPath[] = "sdmc:/config/nxrv-toolbox/config.ini";

RuntimeConfig CompiledRuntimeDefaults();
ConfigLoadReport LoadRuntimeConfig(const RuntimeConfig& defaults, const char* path = RuntimeConfigPath);
bool SaveRuntimeConfig(const RuntimeConfig& config, std::string* error, const char* path = RuntimeConfigPath);
bool ValidateRuntimeConfig(const RuntimeConfig& config, std::string* error);
bool EnsureRuntimeConfigDirectories();
const char* BsdSystemUdpExpectedOutcomeName(BsdSystemUdpExpectedOutcome expected_outcome);
const char* RuntimeScenarioName(RuntimeScenario scenario);
const RuntimeProfile* ActiveRuntimeProfile(const RuntimeConfig& config);
RuntimeProfile* ActiveRuntimeProfile(RuntimeConfig* config);
TunnelUdpWorkloadConfig BuildTunnelUdpWorkload(const RuntimeConfig& config, const RuntimeProfile& profile, std::uint32_t workload_id);

} // namespace toolbox
