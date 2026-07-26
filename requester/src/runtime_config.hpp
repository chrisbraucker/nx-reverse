#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace requester {

struct TunnelUdpWorkloadConfig {
  bool enabled{true};
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
  bool enabled{false};
  bool verify_cloned_session_lifetime{true};
  bool verify_mixed_batch{true};
};

struct RuntimeConfig {
  TunnelUdpWorkloadConfig tunnel_udp;
  TunnelContractValidationConfig tunnel_contract;
};

struct ConfigLoadReport {
  RuntimeConfig config;
  bool loaded_from_file{false};
  std::vector<std::string> diagnostics;
};

constexpr char RuntimeConfigPath[] = "sdmc:/switch/requester/config.ini";

RuntimeConfig CompiledRuntimeDefaults();
ConfigLoadReport LoadRuntimeConfig(const RuntimeConfig &defaults,
                                   const char *path = RuntimeConfigPath);
bool SaveRuntimeConfig(const RuntimeConfig &config, std::string *error,
                       const char *path = RuntimeConfigPath);
bool ValidateRuntimeConfig(const RuntimeConfig &config, std::string *error);
bool EnsureRuntimeConfigDirectories();

} // namespace requester
