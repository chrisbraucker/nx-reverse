#include "bsd_system_udp_scenario.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <span>
#include <string>

#include <arpa/inet.h>
#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.hpp"
#include "logger.hpp"
#include "runtime.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace requester {

namespace {

constexpr std::size_t PayloadHeaderBytes = 24;

class BsdSocketScope {
public:
  BsdSocketScope() = default;

  [[nodiscard]] Result Initialize() {
    const Result rc = socketInitialize(&config::SocketConfigBsdSystemMitm);
    initialized_ = R_SUCCEEDED(rc);
    return rc;
  }

  ~BsdSocketScope() {
    if (initialized_) {
      socketExit();
    }
  }

  BsdSocketScope(const BsdSocketScope &) = delete;
  BsdSocketScope &operator=(const BsdSocketScope &) = delete;

private:
  bool initialized_{};
};

void StoreBigEndian32(std::uint8_t *out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 24U);
  out[1] = static_cast<std::uint8_t>(value >> 16U);
  out[2] = static_cast<std::uint8_t>(value >> 8U);
  out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t NextPayloadByte(std::uint32_t *state) {
  *state ^= *state << 13U;
  *state ^= *state >> 17U;
  *state ^= *state << 5U;
  return *state;
}

void BuildPayload(std::span<std::uint8_t> payload, std::uint32_t sequence,
                  std::uint32_t flow_index,
                  const TunnelUdpWorkloadConfig &config) {
  std::memset(payload.data(), 0, payload.size());
  std::memcpy(payload.data(), "NXRVBS1", 7);
  StoreBigEndian32(payload.data() + 8, config.workload_id);
  StoreBigEndian32(payload.data() + 12, flow_index);
  StoreBigEndian32(payload.data() + 16, sequence);
  StoreBigEndian32(payload.data() + 20, config.payload_seed);

  std::uint32_t state =
      config.payload_seed ^ sequence ^ (flow_index * 0x9E3779B9U);
  for (std::size_t index = PayloadHeaderBytes; index < payload.size();
       ++index) {
    payload[index] = static_cast<std::uint8_t>(NextPayloadByte(&state));
  }
}

void CloseDescriptors(std::span<int> descriptors) {
  for (int &descriptor : descriptors) {
    if (descriptor >= 0) {
      static_cast<void>(close(descriptor));
      descriptor = -1;
    }
  }
}

[[nodiscard]] bool IsExpectedReply(std::span<const std::uint8_t> actual,
                                   std::span<const std::uint8_t> expected) {
  return actual.size() == expected.size() &&
         std::memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

[[nodiscard]] std::string FormatEndpoint(const sockaddr_in &endpoint) {
  char address[INET_ADDRSTRLEN]{};
  const char *const text =
      inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
  return std::string(text != nullptr ? text : "<invalid>") + ":" +
         std::to_string(ntohs(endpoint.sin_port));
}

} // namespace

ScenarioResult RunBsdSystemUdpWorkload(AppContext &ctx,
                                       const TunnelUdpWorkloadConfig &config) {
  ScenarioResult result{.name = "bsd_system_udp_workload"};
  logger::Status(ctx, "Running normal bsd:s UDP workload to %s:%u",
                 config.destination_ipv4.c_str(), config.destination_port);

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(config.destination_port);
  if (inet_pton(AF_INET, config.destination_ipv4.c_str(), &remote.sin_addr) !=
      1) {
    result.detail = "invalid configured BSD workload destination";
    return result;
  }
  if (config.concurrent_flows == 0 ||
      config.concurrent_flows > wgnx::tunnel::MaximumFlowsPerClient) {
    result.detail = "configured BSD workload flow count is unsupported";
    return result;
  }

  BsdSocketScope socket_scope;
  const Result initialize_rc = socket_scope.Initialize();
  if (R_FAILED(initialize_rc)) {
    result.rc = initialize_rc;
    result.detail = "socketInitialize(bsd:s) failed";
    return result;
  }

  std::array<int, wgnx::tunnel::MaximumFlowsPerClient> descriptors{};
  descriptors.fill(-1);
  for (std::uint32_t flow_index = 0; flow_index < config.concurrent_flows;
       ++flow_index) {
    const int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0) {
      result.err = errno;
      result.detail = "socket failed flow=" + std::to_string(flow_index) +
                      ": " + FormatErrno(errno);
      CloseDescriptors(descriptors);
      return result;
    }
    descriptors[flow_index] = descriptor;
    if (connect(descriptor, reinterpret_cast<const sockaddr *>(&remote),
                sizeof(remote)) != 0) {
      result.err = errno;
      result.detail = "connect failed flow=" + std::to_string(flow_index) +
                      ": " + FormatErrno(errno);
      CloseDescriptors(descriptors);
      return result;
    }
  }
  logger::Log(ctx, "bsd_system_udp opened flows=%u destination=%s",
              config.concurrent_flows, FormatEndpoint(remote).c_str());

