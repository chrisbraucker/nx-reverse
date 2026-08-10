#include "bsd_system_udp_scenario.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <string>

#include <arpa/inet.h>
#include <switch.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bsd_system_udp_outcome.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "runtime.hpp"
#include "socket_config.hpp"
#include "udp_workload_metrics.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace toolbox {

namespace {

constexpr std::size_t PayloadHeaderBytes = 24;
constexpr std::uint32_t MaximumQueueFullRetriesPerDatagram = 16;

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

    BsdSocketScope(const BsdSocketScope&) = delete;
    BsdSocketScope& operator=(const BsdSocketScope&) = delete;

  private:
    bool initialized_{};
};

void StoreBigEndian32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24U);
    out[1] = static_cast<std::uint8_t>(value >> 16U);
    out[2] = static_cast<std::uint8_t>(value >> 8U);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t NextPayloadByte(std::uint32_t* state) {
    *state ^= *state << 13U;
    *state ^= *state >> 17U;
    *state ^= *state << 5U;
    return *state;
}

void BuildPayload(
    std::span<std::uint8_t> payload, std::uint32_t sequence, std::uint32_t flow_index, const TunnelUdpWorkloadConfig& config
) {
    std::memset(payload.data(), 0, payload.size());
    std::memcpy(payload.data(), "NXRVBS1", 7);
    StoreBigEndian32(payload.data() + 8, config.workload_id);
    StoreBigEndian32(payload.data() + 12, flow_index);
    StoreBigEndian32(payload.data() + 16, sequence);
    StoreBigEndian32(payload.data() + 20, config.payload_seed);

    std::uint32_t state = config.payload_seed ^ sequence ^ (flow_index * 0x9E3779B9U);
    for (std::size_t index = PayloadHeaderBytes; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(NextPayloadByte(&state));
    }
}

void CloseDescriptors(std::span<int> descriptors) {
    for (int& descriptor : descriptors) {
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
            descriptor = -1;
        }
    }
}

[[nodiscard]] bool ConfigureNonBlocking(const int descriptor, ScenarioResult* result, const std::uint32_t flow_index) {
    const int current_flags = fcntl(descriptor, F_GETFL, 0);
    if (current_flags < 0) {
        result->err = errno;
        result->detail = "fcntl(F_GETFL) failed flow=" + std::to_string(flow_index) + ": " + FormatErrno(errno);
        return false;
    }
    if (fcntl(descriptor, F_SETFL, current_flags | O_NONBLOCK) != 0) {
        result->err = errno;
        result->detail = "fcntl(F_SETFL, O_NONBLOCK) failed flow=" + std::to_string(flow_index) + ": " + FormatErrno(errno);
        return false;
    }
    return true;
}

[[nodiscard]] bool VerifyPostRouteRejection(const int descriptor, ScenarioResult* result) {
    constexpr int Enable = 1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &Enable, sizeof(Enable)) == 0) {
        result->detail = "setsockopt unexpectedly succeeded on a tunneled socket";
        return false;
    }
    if (errno != EOPNOTSUPP) {
        result->err = errno;
        result->detail = "setsockopt returned an unexpected error: " + FormatErrno(errno);
        return false;
    }
    return true;
}

[[nodiscard]] bool IsExpectedReply(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected) {
    return actual.size() == expected.size() && std::memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

[[nodiscard]] std::string FormatEndpoint(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN]{};
    const char* const text = inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    return std::string(text != nullptr ? text : "<invalid>") + ":" + std::to_string(ntohs(endpoint.sin_port));
}

[[nodiscard]] bool VerifyVisibleLocalEndpoint(
    AppContext& ctx, const int descriptor, ScenarioResult* result, const std::uint32_t flow_index
) {
    sockaddr_in local{};
    socklen_t local_length = sizeof(local);
    if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
        result->err = errno;
        result->detail = "getsockname failed flow=" + std::to_string(flow_index) + ": " + FormatErrno(errno);
        return false;
    }
    if (local_length < sizeof(sockaddr_in) || local.sin_family != AF_INET || local.sin_addr.s_addr == htonl(INADDR_ANY) ||
        local.sin_port == 0) {
        result->detail = "getsockname returned an invalid local endpoint flow=" + std::to_string(flow_index);
        return false;
    }
    logger::Log(ctx, "bsd_system_udp local flow=%u endpoint=%s", flow_index, FormatEndpoint(local).c_str());
    return true;
}

struct BsdSendResult {
    bool sent{};
    bool writable_after_queue_full{};
    std::uint32_t queue_full_retries{};
    int error{};
    std::string detail;
};

