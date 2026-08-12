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

#include "logger.hpp"
#include "runtime_config.hpp"
#include "udp_workload_metrics.hpp"
#include "wgnx/tunnel_client.hpp"

namespace toolbox {

namespace {

constexpr std::size_t PayloadHeaderBytes = 24;
constexpr std::uint32_t MaximumQueueFullRetriesPerDatagram = 16;
constexpr char TcpExpectedReply[] = "NXRV TCP ACK\r\n";

const char* StatusName(wgnx::tunnel::ProtocolStatus status) {
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
    case ProtocolStatus::PayloadTooLarge:
        return "payload_too_large";
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
    case ProtocolStatus::TunnelBlockedByPolicy:
        return "tunnel_blocked_by_policy";
    case ProtocolStatus::WrongFlowKind:
        return "wrong_flow_kind";
    case ProtocolStatus::NotConnected:
        return "not_connected";
    case ProtocolStatus::LocalWriteClosed:
        return "local_write_closed";
    }
    return "unknown";
}

const char* TerminalReasonName(wgnx::tunnel::FlowTerminalReason reason) {
    using wgnx::tunnel::FlowTerminalReason;
    switch (reason) {
    case FlowTerminalReason::None:
        return "none";
    case FlowTerminalReason::ClientClosed:
        return "client_closed";
    case FlowTerminalReason::PeerDeactivated:
        return "peer_deactivated";
    case FlowTerminalReason::PeerActivationChanged:
        return "peer_activation_changed";
    case FlowTerminalReason::PolicyInvalidated:
        return "policy_invalidated";
    case FlowTerminalReason::SysmoduleShutdown:
        return "sysmodule_shutdown";
    case FlowTerminalReason::RemoteClosed:
        return "remote_closed";
    case FlowTerminalReason::ResetDuringConnect:
        return "reset_during_connect";
    case FlowTerminalReason::ResetAfterConnect:
        return "reset_after_connect";
    case FlowTerminalReason::ConnectTimedOut:
        return "connect_timed_out";
    case FlowTerminalReason::RouteLost:
        return "route_lost";
    case FlowTerminalReason::LocalResourceFailure:
        return "local_resource_failure";
    }
    return "unknown";
}

bool ParseIpv4(const char* text, std::array<std::uint8_t, 4>* out_address) {
    return text != nullptr && out_address != nullptr && inet_pton(AF_INET, text, out_address->data()) == 1;
}

std::uint64_t DeadlineAfterMilliseconds(std::uint32_t timeout_ms) {
    return armGetSystemTick() + ((armGetSystemTickFreq() * static_cast<std::uint64_t>(timeout_ms)) / 1000U);
}

std::uint32_t NextPayloadByte(std::uint32_t* state) {
    *state ^= *state << 13U;
    *state ^= *state >> 17U;
    *state ^= *state << 5U;
    return *state;
}

void StoreBigEndian32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>(value >> 16U);
    out[2] = static_cast<std::uint8_t>(value >> 8U);
    out[3] = static_cast<std::uint8_t>(value);
}

void BuildPayload(
    std::span<std::uint8_t> payload, std::uint32_t sequence, std::uint32_t flow_index, const TunnelUdpWorkloadConfig& config
) {
    std::fill(payload.begin(), payload.end(), std::uint8_t{0});
    std::memcpy(payload.data(), "NXRVWG1", 7);
    StoreBigEndian32(payload.data() + 8, config.workload_id);
    StoreBigEndian32(payload.data() + 12, flow_index);
    StoreBigEndian32(payload.data() + 16, sequence);
    StoreBigEndian32(payload.data() + 20, config.payload_seed);
    std::uint32_t state = config.payload_seed ^ sequence ^ (flow_index * 0x9E3779B9U);
    for (std::size_t index = PayloadHeaderBytes; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(NextPayloadByte(&state));
    }
}

bool IsMatchingPayload(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected) {
    return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin());
}

struct CompletionWaitResult {
    Result rc{0};
    bool received_expected{false};
    bool writable{false};
    bool terminal{false};
    bool service_lost{false};
    bool open{false};
    bool remote_write_closed{false};
    bool deadline_expired{false};
    bool malformed_reply{false};
    bool premature_eof{false};
    wgnx::tunnel::FlowTerminalReason terminal_reason{wgnx::tunnel::FlowTerminalReason::None};
    std::uint32_t ignored{0};
    std::uint32_t wake_count{0};
    std::string detail;
};

struct TunnelServiceAvailability {
    Result rc{0};
    bool present{false};
};

TunnelServiceAvailability QueryTunnelServiceAvailability() {
    // Atmosphere's SM extension is a read-only presence query.
    // Do not use smRegisterService as a liveness probe because it mutates SM
    // state and can race a real service registration.
    const SmServiceName service_name = smEncodeName(wgnx::tunnel::ServiceName);
    std::uint8_t present = 0;
    const Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, service_name, present);
    return {.rc = rc, .present = R_SUCCEEDED(rc) && present != 0};
}

