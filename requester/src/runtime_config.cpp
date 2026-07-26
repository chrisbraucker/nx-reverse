#include "runtime_config.hpp"

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include <sys/stat.h>

#include <switch.h>

#include "config.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace requester {

namespace {

constexpr std::size_t MinimumPayloadBytes = 24;
constexpr std::uint32_t MaximumDatagramCount = 4096;
constexpr std::uint32_t MaximumDurationMs = 60000;

std::string Trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

bool ParseBool(std::string_view text, bool *value) {
  if (text == "true" || text == "1") {
    *value = true;
    return true;
  }
  if (text == "false" || text == "0") {
    *value = false;
    return true;
  }
  return false;
}

template <typename T> bool ParseUnsigned(std::string_view text, T *value) {
  static_assert(std::numeric_limits<T>::is_integer);
  T parsed{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

bool IsValidIpv4(std::string_view text) {
  std::array<unsigned int, 4> octets{};
  std::size_t offset = 0;
  for (std::size_t index = 0; index < octets.size(); ++index) {
    const std::size_t separator = text.find('.', offset);
    const bool final_component = index + 1 == octets.size();
    if ((final_component && separator != std::string_view::npos) ||
        (!final_component && separator == std::string_view::npos)) {
      return false;
    }
    const std::size_t end = final_component ? text.size() : separator;
    if (end == offset || end - offset > 3) {
      return false;
    }
    unsigned int octet{};
    if (!ParseUnsigned(text.substr(offset, end - offset), &octet) ||
        octet > 255) {
      return false;
    }
    octets[index] = octet;
    offset = end + 1;
  }
  return offset == text.size() + 1;
}

bool EnsureDirectory(const char *path) {
  return mkdir(path, 0777) == 0 || errno == EEXIST;
}

void ResetToDefault(const RuntimeConfig &defaults, RuntimeConfig *config,
                    std::string_view key) {
  if (key == "tunnel_udp.enabled") {
    config->tunnel_udp.enabled = defaults.tunnel_udp.enabled;
  } else if (key == "tunnel_udp.destination_ipv4") {
    config->tunnel_udp.destination_ipv4 = defaults.tunnel_udp.destination_ipv4;
  } else if (key == "tunnel_udp.destination_port") {
    config->tunnel_udp.destination_port = defaults.tunnel_udp.destination_port;
  } else if (key == "tunnel_udp.workload_id") {
    config->tunnel_udp.workload_id = defaults.tunnel_udp.workload_id;
  } else if (key == "tunnel_udp.payload_bytes") {
    config->tunnel_udp.payload_bytes = defaults.tunnel_udp.payload_bytes;
  } else if (key == "tunnel_udp.datagram_count") {
    config->tunnel_udp.datagram_count = defaults.tunnel_udp.datagram_count;
  } else if (key == "tunnel_udp.pacing_ms") {
    config->tunnel_udp.pacing_ms = defaults.tunnel_udp.pacing_ms;
  } else if (key == "tunnel_udp.concurrent_flows") {
    config->tunnel_udp.concurrent_flows = defaults.tunnel_udp.concurrent_flows;
  } else if (key == "tunnel_udp.receive_deadline_ms") {
    config->tunnel_udp.receive_deadline_ms =
        defaults.tunnel_udp.receive_deadline_ms;
  } else if (key == "tunnel_udp.payload_seed") {
    config->tunnel_udp.payload_seed = defaults.tunnel_udp.payload_seed;
  } else if (key == "tunnel_udp.echo_replies") {
    config->tunnel_udp.echo_replies = defaults.tunnel_udp.echo_replies;
  }
}

bool ApplySetting(const RuntimeConfig &defaults, RuntimeConfig *config,
                  std::string_view key, std::string_view value,
                  std::string *error) {
  auto invalid = [&] {
    ResetToDefault(defaults, config, key);
    *error = "invalid value for " + std::string(key) +
             "; using compiled default";
    return false;
  };

  if (key == "tunnel_udp.enabled") {
    return ParseBool(value, &config->tunnel_udp.enabled) || invalid();
  }
  if (key == "tunnel_udp.destination_ipv4") {
    if (!IsValidIpv4(value)) {
      return invalid();
    }
    config->tunnel_udp.destination_ipv4 = value;
    return true;
  }
  if (key == "tunnel_udp.destination_port") {
    std::uint16_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed == 0) {
      return invalid();
    }
    config->tunnel_udp.destination_port = parsed;
    return true;
  }
  if (key == "tunnel_udp.workload_id") {
    return ParseUnsigned(value, &config->tunnel_udp.workload_id) || invalid();
  }
  if (key == "tunnel_udp.payload_bytes") {
    std::size_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed < MinimumPayloadBytes ||
        parsed > wgnx::tunnel::MaximumUdpPayloadBytes) {
      return invalid();
    }
    config->tunnel_udp.payload_bytes = parsed;
    return true;
  }
  if (key == "tunnel_udp.datagram_count") {
    std::uint32_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed == 0 ||
        parsed > MaximumDatagramCount) {
      return invalid();
    }
    config->tunnel_udp.datagram_count = parsed;
    return true;
  }
  if (key == "tunnel_udp.pacing_ms") {
    std::uint32_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed > MaximumDurationMs) {
      return invalid();
    }
    config->tunnel_udp.pacing_ms = parsed;
    return true;
  }
  if (key == "tunnel_udp.concurrent_flows") {
    std::uint32_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed == 0 ||
        parsed > wgnx::tunnel::MaximumFlowsPerClient) {
      return invalid();
    }
    config->tunnel_udp.concurrent_flows = parsed;
    return true;
  }
  if (key == "tunnel_udp.receive_deadline_ms") {
    std::uint32_t parsed{};
    if (!ParseUnsigned(value, &parsed) || parsed == 0 ||
        parsed > MaximumDurationMs) {
      return invalid();
    }
    config->tunnel_udp.receive_deadline_ms = parsed;
    return true;
  }
  if (key == "tunnel_udp.payload_seed") {
    return ParseUnsigned(value, &config->tunnel_udp.payload_seed) || invalid();
  }
  if (key == "tunnel_udp.echo_replies") {
    return ParseBool(value, &config->tunnel_udp.echo_replies) || invalid();
  }
  *error = "unrecognized configuration key " + std::string(key);
  return false;
}

} // namespace