BsdSendResult SendWithWritableRetry(const int descriptor, const std::span<const std::uint8_t> payload, const std::uint32_t deadline_ms) {
    BsdSendResult result{};
    for (;;) {
        const ssize_t sent = send(descriptor, payload.data(), payload.size(), 0);
        if (sent >= 0) {
            if (static_cast<std::size_t>(sent) == payload.size()) {
                result.sent = true;
            } else {
                result.detail = "short UDP send";
            }
            return result;
        }

        result.error = errno;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || result.queue_full_retries == MaximumQueueFullRetriesPerDatagram) {
            result.detail = "send failed: " + FormatErrno(errno);
            return result;
        }

        ++result.queue_full_retries;
        pollfd writable_descriptor{
            .fd = descriptor,
            .events = POLLOUT,
            .revents = 0,
        };
        const int poll_result = poll(&writable_descriptor, 1, static_cast<int>(deadline_ms));
        if (poll_result < 0) {
            result.error = errno;
            result.detail = "writable poll failed: " + FormatErrno(errno);
            return result;
        }
        if (poll_result == 0) {
            result.error = ETIMEDOUT;
            result.detail = "writable poll timed out";
            return result;
        }
        if ((writable_descriptor.revents & POLLHUP) != 0) {
            result.error = ECONNABORTED;
            result.detail = "writable poll reported closed flow";
            return result;
        }
        if ((writable_descriptor.revents & POLLOUT) == 0) {
            result.detail = "writable poll missing POLLOUT revents=" + std::to_string(writable_descriptor.revents);
            return result;
        }
        result.writable_after_queue_full = true;
    }
}

struct BsdPollResult {
    BsdPollObservation observation{BsdPollObservation::Unexpected};
    int error{};
    short revents{};
};

[[nodiscard]] BsdPollResult WaitForInput(const int descriptor, const std::uint32_t deadline_ms) {
    pollfd poll_descriptor{
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    const int poll_result = poll(&poll_descriptor, 1, static_cast<int>(deadline_ms));
    return {
        .observation = ClassifyBsdPollObservation(poll_result, poll_descriptor.revents),
        .error = poll_result < 0 ? errno : 0,
        .revents = poll_descriptor.revents,
    };
}

[[nodiscard]] bool ReceiveExpectedEcho(
    const int descriptor,
    const std::span<const std::uint8_t> payload,
    const std::span<std::uint8_t> received,
    const sockaddr_in& remote,
    const std::uint32_t deadline_ms,
    ScenarioResult* const result,
    const std::uint32_t sequence
) {
    const BsdPollResult poll_result = WaitForInput(descriptor, deadline_ms);
    if (!IsExpectedBsdPollObservation(BsdSystemUdpExpectedOutcome::EchoReply, poll_result.observation)) {
        result->err = poll_result.error;
        result->detail =
            "poll did not report echo sequence=" + std::to_string(sequence) + " revents=" + std::to_string(poll_result.revents);
        if (poll_result.observation == BsdPollObservation::Timeout) {
            result->err = ETIMEDOUT;
            result->detail = "poll timed out sequence=" + std::to_string(sequence);
        } else if (poll_result.observation == BsdPollObservation::Error) {
            result->detail = "poll failed sequence=" + std::to_string(sequence) + ": " + FormatErrno(poll_result.error);
        }
        return false;
    }

    sockaddr_in source{};
    socklen_t source_length = sizeof(source);
    const ssize_t received_count =
        recvfrom(descriptor, received.data(), received.size(), 0, reinterpret_cast<sockaddr*>(&source), &source_length);
    if (received_count < 0) {
        result->err = errno;
        result->detail = "recvfrom failed sequence=" + std::to_string(sequence) + ": " + FormatErrno(errno);
        return false;
    }
    result->bytes_received += static_cast<std::size_t>(received_count);
    const std::span<const std::uint8_t> reply(received.data(), static_cast<std::size_t>(received_count));
    if (!IsExpectedReply(reply, payload)) {
        result->detail = "unexpected echo sequence=" + std::to_string(sequence) + " source=" + FormatEndpoint(source);
        return false;
    }
    if (source_length < sizeof(sockaddr_in) || source.sin_family != AF_INET || source.sin_addr.s_addr != remote.sin_addr.s_addr ||
        source.sin_port != remote.sin_port) {
        result->detail = "unexpected reply endpoint sequence=" + std::to_string(sequence) + " source=" + FormatEndpoint(source);
        return false;
    }
    return true;
}

[[nodiscard]] bool VerifyTerminalSend(const int descriptor, const std::span<const std::uint8_t> payload, ScenarioResult* const result) {
    const ssize_t sent = send(descriptor, payload.data(), payload.size(), 0);
    if (sent < 0 && errno == ECONNABORTED) {
        return true;
    }
    result->err = sent < 0 ? errno : 0;
    result->detail = "post-closure send did not return ECONNABORTED";
    if (result->err != 0) {
        result->detail += ": " + FormatErrno(result->err);
    }
    return false;
}

} // namespace