CompletionWaitResult WaitForEcho(
    wgnx::tunnel::client::ScopedClient& client,
    Event* event,
    wgnx::tunnel::FlowHandle flow,
    std::span<const std::uint8_t> expected,
    std::uint32_t timeout_ms
) {
    CompletionWaitResult result{};
    const std::uint64_t deadline = DeadlineAfterMilliseconds(timeout_ms);
    std::array<wgnx::tunnel::CompletionRecord, wgnx::tunnel::MaximumBatchEntries> completions{};
    std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadStorageBytes> payload{};

    while (armGetSystemTick() < deadline) {
        const std::uint64_t remaining_ticks = deadline - armGetSystemTick();
        const std::uint64_t remaining_ns = (remaining_ticks * 1000000000ULL) / armGetSystemTickFreq();
        const Result wait_rc = eventWait(event, remaining_ns);
        if (R_FAILED(wait_rc)) {
            result.rc = wait_rc;
            result.detail = "completion event wait rc=" + FormatResult(wait_rc);
            return result;
        }
        ++result.wake_count;

        for (;;) {
            std::uint32_t count = 0;
            wgnx::tunnel::ProtocolStatus status = wgnx::tunnel::ProtocolStatus::QueueEmpty;
            const Result rc = wgnx::tunnel::client::ReceiveCompletions(
                client,
                completions.data(),
                completions.size(),
                payload.data(),
                payload.size(),
                &count,
                &status
            );
            if (R_FAILED(rc)) {
                // API v5 permits this exact wake-without-record sequence while the
                // sysmodule performs an orderly shutdown and closes this session.
                result.terminal = true;
                result.service_lost = true;
                result.rc = rc;
                result.detail = "wgnx:tun service closed after completion wake "
                                "ReceiveCompletions CMIF rc=" +
                                FormatResult(rc);
                return result;
            }
            if (status == wgnx::tunnel::ProtocolStatus::QueueEmpty) {
                break;
            }
            if (status != wgnx::tunnel::ProtocolStatus::Success) {
                result.detail = "ReceiveCompletions status=" + std::string(StatusName(status));
                return result;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto& completion = completions[index];
                if (completion.type == wgnx::tunnel::CompletionType::FlowStateChanged && completion.flow.value == flow.value) {
                    result.terminal = true;
                    result.detail = "flow terminal=" + std::to_string(static_cast<std::uint32_t>(completion.terminal_reason));
                    return result;
                }
                if (completion.type != wgnx::tunnel::CompletionType::InboundUdpDatagram || completion.flow.value != flow.value ||
                    completion.payload_offset > payload.size() || completion.payload_size > payload.size() - completion.payload_offset) {
                    ++result.ignored;
                    continue;
                }
                const std::span<const std::uint8_t> received(payload.data() + completion.payload_offset, completion.payload_size);
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

CompletionWaitResult WaitForWritable(
    wgnx::tunnel::client::ScopedClient& client, Event* event, wgnx::tunnel::FlowHandle flow, std::uint32_t timeout_ms
) {
    CompletionWaitResult result{};
    const std::uint64_t deadline = DeadlineAfterMilliseconds(timeout_ms);
    std::array<wgnx::tunnel::CompletionRecord, wgnx::tunnel::MaximumBatchEntries> completions{};
    std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadStorageBytes> payload{};

    while (armGetSystemTick() < deadline) {
        const std::uint64_t remaining_ticks = deadline - armGetSystemTick();
        const std::uint64_t remaining_ns = (remaining_ticks * 1000000000ULL) / armGetSystemTickFreq();
        const Result wait_rc = eventWait(event, remaining_ns);
        if (R_FAILED(wait_rc)) {
            result.rc = wait_rc;
            result.detail = "completion event wait rc=" + FormatResult(wait_rc);
            return result;
        }
        ++result.wake_count;

        for (;;) {
            std::uint32_t count = 0;
            wgnx::tunnel::ProtocolStatus status = wgnx::tunnel::ProtocolStatus::QueueEmpty;
            const Result rc = wgnx::tunnel::client::ReceiveCompletions(
                client,
                completions.data(),
                completions.size(),
                payload.data(),
                payload.size(),
                &count,
                &status
            );
            if (R_FAILED(rc)) {
                // API v5 permits this exact wake-without-record sequence while the
                // sysmodule performs an orderly shutdown and closes this session.
                result.terminal = true;
                result.service_lost = true;
                result.rc = rc;
                result.detail = "wgnx:tun service closed after completion wake "
                                "ReceiveCompletions CMIF rc=" +
                                FormatResult(rc);
                return result;
            }
            if (status == wgnx::tunnel::ProtocolStatus::QueueEmpty) {
                break;
            }
            if (status != wgnx::tunnel::ProtocolStatus::Success) {
                result.detail = "ReceiveCompletions status=" + std::string(StatusName(status));
                return result;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto& completion = completions[index];
                if (completion.type == wgnx::tunnel::CompletionType::FlowStateChanged && completion.flow.value == flow.value) {
                    result.terminal = true;
                    result.detail = "flow terminal=" + std::to_string(static_cast<std::uint32_t>(completion.terminal_reason));
                    return result;
                }
                if (completion.type == wgnx::tunnel::CompletionType::Writable && completion.flow.value == flow.value) {
                    result.writable = true;
                    return result;
                }
                ++result.ignored;
            }
        }
    }
    result.detail = "writable completion deadline expired";
    return result;
}

CompletionWaitResult WaitForTcpProgress(
    wgnx::tunnel::client::ScopedClient& client,
    Event* event,
    wgnx::tunnel::FlowHandle flow,
    bool require_open,
    bool require_reply,
    bool require_remote_write_closed,
    std::uint32_t timeout_ms
) {
    using namespace wgnx::tunnel;

    CompletionWaitResult result{};
    const std::uint64_t deadline = DeadlineAfterMilliseconds(timeout_ms);
    std::array<CompletionRecord, MaximumBatchEntries> completions{};
    std::array<std::uint8_t, MaximumTcpWriteStorageBytes> payload{};
    std::array<std::uint8_t, sizeof(TcpExpectedReply) - 1> reply{};
    std::size_t reply_size{};

    while (armGetSystemTick() < deadline) {
        const std::uint64_t remaining_ticks = deadline - armGetSystemTick();
        const std::uint64_t remaining_ns = (remaining_ticks * 1000000000ULL) / armGetSystemTickFreq();
        const Result wait_rc = eventWait(event, remaining_ns);
        if (R_FAILED(wait_rc)) {
            result.rc = wait_rc;
            result.deadline_expired = wait_rc == KERNELRESULT(TimedOut);
            result.detail = result.deadline_expired ? (require_open ? "TCP connect deadline expired" : "TCP reply deadline expired")
                                                    : "TCP completion event wait rc=" + FormatResult(wait_rc);
            return result;
        }
        ++result.wake_count;

        for (;;) {
            std::uint32_t count{};
            ProtocolStatus status = ProtocolStatus::QueueEmpty;
            const Result rc =
                client::ReceiveCompletions(client, completions.data(), completions.size(), payload.data(), payload.size(), &count, &status);
            if (R_FAILED(rc)) {
                result.rc = rc;
                result.terminal = true;
                result.service_lost = true;
                result.detail = "wgnx:tun service closed after TCP completion wake ReceiveCompletions CMIF rc=" + FormatResult(rc);
                return result;
            }
            if (status == ProtocolStatus::QueueEmpty) {
                break;
            }
            if (status != ProtocolStatus::Success) {
                result.detail = "TCP ReceiveCompletions status=" + std::string(StatusName(status));
                return result;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const CompletionRecord& completion = completions[index];
                if (completion.flow.value != flow.value || completion.flow_kind != FlowKind::Tcp) {
                    ++result.ignored;
                    continue;
                }
                if (completion.type == CompletionType::FlowStateChanged) {
                    if (completion.flow_state == FlowState::Closed) {
                        result.terminal = true;
                        result.terminal_reason = completion.terminal_reason;
                        result.detail = "TCP flow terminal=" + std::string(TerminalReasonName(completion.terminal_reason));
                        return result;
                    }
                    result.open = completion.flow_state == FlowState::Open;
                    FlowStateResult state{};
                    const Result state_rc = wgnx::tunnel::client::GetFlowState(client, flow, &state);
                    if (R_FAILED(state_rc) || state.status != ProtocolStatus::Success) {
                        result.rc = state_rc;
                        result.detail = R_FAILED(state_rc) ? "GetFlowState CMIF failure"
                                                           : "GetFlowState status=" + std::string(StatusName(state.status));
                        return result;
                    }
                    result.remote_write_closed = (state.stream_flags & FlowStreamFlagRemoteWriteOpen) == FlowStreamFlagNone;
                    continue;
                }
                if (completion.type != CompletionType::InboundTcpStream || completion.payload_offset > payload.size() ||
                    completion.payload_size > payload.size() - completion.payload_offset) {
                    ++result.ignored;
                    continue;
                }
                if (reply_size + completion.payload_size > reply.size()) {
                    result.malformed_reply = true;
                    continue;
                }
                std::memcpy(reply.data() + reply_size, payload.data() + completion.payload_offset, completion.payload_size);
                reply_size += completion.payload_size;
            }
            const bool reply_valid = reply_size == reply.size() && std::memcmp(reply.data(), TcpExpectedReply, reply.size()) == 0;
            if (require_reply && reply_size == reply.size() && !reply_valid) {
                result.malformed_reply = true;
            }
            if (require_reply && result.remote_write_closed && !reply_valid) {
                result.premature_eof = !result.malformed_reply;
                result.detail = result.malformed_reply ? "TCP malformed reply before remote EOF" : "TCP premature remote EOF";
                return result;
            }
            if (result.malformed_reply) {
                result.detail = "TCP malformed reply";
                return result;
            }
            if ((require_open && !result.open) || (require_reply && !reply_valid) ||
                (require_remote_write_closed && !result.remote_write_closed)) {
                continue;
            }
            result.received_expected = true;
            return result;
        }
    }
    result.deadline_expired = true;
    result.detail = require_open ? "TCP connect deadline expired" : "TCP reply deadline expired";
    return result;
}

} // namespace

ScenarioResult RunWgnxTunnelTcpExchange(
    AppContext& ctx, const RuntimeProfile& profile, const std::uint32_t workload_id, const TcpScenarioConfig& config
) {
    using namespace wgnx::tunnel;

    ScenarioResult result{.name = "wgnx_tunnel_tcp_exchange"};
    const TunnelServiceAvailability availability = QueryTunnelServiceAvailability();
    if (R_FAILED(availability.rc) || !availability.present) {
        result.rc = availability.rc;
        result.detail = R_FAILED(availability.rc) ? "wgnx:tun availability query failed" : "wgnx:tun is not running";
        return result;
    }

    client::ScopedRootService root;
    Result rc = root.Open();
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "wgnx:tun root open failure";
        return result;
    }
    Capabilities capabilities{};
    rc = client::GetTunCapabilities(root, &capabilities);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "GetCapabilities CMIF failure";
        return result;
    }
    if (capabilities.api_version != TunApiVersion) {
        result.detail = "API mismatch compiled=" + std::to_string(TunApiVersion) + " actual=" + std::to_string(capabilities.api_version);
        return result;
    }
    if ((capabilities.capability_mask & CapabilityMask(Capability::ConnectedIpv4Tcp)) == 0) {
        result.detail = "connected TCP capability unavailable";
        return result;
    }

    std::array<std::uint8_t, 4> destination{};
    if (!ParseIpv4(profile.tunnel_destination_ipv4.c_str(), &destination)) {
        result.detail = "invalid configured tunnel TCP destination";
        return result;
    }
    client::ScopedClient tunnel_client;
    rc = client::OpenTunnelClient(root, &tunnel_client);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "OpenTunnelClient CMIF failure";
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

    const OpenConnectedFlowRequest request{
        .remote =
            {.address = {destination[0], destination[1], destination[2], destination[3]},
             .port = profile.tcp_destination_port,
             .reserved = 0},
        .kind = FlowKind::Tcp,
        .reserved = 0,
        .diagnostic_tag = workload_id,
    };
    OpenConnectedFlowResult opened{};
    rc = client::OpenConnectedFlow(tunnel_client, request, &opened);
    if (R_FAILED(rc) || opened.status != ProtocolStatus::Success || opened.advertised_local.port == 0) {
        eventClose(&completion_event);
        result.rc = rc;
        result.detail = R_FAILED(rc)                               ? "OpenConnectedFlow CMIF failure"
                        : opened.status != ProtocolStatus::Success ? "OpenConnectedFlow status=" + std::string(StatusName(opened.status))
                                                                   : "OpenConnectedFlow did not publish a virtual local endpoint";
        return result;
    }
    const auto close_flow = [&] {
        ProtocolStatus ignored{};
        static_cast<void>(client::CloseFlow(tunnel_client, opened.flow, &ignored));
    };

    const CompletionWaitResult connected =
        WaitForTcpProgress(tunnel_client, &completion_event, opened.flow, true, false, false, config.receive_deadline_ms);
    if (!connected.received_expected) {
        close_flow();
        eventClose(&completion_event);
        result.rc = connected.rc;
        result.detail = "TCP connect failure: " + connected.detail;
        return result;
    }
    FlowStateResult state{};
    rc = client::GetFlowState(tunnel_client, opened.flow, &state);
    if (R_FAILED(rc) || state.status != ProtocolStatus::Success || state.state != FlowState::Open || state.flow_kind != FlowKind::Tcp ||
        state.advertised_local.port == 0) {
        close_flow();
        eventClose(&completion_event);
        result.rc = rc;
        result.detail = R_FAILED(rc) ? "GetFlowState CMIF failure" : "TCP flow did not publish an open virtual local endpoint";
        return result;
    }

    std::array<char, 32> request_bytes{};
    const int request_size = std::snprintf(request_bytes.data(), request_bytes.size(), "NXRV TCP %u\r\n", workload_id);
    if (request_size <= 0 || static_cast<std::size_t>(request_size) >= request_bytes.size() ||
        static_cast<std::size_t>(request_size) > capabilities.maximum_tcp_write_bytes) {
        close_flow();
        eventClose(&completion_event);
        result.detail = "TCP request exceeds the configured stream-write bound";
        return result;
    }
    const PayloadRange range{
        .flow = opened.flow,
        .payload_offset = 0,
        .payload_size = static_cast<std::uint32_t>(request_size),
        .client_tag = workload_id,
    };
    PayloadResult write{};
    rc = client::WriteTcpStream(tunnel_client, range, request_bytes.data(), static_cast<std::size_t>(request_size), &write);
    if (R_FAILED(rc) || write.status != ProtocolStatus::Success || write.accepted_bytes != static_cast<std::uint32_t>(request_size)) {
        close_flow();
        eventClose(&completion_event);
        result.rc = rc;
        result.detail = R_FAILED(rc) ? "WriteTcpStream CMIF failure" : "WriteTcpStream status=" + std::string(StatusName(write.status));
        return result;
    }
    ProtocolStatus shutdown_status{};
    rc = client::ShutdownTcpWrite(tunnel_client, opened.flow, &shutdown_status);
    if (R_FAILED(rc) || shutdown_status != ProtocolStatus::Success) {
        close_flow();
        eventClose(&completion_event);
        result.rc = rc;
        result.detail =
            R_FAILED(rc) ? "ShutdownTcpWrite CMIF failure" : "ShutdownTcpWrite status=" + std::string(StatusName(shutdown_status));
        return result;
    }
    const CompletionWaitResult reply =
        WaitForTcpProgress(tunnel_client, &completion_event, opened.flow, false, true, true, config.receive_deadline_ms);
    eventClose(&completion_event);
    if (!reply.received_expected) {
        close_flow();
        result.rc = reply.rc;
        result.detail = "TCP reply failure: " + reply.detail;
        return result;
    }
    ProtocolStatus close_status{};
    const Result close_rc = client::CloseFlow(tunnel_client, opened.flow, &close_status);
    if (R_FAILED(close_rc) || close_status != ProtocolStatus::Success) {
        result.rc = close_rc;
        result.detail = R_FAILED(close_rc) ? "CloseFlow CMIF failure" : "CloseFlow status=" + std::string(StatusName(close_status));
        return result;
    }
    result.success = true;
    result.bytes_sent = static_cast<std::size_t>(request_size);
    result.bytes_received = sizeof(TcpExpectedReply) - 1;
    result.detail = "api=" + std::to_string(capabilities.api_version) +
                    " virtual_local_port=" + std::to_string(state.advertised_local.port) +
                    " accepted=" + std::to_string(write.accepted_bytes) +
                    " event_wakes=" + std::to_string(connected.wake_count + reply.wake_count) + " reply=validated eof=observed close=ok";
    return result;
}

ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx, const TunnelUdpWorkloadConfig& config) {
    using namespace wgnx::tunnel;

    ScenarioResult result{.name = "wgnx_tunnel_udp_workload"};
    logger::Status(ctx, "Running wgnx:tun UDP workload to %s:%u", config.destination_ipv4.c_str(), config.destination_port);
    const TunnelServiceAvailability availability = QueryTunnelServiceAvailability();
    if (R_FAILED(availability.rc)) {
        result.rc = availability.rc;
        result.detail = "wgnx:tun availability query failed";
        return result;
    }
    if (!availability.present) {
        result.detail = "wgnx:tun is not running";
        return result;
    }
    if (config.concurrent_flows == 0 || config.concurrent_flows > MaximumFlowsPerClient) {
        result.detail = "configured WGNX workload flow count is unsupported";
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
        result.detail = R_FAILED(rc) ? "GetCapabilities CMIF failure"
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
    if (R_FAILED(rc) || capabilities.api_version != TunApiVersion || config.payload_bytes > capabilities.maximum_udp_payload_bytes) {
        result.rc = rc;
        result.detail = R_FAILED(rc) ? "GetCapabilities CMIF failure" : "capability bounds mismatch";
        return result;
    }

    std::array<RouteRecord, MaximumPolicyRoutes> routes{};
    RoutingPolicySnapshot policy{};
    rc = client::GetRoutingPolicySnapshot(tunnel_client, routes.data(), routes.size(), &policy);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "GetRoutingPolicySnapshot CMIF failure";
        return result;
    }

    std::array<std::uint8_t, 4> destination{};
    if (!ParseIpv4(config.destination_ipv4.c_str(), &destination)) {
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

    std::array<FlowHandle, MaximumFlowsPerClient> flows{};
    std::uint32_t opened_flows = 0;
    for (; opened_flows < config.concurrent_flows; ++opened_flows) {
        const OpenConnectedFlowRequest request{
            .remote =
                {.address = {destination[0], destination[1], destination[2], destination[3]},
                 .port = config.destination_port,
                 .reserved = 0},
            .kind = FlowKind::Udp,
            .reserved = 0,
            .diagnostic_tag = (static_cast<std::uint64_t>(config.workload_id) << 32U) | opened_flows,
        };
        const auto opened = [&] {
            OpenConnectedFlowResult value{};
            rc = client::OpenConnectedFlow(tunnel_client, request, &value);
            return value;
        }();
        if (R_FAILED(rc) || opened.status != ProtocolStatus::Success || opened.advertised_local.port == 0) {
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "OpenConnectedFlow CMIF failure"
                            : opened.status != ProtocolStatus::Success
                                ? "OpenConnectedFlow status=" + std::string(StatusName(opened.status))
                                : "OpenConnectedFlow did not publish a virtual local endpoint";
            break;
        }
        flows[opened_flows] = opened.flow;
    }
    if (opened_flows != config.concurrent_flows) {
        for (std::uint32_t index = 0; index < opened_flows; ++index) {
            ProtocolStatus ignored{};
            static_cast<void>(client::CloseFlow(tunnel_client, flows[index], &ignored));
        }
        eventClose(&completion_event);
        return result;
    }

    std::array<std::uint8_t, MaximumUdpPayloadStorageBytes> payload_storage{};
    const std::span<std::uint8_t> payload(payload_storage.data(), config.payload_bytes);
    const std::uint64_t tick_frequency = armGetSystemTickFreq();
    UdpWorkloadMetrics workload_metrics;
    std::uint32_t accepted = 0;
    std::uint32_t echoed = 0;
    std::uint32_t ignored_completions = 0;
    std::uint32_t event_wakes = 0;
    std::uint32_t queue_full_events = 0;
    bool tunnel_service_lost = false;
    const auto log_metrics = [&] {
        logger::Log(
            ctx,
            "[udp-workload-summary] kind=wgnx-tun workload=%u scope=workload flow=all "
            "attempted=%u accepted=%u submitted_bytes=%llu submission_elapsed_ns=%llu echoed=%u received_bytes=%llu "
            "rtt_samples=%u rtt_min_ns=%llu rtt_mean_ns=%llu rtt_p50_upper_ns=%llu "
            "rtt_p95_upper_ns=%llu rtt_p99_upper_ns=%llu rtt_max_ns=%llu queue_full_events=%u",
            config.workload_id,
            workload_metrics.attempted_datagrams(),
            workload_metrics.submitted_datagrams(),
            static_cast<unsigned long long>(workload_metrics.submitted_bytes()),
            static_cast<unsigned long long>(workload_metrics.SubmissionElapsedNanoseconds(tick_frequency)),
            workload_metrics.echoed_datagrams(),
            static_cast<unsigned long long>(workload_metrics.echoed_bytes()),
            workload_metrics.echoed_datagrams(),
            static_cast<unsigned long long>(workload_metrics.RttMinimumNanoseconds()),
            static_cast<unsigned long long>(workload_metrics.RttMeanNanoseconds()),
            static_cast<unsigned long long>(workload_metrics.RttPercentileUpperNanoseconds(50)),
            static_cast<unsigned long long>(workload_metrics.RttPercentileUpperNanoseconds(95)),
            static_cast<unsigned long long>(workload_metrics.RttPercentileUpperNanoseconds(99)),
            static_cast<unsigned long long>(workload_metrics.RttMaximumNanoseconds()),
            queue_full_events
        );
    };
    for (std::uint32_t sequence = 0; sequence < config.datagram_count; ++sequence) {
        const std::uint32_t flow_index = sequence % config.concurrent_flows;
        BuildPayload(payload, sequence, flow_index, config);
        const PayloadRange descriptor{
            .flow = flows[flow_index],
            .payload_offset = 0,
            .payload_size = static_cast<std::uint32_t>(payload.size()),
            .client_tag = (static_cast<std::uint64_t>(config.workload_id) << 32U) | sequence,
        };
        const std::uint64_t send_start_tick = armGetSystemTick();
        workload_metrics.RecordAttempt();
        std::uint32_t queue_full_retries_for_datagram = 0;
        for (;;) {
            PayloadResult disposition{};
            rc = client::SendUdpDatagram(tunnel_client, descriptor, payload.data(), payload.size(), &disposition);
            if (R_FAILED(rc)) {
                result.rc = rc;
                result.detail = "wgnx:tun service closed during SendUdpDatagram "
                                "CMIF rc=" +
                                FormatResult(rc);
                tunnel_service_lost = true;
                break;
            }
            if (disposition.status == ProtocolStatus::Success) {
                break;
            }
            if (disposition.status != ProtocolStatus::QueueFull || queue_full_retries_for_datagram == MaximumQueueFullRetriesPerDatagram) {
                result.detail = "SendUdpDatagram status=" + std::string(StatusName(disposition.status)) +
                                " sequence=" + std::to_string(sequence) + " queue_full_events=" + std::to_string(queue_full_events) +
                                " retries_for_datagram=" + std::to_string(queue_full_retries_for_datagram);
                break;
            }

            ++queue_full_events;
            ++queue_full_retries_for_datagram;
            const CompletionWaitResult wait =
                WaitForWritable(tunnel_client, &completion_event, flows[flow_index], config.receive_deadline_ms);
            ignored_completions += wait.ignored;
            event_wakes += wait.wake_count;
            if (!wait.writable) {
                result.rc = wait.rc;
                tunnel_service_lost = wait.service_lost;
                result.detail = wait.detail + " waiting_for_writable sequence=" + std::to_string(sequence) +
                                " queue_full_events=" + std::to_string(queue_full_events) +
                                " retries_for_datagram=" + std::to_string(queue_full_retries_for_datagram);
                break;
            }
        }
        if (R_FAILED(rc) || !result.detail.empty()) {
            break;
        }
        ++accepted;
        result.bytes_sent += payload.size();
        const std::uint64_t accepted_tick = armGetSystemTick();
        workload_metrics.RecordSubmission(accepted_tick, payload.size());

        if (config.echo_replies) {
            const CompletionWaitResult wait =
                WaitForEcho(tunnel_client, &completion_event, flows[flow_index], payload, config.receive_deadline_ms);
            ignored_completions += wait.ignored;
            event_wakes += wait.wake_count;
            if (!wait.received_expected) {
                result.rc = wait.rc;
                tunnel_service_lost = wait.service_lost;
                result.detail = wait.detail + " sequence=" + std::to_string(sequence);
                break;
            }
            ++echoed;
            result.bytes_received += payload.size();
            const std::uint64_t received_tick = armGetSystemTick();
            workload_metrics.RecordEcho(send_start_tick, received_tick, tick_frequency, payload.size());
        }
        if (config.pacing_ms != 0) {
            SleepMilliseconds(config.pacing_ms);
        }
    }

    if (tunnel_service_lost) {
        logger::Log(
            ctx,
            "scenario=wgnx_tunnel_udp_workload skip CloseFlow because "
            "wgnx:tun closed"
        );
    } else {
        for (std::uint32_t index = 0; index < config.concurrent_flows; ++index) {
            const FlowHandle flow = flows[index];
            ProtocolStatus close_status{};
            const Result close_rc = client::CloseFlow(tunnel_client, flow, &close_status);
            logger::Log(
                ctx,
                "scenario=wgnx_tunnel_udp_workload close rc=%s status=%s",
                FormatResult(close_rc).c_str(),
                StatusName(close_status)
            );
        }
    }
    eventClose(&completion_event);
    log_metrics();

    if (accepted == config.datagram_count && (!config.echo_replies || echoed == accepted)) {
        result.success = true;
        result.detail = "api=" + std::to_string(capabilities.api_version) + " policy=" + std::to_string(policy.policy_generation) +
                        " routes=" + std::to_string(policy.route_count) + " flows=" + std::to_string(config.concurrent_flows) +
                        " accepted=" + std::to_string(accepted) + " echoed=" + std::to_string(echoed) +
                        " queue_full_events=" + std::to_string(queue_full_events) +
                        " ignored_completions=" + std::to_string(ignored_completions) + " event_wakes=" + std::to_string(event_wakes);
        if (config.echo_replies) {
            result.detail += " avg_echo_latency_ms=" + std::to_string(workload_metrics.RttMeanMilliseconds());
        }
    }
    return result;
}

ScenarioResult RunWgnxTunnelContractValidation(
    AppContext& ctx, const TunnelUdpWorkloadConfig& workload, const TunnelContractValidationConfig& config
) {
    using namespace wgnx::tunnel;

    ScenarioResult result{.name = "wgnx_tunnel_contract_validation"};
    if (!config.verify_cloned_session_lifetime && !config.verify_mixed_batch) {
        result.detail = "no contract validations are enabled";
        return result;
    }
    logger::Status(
        ctx,
        "Running wgnx:tun contract validation clone_lifetime=%u mixed_batch=%u",
        static_cast<unsigned>(config.verify_cloned_session_lifetime),
        static_cast<unsigned>(config.verify_mixed_batch)
    );
    const TunnelServiceAvailability availability = QueryTunnelServiceAvailability();
    if (R_FAILED(availability.rc)) {
        result.rc = availability.rc;
        result.detail = "wgnx:tun availability query failed";
        return result;
    }
    if (!availability.present) {
        result.detail = "wgnx:tun is not running";
        return result;
    }

    std::array<std::uint8_t, 4> destination{};
    if (!ParseIpv4(workload.destination_ipv4.c_str(), &destination)) {
        result.detail = "invalid configured workload destination";
        return result;
    }

    client::ScopedRootService root;
    Result rc = root.Open();
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "wgnx:tun root open failure";
        return result;
    }
    Capabilities capabilities{};
    rc = client::GetTunCapabilities(root, &capabilities);
    if (R_FAILED(rc) || capabilities.api_version != TunApiVersion) {
        result.rc = rc;
        result.detail = R_FAILED(rc) ? "GetCapabilities CMIF failure" : "wgnx:tun API version mismatch";
        return result;
    }
    if (workload.payload_bytes > capabilities.maximum_udp_payload_bytes) {
        result.detail = "configured payload exceeds wgnx:tun capability";
        return result;
    }

    client::ScopedClient primary;
    rc = client::OpenTunnelClient(root, &primary);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "OpenTunnelClient CMIF failure";
        return result;
    }

    const OpenConnectedFlowRequest request{
        .remote =
            {.address = {destination[0], destination[1], destination[2], destination[3]}, .port = workload.destination_port, .reserved = 0},
        .kind = FlowKind::Udp,
        .reserved = 0,
        .diagnostic_tag = (static_cast<std::uint64_t>(workload.workload_id) << 32U) | 0x434F4E54U,
    };
    const auto open_flow = [&](client::ScopedClient& tunnel_client, FlowHandle* out_flow) -> bool {
        OpenConnectedFlowResult opened{};
        rc = client::OpenConnectedFlow(tunnel_client, request, &opened);
        if (R_FAILED(rc) || opened.status != ProtocolStatus::Success || opened.advertised_local.port == 0) {
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "OpenConnectedFlow CMIF failure"
                            : opened.status != ProtocolStatus::Success
                                ? "OpenConnectedFlow status=" + std::string(StatusName(opened.status))
                                : "OpenConnectedFlow did not publish a virtual local endpoint";
            return false;
        }
        *out_flow = opened.flow;
        return true;
    };
    const auto close_flow = [](client::ScopedClient& tunnel_client, FlowHandle* flow) {
        if (flow->value == 0 || !tunnel_client.IsOpen()) {
            return;
        }
        ProtocolStatus status{};
        static_cast<void>(client::CloseFlow(tunnel_client, *flow, &status));
        *flow = {};
    };
    const auto load_event = [&](client::ScopedClient& tunnel_client, Event* out_event) -> bool {
        Handle handle = INVALID_HANDLE;
        rc = client::GetCompletionEvent(tunnel_client, &handle);
        if (R_FAILED(rc)) {
            result.rc = rc;
            result.detail = "GetCompletionEvent CMIF failure";
            return false;
        }
        eventLoadRemote(out_event, handle, false);
        return true;
    };

    std::array<std::uint8_t, MaximumUdpPayloadStorageBytes + 1> payload_storage{};
    const std::span<std::uint8_t> expected_payload(payload_storage.data(), workload.payload_bytes);
    BuildPayload(expected_payload, 0x434C4F4EU, 0, workload);
    std::string completed;

    if (config.verify_cloned_session_lifetime) {
        client::ScopedClient clone;
        rc = client::CloneTunnelClient(primary, &clone);
        if (R_FAILED(rc)) {
            result.rc = rc;
            result.detail = "serviceClone wgnx:tun client failure";
            return result;
        }
        logger::Log(ctx, "contract_validation clone created");
        Capabilities clone_capabilities{};
        rc = client::GetCapabilities(clone, &clone_capabilities);
        if (R_FAILED(rc) || clone_capabilities.api_version != TunApiVersion) {
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "clone GetCapabilities CMIF failure" : "clone API version mismatch";
            return result;
        }

        FlowHandle cloned_flow{};
        if (!open_flow(primary, &cloned_flow)) {
            return result;
        }
        Event completion_event{};
        if (!load_event(clone, &completion_event)) {
            close_flow(clone, &cloned_flow);
            return result;
        }

        primary.Close();
        std::array<client::ScopedClient, MaximumClientContexts - 1> retained{};
        for (client::ScopedClient& extra : retained) {
            rc = client::OpenTunnelClient(root, &extra);
            if (R_FAILED(rc)) {
                eventClose(&completion_event);
                close_flow(clone, &cloned_flow);
                result.rc = rc;
                result.detail = "client-context capacity setup failure";
                return result;
            }
        }
        client::ScopedClient capacity_probe;
        const Result capacity_rc = client::OpenTunnelClient(root, &capacity_probe);
        logger::Log(ctx, "contract_validation clone capacity_probe_rc=%s", FormatResult(capacity_rc).c_str());
        if (R_SUCCEEDED(capacity_rc)) {
            capacity_probe.Close();
            eventClose(&completion_event);
            close_flow(clone, &cloned_flow);
            result.detail = "clone did not retain its logical client context";
            return result;
        }

        const PayloadRange descriptor{
            .flow = cloned_flow,
            .payload_offset = 0,
            .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
            .client_tag = 0x434C4F4EU,
        };
        PayloadResult disposition{};
        rc = client::SendUdpDatagram(clone, descriptor, expected_payload.data(), expected_payload.size(), &disposition);
        if (R_FAILED(rc) || disposition.status != ProtocolStatus::Success) {
            eventClose(&completion_event);
            close_flow(clone, &cloned_flow);
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "clone SendUdpDatagram CMIF failure"
                                         : "clone SendUdpDatagram status=" + std::string(StatusName(disposition.status));
            return result;
        }
        const CompletionWaitResult echo =
            WaitForEcho(clone, &completion_event, cloned_flow, expected_payload, workload.receive_deadline_ms);
        eventClose(&completion_event);
        if (!echo.received_expected) {
            if (!echo.service_lost) {
                close_flow(clone, &cloned_flow);
            }
            result.rc = echo.rc;
            result.detail = "clone echo failure: " + echo.detail;
            return result;
        }
        close_flow(clone, &cloned_flow);
        clone.Close();
        for (client::ScopedClient& extra : retained) {
            extra.Close();
        }
        rc = client::OpenTunnelClient(root, &primary);
        if (R_FAILED(rc)) {
            result.rc = rc;
            result.detail = "final clone close did not release client context";
            return result;
        }
        logger::Log(ctx, "contract_validation clone final_reference_released");
        completed = "clone_lifetime=ok";
    }

    if (config.verify_mixed_batch) {
        FlowHandle batch_flow{};
        if (!open_flow(primary, &batch_flow)) {
            return result;
        }
        const std::array<PayloadRange, 4> descriptors = {
            PayloadRange{
                .flow = batch_flow,
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
                .client_tag = 1
            },
            PayloadRange{
                .flow = batch_flow,
                .payload_offset = static_cast<std::uint32_t>(payload_storage.size()),
                .payload_size = 1,
                .client_tag = 2
            },
            PayloadRange{
                .flow = batch_flow,
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(payload_storage.size()),
                .client_tag = 3
            },
            PayloadRange{
                .flow = {},
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
                .client_tag = 4
            },
        };
        std::array<PayloadResult, descriptors.size()> dispositions{};
        rc = client::SendUdpDatagramBatch(
            primary,
            descriptors.data(),
            descriptors.size(),
            payload_storage.data(),
            payload_storage.size(),
            dispositions.data(),
            dispositions.size()
        );
        const std::array<ProtocolStatus, dispositions.size()> expected = {
            ProtocolStatus::Success,
            ProtocolStatus::MalformedInput,
            ProtocolStatus::PayloadTooLarge,
            ProtocolStatus::StaleHandle,
        };
        bool dispositions_match = R_SUCCEEDED(rc);
        for (std::size_t index = 0; index < dispositions.size(); ++index) {
            logger::Log(
                ctx,
                "contract_validation batch index=%zu tag=%llu status=%s",
                index,
                static_cast<unsigned long long>(dispositions[index].client_tag),
                StatusName(dispositions[index].status)
            );
            dispositions_match =
                dispositions_match && dispositions[index].client_tag == index + 1 && dispositions[index].status == expected[index];
        }
        if (!dispositions_match) {
            close_flow(primary, &batch_flow);
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "SendUdpDatagramBatch CMIF failure" : "mixed batch dispositions differed from contract";
            return result;
        }

        Event completion_event{};
        if (!load_event(primary, &completion_event)) {
            close_flow(primary, &batch_flow);
            return result;
        }
        const CompletionWaitResult echo =
            WaitForEcho(primary, &completion_event, batch_flow, expected_payload, workload.receive_deadline_ms);
        eventClose(&completion_event);
        if (!echo.service_lost) {
            close_flow(primary, &batch_flow);
        }
        if (!echo.received_expected) {
            result.rc = echo.rc;
            result.detail = "mixed batch echo failure: " + echo.detail;
            return result;
        }
        if (!completed.empty()) {
            completed += " ";
        }
        completed += "mixed_batch=ok";
    }

    result.success = true;
    result.bytes_sent = expected_payload.size() * static_cast<std::size_t>(config.verify_cloned_session_lifetime) +
                        expected_payload.size() * static_cast<std::size_t>(config.verify_mixed_batch);
    result.bytes_received = result.bytes_sent;
    result.detail = "api=" + std::to_string(capabilities.api_version) + " " + completed;
    return result;
}

ScenarioResult RunWgnxTunnelUdpWorkload(AppContext& ctx) {
    const RuntimeConfig defaults = CompiledRuntimeDefaults();
    return RunWgnxTunnelUdpWorkload(ctx, BuildTunnelUdpWorkload(defaults, *ActiveRuntimeProfile(defaults), defaults.next_workload_id));
}

} // namespace toolbox