RuntimeConfig CompiledRuntimeDefaults() {
  return {
      .tunnel_udp =
          {
              .enabled = config::EnableScenarioWgnxTunnelUdpWorkload,
              .destination_ipv4 = config::WgnxTunnelDestinationIpv4,
              .destination_port = config::WgnxTunnelDestinationPort,
              .workload_id = config::WgnxTunnelWorkloadId,
              .payload_bytes = config::WgnxTunnelPayloadBytes,
              .datagram_count = config::WgnxTunnelDatagramCount,
              .pacing_ms = config::WgnxTunnelPacingMs,
              .concurrent_flows = config::WgnxTunnelConcurrentFlows,
              .receive_deadline_ms = config::WgnxTunnelReceiveDeadlineMs,
              .payload_seed = config::WgnxTunnelPayloadSeed,
              .echo_replies = config::WgnxTunnelEchoReplies,
          },
  };
}

ConfigLoadReport LoadRuntimeConfig(const RuntimeConfig &defaults,
                                   const char *path) {
  ConfigLoadReport report{.config = defaults};
  FILE *file = std::fopen(path, "r");
  if (file == nullptr) {
    if (errno != ENOENT) {
      report.diagnostics.push_back("unable to read configuration: " +
                                   std::string(std::strerror(errno)));
    }
    return report;
  }

  report.loaded_from_file = true;
  char line[512];
  std::size_t line_number = 0;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    ++line_number;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }
    const std::size_t separator = trimmed.find('=');
    if (separator == std::string::npos) {
      report.diagnostics.push_back("configuration line " +
                                   std::to_string(line_number) +
                                   " has no '=' separator");
      continue;
    }
    const std::string key = Trim(std::string_view(trimmed).substr(0, separator));
    const std::string value =
        Trim(std::string_view(trimmed).substr(separator + 1));
    if (key.empty() || value.empty()) {
      report.diagnostics.push_back("configuration line " +
                                   std::to_string(line_number) +
                                   " has an empty key or value");
      continue;
    }
    std::string error;
    if (!ApplySetting(defaults, &report.config, key, value, &error)) {
      report.diagnostics.push_back("configuration line " +
                                   std::to_string(line_number) + ": " + error);
    }
  }
  std::fclose(file);
  return report;
}

