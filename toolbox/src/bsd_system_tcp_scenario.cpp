#include "bsd_system_tcp_scenario.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <poll.h>
#include <switch.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.hpp"
#include "logger.hpp"
#include "runtime.hpp"

namespace toolbox {

namespace {

constexpr char ExpectedReply[] = "NXRV TCP ACK\r\n";

std::uint64_t ElapsedMilliseconds(const std::uint64_t started, const std::uint64_t completed) {
    return (completed - started) * 1000U / armGetSystemTickFreq();
}

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

std::string FormatEndpoint(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN]{};
    const char* const text = inet_ntop(AF_INET, &endpoint.sin_addr, address, sizeof(address));
    return std::string(text != nullptr ? text : "<invalid>") + ":" + std::to_string(ntohs(endpoint.sin_port));
}

bool SetTimeouts(const int descriptor, const std::uint32_t deadline_ms, ScenarioResult* result) {
    const timeval timeout{
        .tv_sec = static_cast<long>(deadline_ms / 1000U),
        .tv_usec = static_cast<long>((deadline_ms % 1000U) * 1000U),
    };
    if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        result->err = errno;
        result->detail = "setsockopt timeout failed: " + FormatErrno(errno);
        return false;
    }
    return true;
}

bool SendAll(const int descriptor, const char* data, const std::size_t size, ScenarioResult* result) {
    std::size_t offset{};
    while (offset < size) {
        const ssize_t sent = send(descriptor, data + offset, size - offset, 0);
        if (sent <= 0) {
            result->err = sent == 0 ? EPIPE : errno;
            result->detail = "send failed: " + FormatErrno(result->err);
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    result->bytes_sent = offset;
    return true;
}

bool ReceiveExpectedReply(const int descriptor, const std::uint32_t deadline_ms, ScenarioResult* result) {
    pollfd poll_descriptor{
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    const int poll_result = poll(&poll_descriptor, 1, static_cast<int>(deadline_ms));
    if (poll_result <= 0) {
        result->err = poll_result == 0 ? ETIMEDOUT : errno;
        result->detail = poll_result == 0 ? "receive poll timed out" : "receive poll failed: " + FormatErrno(errno);
        return false;
    }
    if ((poll_descriptor.revents & (POLLERR | POLLNVAL)) != 0 || (poll_descriptor.revents & POLLIN) == 0) {
        result->detail = "receive poll unexpected revents=" + std::to_string(poll_descriptor.revents);
        return false;
    }

    std::array<char, sizeof(ExpectedReply) - 1> reply{};
    std::size_t offset{};
    while (offset < reply.size()) {
        const ssize_t received = recv(descriptor, reply.data() + offset, reply.size() - offset, 0);
        if (received <= 0) {
            result->err = received == 0 ? ECONNRESET : errno;
            result->detail = "recv failed: " + FormatErrno(result->err);
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    result->bytes_received = offset;
    if (std::memcmp(reply.data(), ExpectedReply, reply.size()) != 0) {
        result->detail = "unexpected reply: " + EscapePreview(reply.data(), reply.size(), reply.size());
        return false;
    }
    return true;
}

} // namespace

ScenarioResult RunBsdSystemTcpExchange(
    AppContext& ctx, const RuntimeProfile& profile, const std::uint32_t workload_id, const TcpScenarioConfig& config
) {
    ScenarioResult result{.name = "bsd_system_tcp_exchange"};
    BsdSocketScope socket_scope;
    const Result socket_rc = socket_scope.Initialize();
    if (R_FAILED(socket_rc)) {
        result.rc = socket_rc;
        result.detail = "socketInitialize failed: " + FormatResult(socket_rc);
        return result;
    }

    sockaddr_in remote{
        .sin_family = AF_INET,
        .sin_port = htons(profile.tcp_destination_port),
    };
    if (inet_pton(AF_INET, profile.bsd_destination_ipv4.c_str(), &remote.sin_addr) != 1) {
        result.detail = "invalid configured TCP destination";
        return result;
    }
    logger::Status(ctx, "Running bsd:s TCP exchange to %s", FormatEndpoint(remote).c_str());

    const int descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (descriptor < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
        return result;
    }
    if (!SetTimeouts(descriptor, config.receive_deadline_ms, &result)) {
        static_cast<void>(close(descriptor));
        return result;
    }
    const std::uint64_t connect_started = armGetSystemTick();
    if (connect(descriptor, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        static_cast<void>(close(descriptor));
        return result;
    }
    const std::uint64_t connect_completed = armGetSystemTick();

    sockaddr_in local{};
    socklen_t local_length = sizeof(local);
    if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &local_length) != 0) {
        result.err = errno;
        result.detail = "getsockname failed: " + FormatErrno(errno);
        static_cast<void>(close(descriptor));
        return result;
    }
    if (local_length < sizeof(local) || local.sin_family != AF_INET || local.sin_addr.s_addr == htonl(INADDR_ANY) || local.sin_port == 0) {
        result.detail = "getsockname returned an invalid local endpoint";
        static_cast<void>(close(descriptor));
        return result;
    }

    std::array<char, 32> request{};
    const int request_length = std::snprintf(request.data(), request.size(), "NXRV TCP %u\r\n", workload_id);
    if (request_length < 0 || static_cast<std::size_t>(request_length) >= request.size()) {
        result.detail = "failed to format request";
        static_cast<void>(close(descriptor));
        return result;
    }
    const std::uint64_t send_started = armGetSystemTick();
    if (!SendAll(descriptor, request.data(), static_cast<std::size_t>(request_length), &result)) {
        static_cast<void>(close(descriptor));
        return result;
    }
    const std::uint64_t send_completed = armGetSystemTick();
    const std::uint64_t receive_started = armGetSystemTick();
    if (!ReceiveExpectedReply(descriptor, config.receive_deadline_ms, &result)) {
        static_cast<void>(close(descriptor));
        return result;
    }
    const std::uint64_t receive_completed = armGetSystemTick();
    if (close(descriptor) != 0) {
        result.err = errno;
        result.detail = "close failed: " + FormatErrno(errno);
        return result;
    }
    result.success = true;
    result.detail = "remote=" + FormatEndpoint(remote) + " local=" + FormatEndpoint(local) + " workload=" + std::to_string(workload_id) +
                    " connect_ms=" + std::to_string(ElapsedMilliseconds(connect_started, connect_completed)) +
                    " send_ms=" + std::to_string(ElapsedMilliseconds(send_started, send_completed)) +
                    " receive_ms=" + std::to_string(ElapsedMilliseconds(receive_started, receive_completed)) + " reply=validated close=ok";
    return result;
}

} // namespace toolbox