ScenarioResult RunBsdSystemUdpWorkload(
    AppContext& ctx, const TunnelUdpWorkloadConfig& config, const BsdSystemUdpWorkloadConfig& bsd_config
) {
    ScenarioResult result{.name = "bsd_system_udp_workload"};
    logger::Status(ctx, "Running normal bsd:s UDP workload to %s:%u", config.destination_ipv4.c_str(), config.destination_port);

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(config.destination_port);
    if (inet_pton(AF_INET, config.destination_ipv4.c_str(), &remote.sin_addr) != 1) {
        result.detail = "invalid configured BSD workload destination";
        return result;
    }
    if (config.concurrent_flows == 0 || config.concurrent_flows > wgnx::tunnel::MaximumFlowsPerClient) {
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
    for (std::uint32_t flow_index = 0; flow_index < config.concurrent_flows; ++flow_index) {
        const int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (descriptor < 0) {
            result.err = errno;
            result.detail = "socket failed flow=" + std::to_string(flow_index) + ": " + FormatErrno(errno);
            CloseDescriptors(descriptors);
            return result;
        }
        descriptors[flow_index] = descriptor;
        if (connect(descriptor, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0) {
            result.err = errno;
            result.detail = "connect failed flow=" + std::to_string(flow_index) + ": " + FormatErrno(errno);
            CloseDescriptors(descriptors);
            return result;
        }
        if (!VerifyVisibleLocalEndpoint(ctx, descriptor, &result, flow_index)) {
            CloseDescriptors(descriptors);
            return result;
        }
        if (!ConfigureNonBlocking(descriptor, &result, flow_index)) {
            CloseDescriptors(descriptors);
            return result;
        }
    }
    if (bsd_config.verify_post_route_rejection && !VerifyPostRouteRejection(descriptors[0], &result)) {
        CloseDescriptors(descriptors);
        return result;
    }
    if (bsd_config.verify_post_route_rejection) {
        logger::Log(ctx, "bsd_system_udp verified post-route setsockopt rejection");
    }
    logger::Log(ctx, "bsd_system_udp opened flows=%u destination=%s", config.concurrent_flows, FormatEndpoint(remote).c_str());

    std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadStorageBytes> payload_storage{};
    const std::span<std::uint8_t> payload(payload_storage.data(), config.payload_bytes);
    std::array<std::uint8_t, wgnx::tunnel::MaximumUdpPayloadStorageBytes> received{};
    const std::uint64_t tick_frequency = armGetSystemTickFreq();
    UdpWorkloadMetrics workload_metrics;
    std::uint32_t echoed = 0;
    std::uint32_t queue_full_retries = 0;
    bool writable_recovery = false;
    const auto log_metrics = [&] {
        logger::Log(
            ctx,
            "[udp-workload-summary] kind=bsd-system workload=%u scope=workload flow=all "
            "attempted=%u accepted=%u submitted_bytes=%llu submission_elapsed_ns=%llu echoed=%u received_bytes=%llu "
            "rtt_samples=%u rtt_min_ns=%llu rtt_mean_ns=%llu rtt_p50_upper_ns=%llu "
            "rtt_p95_upper_ns=%llu rtt_p99_upper_ns=%llu rtt_max_ns=%llu queue_full_retries=%u",
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
            queue_full_retries
        );
    };

    if (bsd_config.expected_outcome == BsdSystemUdpExpectedOutcome::NoReplyTimeout) {
        BuildPayload(payload, 0, 0, config);
        workload_metrics.RecordAttempt();
        const BsdSendResult send_result = SendWithWritableRetry(descriptors[0], payload, config.receive_deadline_ms);
        queue_full_retries += send_result.queue_full_retries;
        if (!send_result.sent) {
            result.err = send_result.error;
            result.detail = send_result.detail;
        } else {
            result.bytes_sent += payload.size();
            const std::uint64_t accepted_tick = armGetSystemTick();
            workload_metrics.RecordSubmission(accepted_tick, payload.size());
            const BsdPollResult poll_result = WaitForInput(descriptors[0], config.receive_deadline_ms);
            if (IsExpectedBsdPollObservation(bsd_config.expected_outcome, poll_result.observation)) {
                result.success = true;
                result.detail = "bsd:s no-reply timeout confirmed workload=" + std::to_string(config.workload_id) +
                                " deadline_ms=" + std::to_string(config.receive_deadline_ms);
            } else {
                result.err = poll_result.error;
                result.detail = "no-reply poll unexpected revents=" + std::to_string(poll_result.revents);
            }
        }
        log_metrics();
        CloseDescriptors(descriptors);
        return result;
    }

    if (bsd_config.expected_outcome == BsdSystemUdpExpectedOutcome::TerminalClosure) {
        BuildPayload(payload, 0, 0, config);
        const std::uint64_t send_start_tick = armGetSystemTick();
        workload_metrics.RecordAttempt();
        const BsdSendResult send_result = SendWithWritableRetry(descriptors[0], payload, config.receive_deadline_ms);
        queue_full_retries += send_result.queue_full_retries;
        if (!send_result.sent) {
            result.err = send_result.error;
            result.detail = send_result.detail;
        } else {
            result.bytes_sent += payload.size();
            const std::uint64_t accepted_tick = armGetSystemTick();
            workload_metrics.RecordSubmission(accepted_tick, payload.size());
            if (ReceiveExpectedEcho(descriptors[0], payload, received, remote, config.receive_deadline_ms, &result, 0)) {
                ++echoed;
                const std::uint64_t received_tick = armGetSystemTick();
                workload_metrics.RecordEcho(send_start_tick, received_tick, tick_frequency, payload.size());
                logger::Status(ctx, "bsd_system_udp terminal flow waiting deadline_ms=%u", config.receive_deadline_ms);
                const BsdPollResult poll_result = WaitForInput(descriptors[0], config.receive_deadline_ms);
                if (!IsExpectedBsdPollObservation(bsd_config.expected_outcome, poll_result.observation)) {
                    result.err = poll_result.error;
                    result.detail = "terminal poll unexpected revents=" + std::to_string(poll_result.revents);
                } else if (VerifyTerminalSend(descriptors[0], payload, &result)) {
                    if (close(descriptors[0]) != 0) {
                        result.err = errno;
                        result.detail = "terminal close failed: " + FormatErrno(errno);
                    } else {
                        descriptors[0] = -1;
                        result.success = true;
                        result.detail = "bsd:s terminal closure confirmed workload=" + std::to_string(config.workload_id) +
                                        " echoed=" + std::to_string(echoed) + " post_send=ECONNABORTED";
                        result.detail += " avg_echo_latency_ms=" + std::to_string(workload_metrics.RttMeanMilliseconds());
                    }
                }
            }
        }
        log_metrics();
        CloseDescriptors(descriptors);
        return result;
    }

    for (std::uint32_t sequence = 0; sequence < config.datagram_count; ++sequence) {
        const std::uint32_t flow_index = sequence % config.concurrent_flows;
        const int descriptor = descriptors[flow_index];
        BuildPayload(payload, sequence, flow_index, config);
        const std::uint64_t send_start_tick = armGetSystemTick();
        workload_metrics.RecordAttempt();
        const BsdSendResult send_result = SendWithWritableRetry(descriptor, payload, config.receive_deadline_ms);
        queue_full_retries += send_result.queue_full_retries;
        writable_recovery = writable_recovery || send_result.writable_after_queue_full;
        if (!send_result.sent) {
            result.err = send_result.error;
            result.detail =
                send_result.detail + " sequence=" + std::to_string(sequence) + " queue_full_retries=" + std::to_string(queue_full_retries);
            break;
        }
        result.bytes_sent += payload.size();
        const std::uint64_t accepted_tick = armGetSystemTick();
        workload_metrics.RecordSubmission(accepted_tick, payload.size());

        if (config.echo_replies) {
            if (!ReceiveExpectedEcho(descriptor, payload, received, remote, config.receive_deadline_ms, &result, sequence)) {
                break;
            }
            ++echoed;
            const std::uint64_t received_tick = armGetSystemTick();
            workload_metrics.RecordEcho(send_start_tick, received_tick, tick_frequency, payload.size());
            if (sequence == 0 || sequence + 1 == config.datagram_count) {
                TOOLBOX_LOG_PACKET(ctx, "bsd_system_udp echo flow=%u sequence=%u reply_received", flow_index, sequence);
            }
        }

        if (config.pacing_ms != 0) {
            SleepMilliseconds(config.pacing_ms);
        }
    }

    CloseDescriptors(descriptors);
    log_metrics();
    if (result.detail.empty() && bsd_config.require_writable_recovery && !HasWritableRecovery(queue_full_retries, writable_recovery)) {
        result.detail = "writable recovery was not observed";
    }
    if (result.detail.empty() && (!config.echo_replies || echoed == config.datagram_count)) {
        result.success = true;
        result.detail = "bsd_service=system destination=" + config.destination_ipv4 + ":" + std::to_string(config.destination_port) +
                        " workload=" + std::to_string(config.workload_id) + " flows=" + std::to_string(config.concurrent_flows) +
                        " sent=" + std::to_string(config.datagram_count) + " echoed=" + std::to_string(echoed) +
                        " queue_full_retries=" + std::to_string(queue_full_retries) +
                        " writable_recovery=" + (writable_recovery ? "true" : "false");
        if (config.echo_replies) {
            result.detail += " avg_echo_latency_ms=" + std::to_string(workload_metrics.RttMeanMilliseconds());
        }
    }
    return result;
}

} // namespace toolbox