bool ValidateRuntimeConfig(const RuntimeConfig &config, std::string *error) {
  const auto fail = [&](const char *message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  const auto &tunnel = config.tunnel_udp;
  if (!IsValidIpv4(tunnel.destination_ipv4)) {
    return fail("tunnel_udp.destination_ipv4 must be an IPv4 address");
  }
  if (tunnel.destination_port == 0) {
    return fail("tunnel_udp.destination_port must be non-zero");
  }
  if (tunnel.payload_bytes < MinimumPayloadBytes ||
      tunnel.payload_bytes > wgnx::tunnel::MaximumUdpPayloadBytes) {
    return fail("tunnel_udp.payload_bytes is outside the supported range");
  }
  if (tunnel.datagram_count == 0 || tunnel.datagram_count > MaximumDatagramCount) {
    return fail("tunnel_udp.datagram_count is outside the supported range");
  }
  if (tunnel.pacing_ms > MaximumDurationMs ||
      tunnel.receive_deadline_ms == 0 ||
      tunnel.receive_deadline_ms > MaximumDurationMs) {
    return fail("tunnel_udp timeout is outside the supported range");
  }
  if (tunnel.concurrent_flows == 0 ||
      tunnel.concurrent_flows > wgnx::tunnel::MaximumFlowsPerClient) {
    return fail("tunnel_udp.concurrent_flows is outside the supported range");
  }
  return true;
}

bool EnsureRuntimeConfigDirectories() {
  return EnsureDirectory("sdmc:/switch") &&
         EnsureDirectory("sdmc:/switch/requester");
}

bool SaveRuntimeConfig(const RuntimeConfig &config, std::string *error,
                       const char *path) {
  if (!ValidateRuntimeConfig(config, error)) {
    return false;
  }
  if (!EnsureRuntimeConfigDirectories()) {
    if (error != nullptr) {
      *error = "unable to create configuration directory";
    }
    return false;
  }

  const std::string temporary_path = std::string(path) + ".tmp";
  FILE *file = std::fopen(temporary_path.c_str(), "w");
  if (file == nullptr) {
    if (error != nullptr) {
      *error = "unable to create configuration: " +
               std::string(std::strerror(errno));
    }
    return false;
  }
  const auto &tunnel = config.tunnel_udp;
  std::fprintf(file,
               "# NX Reversing Requester runtime configuration\n"
               "tunnel_udp.enabled=%s\n"
               "tunnel_udp.destination_ipv4=%s\n"
               "tunnel_udp.destination_port=%u\n"
               "tunnel_udp.workload_id=%u\n"
               "tunnel_udp.payload_bytes=%zu\n"
               "tunnel_udp.datagram_count=%u\n"
               "tunnel_udp.pacing_ms=%u\n"
               "tunnel_udp.concurrent_flows=%u\n"
               "tunnel_udp.receive_deadline_ms=%u\n"
               "tunnel_udp.payload_seed=%u\n"
               "tunnel_udp.echo_replies=%s\n",
               tunnel.enabled ? "true" : "false",
               tunnel.destination_ipv4.c_str(), tunnel.destination_port,
               tunnel.workload_id, tunnel.payload_bytes, tunnel.datagram_count,
               tunnel.pacing_ms, tunnel.concurrent_flows,
               tunnel.receive_deadline_ms, tunnel.payload_seed,
               tunnel.echo_replies ? "true" : "false");
  const bool flush_failed = std::fflush(file) != 0;
  const bool close_failed = std::fclose(file) != 0;
  const bool write_failed = flush_failed || close_failed;
  if (write_failed) {
    std::remove(temporary_path.c_str());
    if (error != nullptr) {
      *error = "unable to write configuration";
    }
    return false;
  }
  if (std::rename(temporary_path.c_str(), path) != 0) {
    std::remove(temporary_path.c_str());
    if (error != nullptr) {
      *error = "unable to replace configuration: " +
               std::string(std::strerror(errno));
    }
    return false;
  }
  return true;
}

} // namespace requester