  std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadBytes>
      payload_storage{};
  const std::span<std::uint8_t> payload(payload_storage.data(),
                                        config.payload_bytes);
  std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadBytes> received{};
  std::uint32_t echoed = 0;

  for (std::uint32_t sequence = 0; sequence < config.datagram_count;
       ++sequence) {
    const std::uint32_t flow_index = sequence % config.concurrent_flows;
    const int descriptor = descriptors[flow_index];
    BuildPayload(payload, sequence, flow_index, config);
    const ssize_t sent = send(descriptor, payload.data(), payload.size(), 0);
    if (sent < 0) {
      result.err = errno;
      result.detail = "send failed sequence=" + std::to_string(sequence) +
                      ": " + FormatErrno(errno);
      break;
    }
    if (static_cast<std::size_t>(sent) != payload.size()) {
      result.detail = "short UDP send sequence=" + std::to_string(sequence);
      break;
    }
    result.bytes_sent += static_cast<std::size_t>(sent);

    if (config.echo_replies) {
      pollfd poll_descriptor{
          .fd = descriptor,
          .events = POLLIN,
          .revents = 0,
      };
      const int poll_result = poll(
          &poll_descriptor, 1, static_cast<int>(config.receive_deadline_ms));
      if (poll_result < 0) {
        result.err = errno;
        result.detail = "poll failed sequence=" + std::to_string(sequence) +
                        ": " + FormatErrno(errno);
        break;
      }
      if (poll_result == 0) {
        result.err = ETIMEDOUT;
        result.detail = "poll timed out sequence=" + std::to_string(sequence);
        break;
      }
      if ((poll_descriptor.revents & POLLIN) == 0) {
        result.detail =
            "poll missing POLLIN sequence=" + std::to_string(sequence) +
            " revents=" + std::to_string(poll_descriptor.revents);
        break;
      }

      sockaddr_in source{};
      socklen_t source_length = sizeof(source);
      const ssize_t received_count =
          recvfrom(descriptor, received.data(), received.size(), 0,
                   reinterpret_cast<sockaddr *>(&source), &source_length);
      if (received_count < 0) {
        result.err = errno;
        result.detail = "recvfrom failed sequence=" + std::to_string(sequence) +
                        ": " + FormatErrno(errno);
        break;
      }
      result.bytes_received += static_cast<std::size_t>(received_count);
      const std::span<const std::uint8_t> reply(
          received.data(), static_cast<std::size_t>(received_count));
      if (!IsExpectedReply(reply, payload)) {
        result.detail = "unexpected echo sequence=" + std::to_string(sequence) +
                        " source=" + FormatEndpoint(source);
        break;
      }
      if (source_length < sizeof(sockaddr_in) || source.sin_family != AF_INET ||
          source.sin_addr.s_addr != remote.sin_addr.s_addr ||
          source.sin_port != remote.sin_port) {
        result.detail =
            "unexpected reply endpoint sequence=" + std::to_string(sequence) +
            " source=" + FormatEndpoint(source);
        break;
      }
      ++echoed;
      logger::Log(ctx, "bsd_system_udp echo flow=%u sequence=%u source=%s",
                  flow_index, sequence, FormatEndpoint(source).c_str());
    }

    if (config.pacing_ms != 0) {
      SleepMilliseconds(config.pacing_ms);
    }
  }

  CloseDescriptors(descriptors);
  if (result.detail.empty() &&
      (!config.echo_replies || echoed == config.datagram_count)) {
    result.success = true;
    result.detail =
        "bsd_service=system destination=" + config.destination_ipv4 + ":" +
        std::to_string(config.destination_port) +
        " flows=" + std::to_string(config.concurrent_flows) +
        " sent=" + std::to_string(config.datagram_count) +
        " echoed=" + std::to_string(echoed);
  }
  return result;
}

} // namespace requester
