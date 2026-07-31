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
#include "wgnx/tunnel_client.hpp"

namespace requester {

namespace {

constexpr std::size_t PayloadHeaderBytes = 24;
constexpr std::uint32_t MaximumQueueFullRetriesPerDatagram = 16;

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
    case ProtocolStatus::TunnelBlockedByPolicy:
        return "tunnel_blocked_by_policy";
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
                // API v3 permits this exact wake-without-record sequence while the
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
                if (completion.type != wgnx::tunnel::CompletionType::InboundDatagram || completion.flow.value != flow.value ||
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

CompletionWaitResult
WaitForWritable(wgnx::tunnel::client::ScopedClient& client, Event* event, wgnx::tunnel::FlowHandle flow, std::uint32_t timeout_ms) {
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
                // API v3 permits this exact wake-without-record sequence while the
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

} // namespace

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
        result.detail = R_FAILED(rc) ? "GetTunApiVersion CMIF failure"
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
        const OpenConnectedUdpFlowRequest request{
            .remote =
                {.address = {destination[0], destination[1], destination[2], destination[3]},
                 .port = config.destination_port,
                 .reserved = 0},
            .diagnostic_tag = (static_cast<std::uint64_t>(config.workload_id) << 32U) | opened_flows,
        };
        const auto opened = [&] {
            OpenConnectedUdpFlowResult value{};
            rc = client::OpenConnectedUdpFlow(tunnel_client, request, &value);
            return value;
        }();
        if (R_FAILED(rc) || opened.status != ProtocolStatus::Success) {
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "OpenConnectedUdpFlow CMIF failure"
                                         : "OpenConnectedUdpFlow status=" + std::string(StatusName(opened.status));
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
    std::uint32_t accepted = 0;
    std::uint32_t echoed = 0;
    std::uint32_t ignored_completions = 0;
    std::uint32_t event_wakes = 0;
    std::uint32_t queue_full_events = 0;
    bool tunnel_service_lost = false;
    for (std::uint32_t sequence = 0; sequence < config.datagram_count; ++sequence) {
        const std::uint32_t flow_index = sequence % config.concurrent_flows;
        BuildPayload(payload, sequence, flow_index, config);
        const DatagramDescriptor descriptor{
            .flow = flows[flow_index],
            .payload_offset = 0,
            .payload_size = static_cast<std::uint32_t>(payload.size()),
            .client_tag = (static_cast<std::uint64_t>(config.workload_id) << 32U) | sequence,
        };
        std::uint32_t queue_full_retries_for_datagram = 0;
        for (;;) {
            DatagramDisposition disposition{};
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

    if (accepted == config.datagram_count && (!config.echo_replies || echoed == accepted)) {
        result.success = true;
        result.detail = "api=" + std::to_string(capabilities.api_version) + " policy=" + std::to_string(policy.policy_generation) +
                        " routes=" + std::to_string(policy.route_count) + " flows=" + std::to_string(config.concurrent_flows) +
                        " accepted=" + std::to_string(accepted) + " echoed=" + std::to_string(echoed) +
                        " queue_full_events=" + std::to_string(queue_full_events) +
                        " ignored_completions=" + std::to_string(ignored_completions) + " event_wakes=" + std::to_string(event_wakes);
    }
    return result;
}

ScenarioResult
RunWgnxTunnelContractValidation(AppContext& ctx, const TunnelUdpWorkloadConfig& workload, const TunnelContractValidationConfig& config) {
    using namespace wgnx::tunnel;

    ScenarioResult result{.name = "wgnx_tunnel_contract_validation"};
    if (!config.enabled) {
        result.skipped = true;
        result.detail = "disabled by runtime configuration";
        return result;
    }
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
        result.detail = R_FAILED(rc) ? "GetTunApiVersion CMIF failure" : "wgnx:tun API version mismatch";
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

    const OpenConnectedUdpFlowRequest request{
        .remote =
            {.address = {destination[0], destination[1], destination[2], destination[3]}, .port = workload.destination_port, .reserved = 0},
        .diagnostic_tag = (static_cast<std::uint64_t>(workload.workload_id) << 32U) | 0x434F4E54U,
    };
    const auto open_flow = [&](client::ScopedClient& tunnel_client, FlowHandle* out_flow) -> bool {
        OpenConnectedUdpFlowResult opened{};
        rc = client::OpenConnectedUdpFlow(tunnel_client, request, &opened);
        if (R_FAILED(rc) || opened.status != ProtocolStatus::Success) {
            result.rc = rc;
            result.detail = R_FAILED(rc) ? "OpenConnectedUdpFlow CMIF failure"
                                         : "OpenConnectedUdpFlow status=" + std::string(StatusName(opened.status));
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

        const DatagramDescriptor descriptor{
            .flow = cloned_flow,
            .payload_offset = 0,
            .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
            .client_tag = 0x434C4F4EU,
        };
        DatagramDisposition disposition{};
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
        const std::array<DatagramDescriptor, 4> descriptors = {
            DatagramDescriptor{
                .flow = batch_flow,
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
                .client_tag = 1
            },
            DatagramDescriptor{
                .flow = batch_flow,
                .payload_offset = static_cast<std::uint32_t>(payload_storage.size()),
                .payload_size = 1,
                .client_tag = 2
            },
            DatagramDescriptor{
                .flow = batch_flow,
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(payload_storage.size()),
                .client_tag = 3
            },
            DatagramDescriptor{
                .flow = {},
                .payload_offset = 0,
                .payload_size = static_cast<std::uint32_t>(expected_payload.size()),
                .client_tag = 4
            },
        };
        std::array<DatagramDisposition, descriptors.size()> dispositions{};
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
            ProtocolStatus::DatagramTooLarge,
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
    return RunWgnxTunnelUdpWorkload(ctx, CompiledRuntimeDefaults().tunnel_udp);
}

} // namespace requester
