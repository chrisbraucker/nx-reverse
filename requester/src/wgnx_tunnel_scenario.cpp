#include "wgnx_tunnel_scenario.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include <arpa/inet.h>
#include <switch.h>

#include "config.hpp"
#include "logger.hpp"
#include "wgnx/tunnel_client.hpp"

namespace requester {

namespace {

constexpr std::size_t PayloadHeaderBytes = 24;

static_assert(config::WgnxTunnelPayloadBytes >= PayloadHeaderBytes);
static_assert(config::WgnxTunnelPayloadBytes <=
              wgnx::tunnel::MaximumUdpPayloadBytes);
static_assert(config::WgnxTunnelConcurrentFlows > 0);
static_assert(config::WgnxTunnelConcurrentFlows <=
              wgnx::tunnel::MaximumFlowsPerClient);

const char *StatusName(wgnx::tunnel::ProtocolStatus status) {
  using wgnx::tunnel::ProtocolStatus;
  switch (status) {
  case ProtocolStatus::Success:
    return "success";
  case ProtocolStatus::MalformedInput:
    return "malformed_input";
  case ProtocolStatus::UnsupportedOperation:
    return "unsupported_operation";
  case ProtocolStatus::IncompatibleApiVersion:
    return "incompatible_api_version";
  case ProtocolStatus::RouteNotCovered:
    return "route_not_covered";
  case ProtocolStatus::PeerUnavailable:
    return "peer_unavailable";
  case ProtocolStatus::TransportUnavailable:
    return "transport_unavailable";
  case ProtocolStatus::FlowQuotaExhausted:
    return "flow_quota_exhausted";
  case ProtocolStatus::DatagramTooLarge:
    return "datagram_too_large";
  case ProtocolStatus::QueueFull:
    return "queue_full";
  case ProtocolStatus::StaleHandle:
    return "stale_handle";
  case ProtocolStatus::FlowClosed:
    return "flow_closed";
  case ProtocolStatus::QueueEmpty:
    return "queue_empty";
  case ProtocolStatus::OutputBufferTooSmall:
    return "output_buffer_too_small";
  case ProtocolStatus::ReverseTupleExhausted:
    return "reverse_tuple_exhausted";
  }
  return "unknown";
}

bool ParseIpv4(const char *text, std::array<std::uint8_t, 4> *out_address) {
  return text != nullptr && out_address != nullptr &&
         inet_pton(AF_INET, text, out_address->data()) == 1;
}

std::uint64_t DeadlineAfterMilliseconds(std::uint32_t timeout_ms) {
  return armGetSystemTick() +
         ((armGetSystemTickFreq() * static_cast<std::uint64_t>(timeout_ms)) /
          1000U);
}

std::uint32_t NextPayloadByte(std::uint32_t *state) {
  *state ^= *state << 13U;
  *state ^= *state >> 17U;
  *state ^= *state << 5U;
  return *state;
}

void StoreBigEndian32(std::uint8_t *out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 24U);
  out[1] = static_cast<std::uint8_t>(value >> 16U);
  out[2] = static_cast<std::uint8_t>(value >> 8U);
  out[3] = static_cast<std::uint8_t>(value);
}

void BuildPayload(std::span<std::uint8_t> payload, std::uint32_t sequence,
                  std::uint32_t flow_index) {
  std::fill(payload.begin(), payload.end(), std::uint8_t{0});
  std::memcpy(payload.data(), "NXRVWG1", 7);
  StoreBigEndian32(payload.data() + 8, config::WgnxTunnelWorkloadId);
  StoreBigEndian32(payload.data() + 12, flow_index);
  StoreBigEndian32(payload.data() + 16, sequence);
  StoreBigEndian32(payload.data() + 20, config::WgnxTunnelPayloadSeed);
  std::uint32_t state =
      config::WgnxTunnelPayloadSeed ^ sequence ^ (flow_index * 0x9E3779B9U);
  for (std::size_t index = PayloadHeaderBytes; index < payload.size();
       ++index) {
    payload[index] = static_cast<std::uint8_t>(NextPayloadByte(&state));
  }
}

bool IsMatchingPayload(std::span<const std::uint8_t> actual,
                       std::span<const std::uint8_t> expected) {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

struct CompletionWaitResult {
  bool received_expected{false};
  bool terminal{false};
  std::uint32_t ignored{0};
  std::uint32_t wake_count{0};
  std::string detail;
};

CompletionWaitResult WaitForEcho(wgnx::tunnel::client::ScopedClient &client,
                                 Event *event, wgnx::tunnel::FlowHandle flow,
                                 std::span<const std::uint8_t> expected,
                                 std::uint32_t timeout_ms) {
  CompletionWaitResult result{};
  const std::uint64_t deadline = DeadlineAfterMilliseconds(timeout_ms);
  std::array<wgnx::tunnel::CompletionRecord, wgnx::tunnel::MaximumBatchEntries>
      completions{};
  std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadBytes> payload{};

  while (armGetSystemTick() < deadline) {
    const std::uint64_t remaining_ticks = deadline - armGetSystemTick();
    const std::uint64_t remaining_ns =
        (remaining_ticks * 1000000000ULL) / armGetSystemTickFreq();
    const Result wait_rc = eventWait(event, remaining_ns);
    if (R_FAILED(wait_rc)) {
      result.detail = "completion event wait rc=" + FormatResult(wait_rc);
      return result;
    }
    ++result.wake_count;

    for (;;) {
      std::uint32_t count = 0;
      wgnx::tunnel::ProtocolStatus status =
          wgnx::tunnel::ProtocolStatus::QueueEmpty;
      const Result rc = wgnx::tunnel::client::ReceiveCompletions(
          client, completions.data(), completions.size(), payload.data(),
          payload.size(), &count, &status);
      if (R_FAILED(rc)) {
        result.detail = "ReceiveCompletions CMIF rc=" + FormatResult(rc);
        return result;
      }
      if (status == wgnx::tunnel::ProtocolStatus::QueueEmpty) {
        break;
      }
      if (status != wgnx::tunnel::ProtocolStatus::Success) {
        result.detail =
            "ReceiveCompletions status=" + std::string(StatusName(status));
        return result;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        const auto &completion = completions[index];
        if (completion.type == wgnx::tunnel::CompletionType::FlowStateChanged &&
            completion.flow.value == flow.value) {
          result.terminal = true;
          result.detail =
              "flow terminal=" + std::to_string(static_cast<std::uint32_t>(
                                     completion.terminal_reason));
          return result;
        }
        if (completion.type != wgnx::tunnel::CompletionType::InboundDatagram ||
            completion.flow.value != flow.value ||
            completion.payload_offset > payload.size() ||
            completion.payload_size >
                payload.size() - completion.payload_offset) {
          ++result.ignored;
          continue;
        }
        const std::span<const std::uint8_t> received(
            payload.data() + completion.payload_offset,
            completion.payload_size);
        if (IsMatchingPayload(received, expected)) {
          result.received_expected = true;
          return result;
        }
        ++result.ignored;
      }
    }
  }
  result.detail = "completion deadline expired";
  return result;
}

} // namespace

ScenarioResult RunWgnxTunnelUdpWorkload(AppContext &ctx) {
  using namespace wgnx::tunnel;

  ScenarioResult result{.name = "wgnx_tunnel_udp_workload"};
  logger::Status(ctx, "Running wgnx:tun UDP workload to %s:%u",
                 config::WgnxTunnelDestinationIpv4,
                 config::WgnxTunnelDestinationPort);
  if (!client::IsServiceRunning()) {
    result.detail = "wgnx:tun is not running";
    return result;
  }

  client::ScopedRootService root;
  Result rc = root.Open();
  if (R_FAILED(rc)) {
    result.rc = rc;
    result.detail = "wgnx:tun root open failure";
    return result;
  }
  Capabilities root_capabilities{};
  rc = client::GetTunCapabilities(root, &root_capabilities);
  if (R_FAILED(rc) || root_capabilities.api_version != TunApiVersion) {
    result.rc = rc;
    result.detail =
        R_FAILED(rc)
            ? "GetTunApiVersion CMIF failure"
            : "API mismatch compiled=" + std::to_string(TunApiVersion) +
                  " actual=" + std::to_string(root_capabilities.api_version);
    return result;
  }

  client::ScopedClient tunnel_client;
  rc = client::OpenTunnelClient(root, &tunnel_client);
  if (R_FAILED(rc)) {
    result.rc = rc;
    result.detail = "OpenTunnelClient CMIF failure";
    return result;
  }
  Capabilities capabilities{};
  rc = client::GetCapabilities(tunnel_client, &capabilities);
  if (R_FAILED(rc) || capabilities.api_version != TunApiVersion ||
      config::WgnxTunnelPayloadBytes > capabilities.maximum_udp_payload_bytes) {
    result.rc = rc;
    result.detail = R_FAILED(rc) ? "GetCapabilities CMIF failure"
                                 : "capability bounds mismatch";
    return result;
  }

  std::array<RouteRecord, MaximumPolicyRoutes> routes{};
  RoutingPolicySnapshot policy{};
  rc = client::GetRoutingPolicySnapshot(tunnel_client, routes.data(),
                                        routes.size(), &policy);
  if (R_FAILED(rc)) {
    result.rc = rc;
    result.detail = "GetRoutingPolicySnapshot CMIF failure";
    return result;
  }

  std::array<std::uint8_t, 4> destination{};
  if (!ParseIpv4(config::WgnxTunnelDestinationIpv4, &destination)) {
    result.detail = "invalid configured workload destination";
    return result;
  }

  Handle event_handle = INVALID_HANDLE;
  rc = client::GetCompletionEvent(tunnel_client, &event_handle);
  if (R_FAILED(rc)) {
    result.rc = rc;
    result.detail = "GetCompletionEvent CMIF failure";
    return result;
  }
  Event completion_event{};
  eventLoadRemote(&completion_event, event_handle, false);

  std::array<FlowHandle, config::WgnxTunnelConcurrentFlows> flows{};
  std::uint32_t opened_flows = 0;
  for (; opened_flows < flows.size(); ++opened_flows) {
    const OpenConnectedUdpFlowRequest request{
        .remote = {.address = {destination[0], destination[1], destination[2],
                               destination[3]},
                   .port = config::WgnxTunnelDestinationPort,
                   .reserved = 0},
        .diagnostic_tag =
            (static_cast<std::uint64_t>(config::WgnxTunnelWorkloadId) << 32U) |
            opened_flows,
    };
    const auto opened = [&] {
      OpenConnectedUdpFlowResult value{};
      rc = client::OpenConnectedUdpFlow(tunnel_client, request, &value);
      return value;
    }();
    if (R_FAILED(rc) || opened.status != ProtocolStatus::Success) {
      result.rc = rc;
      result.detail = R_FAILED(rc) ? "OpenConnectedUdpFlow CMIF failure"
                                   : "OpenConnectedUdpFlow status=" +
                                         std::string(StatusName(opened.status));
      break;
    }
    flows[opened_flows] = opened.flow;
  }
  if (opened_flows != flows.size()) {
    for (std::uint32_t index = 0; index < opened_flows; ++index) {
      ProtocolStatus ignored{};
      static_cast<void>(
          client::CloseFlow(tunnel_client, flows[index], &ignored));
    }
    eventClose(&completion_event);
    return result;
  }

  std::array<std::uint8_t, config::WgnxTunnelPayloadBytes> payload{};
  std::uint32_t accepted = 0;
  std::uint32_t echoed = 0;
  std::uint32_t ignored_completions = 0;
  std::uint32_t event_wakes = 0;
  for (std::uint32_t sequence = 0; sequence < config::WgnxTunnelDatagramCount;
       ++sequence) {
    const std::uint32_t flow_index = sequence % flows.size();
    BuildPayload(payload, sequence, flow_index);
    const DatagramDescriptor descriptor{
        .flow = flows[flow_index],
        .payload_offset = 0,
        .payload_size = static_cast<std::uint32_t>(payload.size()),
        .client_tag =
            (static_cast<std::uint64_t>(config::WgnxTunnelWorkloadId) << 32U) |
            sequence,
    };
    DatagramDisposition disposition{};
    rc = client::SendUdpDatagram(tunnel_client, descriptor, payload.data(),
                                 payload.size(), &disposition);
    if (R_FAILED(rc) || disposition.status != ProtocolStatus::Success) {
      result.rc = rc;
      result.detail = R_FAILED(rc)
                          ? "SendUdpDatagram CMIF failure"
                          : "SendUdpDatagram status=" +
                                std::string(StatusName(disposition.status));
      break;
    }
    ++accepted;
    result.bytes_sent += payload.size();

    if (config::WgnxTunnelEchoReplies) {
      const CompletionWaitResult wait =
          WaitForEcho(tunnel_client, &completion_event, flows[flow_index],
                      payload, config::WgnxTunnelReceiveDeadlineMs);
      ignored_completions += wait.ignored;
      event_wakes += wait.wake_count;
      if (!wait.received_expected) {
        result.detail = wait.detail + " sequence=" + std::to_string(sequence);
        break;
      }
      ++echoed;
      result.bytes_received += payload.size();
    }
    if (config::WgnxTunnelPacingMs != 0) {
      SleepMilliseconds(config::WgnxTunnelPacingMs);
    }
  }

  for (FlowHandle flow : flows) {
    ProtocolStatus close_status{};
    const Result close_rc =
        client::CloseFlow(tunnel_client, flow, &close_status);
    logger::Log(ctx, "scenario=wgnx_tunnel_udp_workload close rc=%s status=%s",
                FormatResult(close_rc).c_str(), StatusName(close_status));
  }
  eventClose(&completion_event);

  if (accepted == config::WgnxTunnelDatagramCount &&
      (!config::WgnxTunnelEchoReplies || echoed == accepted)) {
    result.success = true;
    result.detail =
        "api=" + std::to_string(capabilities.api_version) +
        " policy=" + std::to_string(policy.policy_generation) +
        " routes=" + std::to_string(policy.route_count) +
        " flows=" + std::to_string(flows.size()) +
        " accepted=" + std::to_string(accepted) +
        " echoed=" + std::to_string(echoed) +
        " ignored_completions=" + std::to_string(ignored_completions) +
        " event_wakes=" + std::to_string(event_wakes);
  }
  return result;
}

} // namespace requester
