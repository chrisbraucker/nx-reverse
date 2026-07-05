#include "scenarios.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.hpp"
#include "logger.hpp"

namespace requester {

namespace {

constexpr std::size_t Ipv4StringCapacity = 16;

struct ResolvedEndpoint {
    sockaddr_storage addr {};
    socklen_t addr_len = 0;
    std::string ip;
};

void SetSocketTimeouts(int sockfd) {
    const struct timeval timeout {
        .tv_sec = static_cast<long>(config::SocketTimeoutMs / 1000U),
        .tv_usec = static_cast<long>((config::SocketTimeoutMs % 1000U) * 1000U),
    };

    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool ResolveIpv4(
    AppContext& ctx,
    const char *host,
    std::uint16_t port,
    int socktype,
    ResolvedEndpoint& out_endpoint,
    std::string& detail) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;

    char port_buffer[16];
    std::snprintf(port_buffer, sizeof(port_buffer), "%u", port);

    struct addrinfo *results = nullptr;
    const int gai_rc = getaddrinfo(host, port_buffer, &hints, &results);
    if (gai_rc != 0) {
        detail = std::string("getaddrinfo failed: ") + gai_strerror(gai_rc);
        logger::Log(
            ctx,
            "resolve host=%s port=%u socktype=%d failed: %s",
            host,
            port,
            socktype,
            detail.c_str());
        return false;
    }

    for (const struct addrinfo *it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET || it->ai_addrlen > sizeof(out_endpoint.addr)) {
            continue;
        }

        std::memcpy(&out_endpoint.addr, it->ai_addr, it->ai_addrlen);
        out_endpoint.addr_len = static_cast<socklen_t>(it->ai_addrlen);

        char ip_buffer[Ipv4StringCapacity];
        const auto *sin = reinterpret_cast<const sockaddr_in *>(it->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
            out_endpoint.ip = "unknown";
        } else {
            out_endpoint.ip = ip_buffer;
        }

        detail = "resolved=" + out_endpoint.ip;
        freeaddrinfo(results);
        return true;
    }

    freeaddrinfo(results);
    detail = "no AF_INET result";
    logger::Log(ctx, "resolve host=%s port=%u produced no AF_INET result", host, port);
    return false;
}

ScenarioResult RunEnvironmentSnapshot(AppContext& ctx) {
    ScenarioResult result { .name = "environment_snapshot" };

    logger::Status(ctx, "Running NIFM status check");

    ctx.env.internet_status_rc = nifmGetInternetConnectionStatus(
        &ctx.env.connection_type,
        &ctx.env.wifi_strength,
        &ctx.env.connection_status);
    ctx.env.current_ip_rc = nifmGetCurrentIpAddress(&ctx.env.current_ip);
    ctx.env.ip_config_rc = nifmGetCurrentIpConfigInfo(
        &ctx.env.current_ip,
        &ctx.env.subnet_mask,
        &ctx.env.gateway,
        &ctx.env.primary_dns,
        &ctx.env.secondary_dns);

    result.rc = ctx.env.internet_status_rc;
    result.success = R_SUCCEEDED(ctx.env.current_ip_rc);
    result.detail =
        "status_rc=" + FormatResult(ctx.env.internet_status_rc) +
        " type=" + FormatInternetConnectionType(ctx.env.connection_type) +
        " wifi_strength=" + std::to_string(ctx.env.wifi_strength) +
        " connection_status=" + FormatInternetConnectionStatus(ctx.env.connection_status) +
        " ip_rc=" + FormatResult(ctx.env.current_ip_rc) +
        " current_ip=" + FormatIpv4(ctx.env.current_ip) +
        " ip_config_rc=" + FormatResult(ctx.env.ip_config_rc) +
        " subnet=" + FormatIpv4(ctx.env.subnet_mask) +
        " gateway=" + FormatIpv4(ctx.env.gateway) +
        " dns1=" + FormatIpv4(ctx.env.primary_dns) +
        " dns2=" + FormatIpv4(ctx.env.secondary_dns);

    logger::Log(ctx, "scenario=%s %s", result.name.c_str(), result.detail.c_str());
    return result;
}

ScenarioResult RunDnsResolve(AppContext& ctx) {
    ScenarioResult result { .name = "dns_resolve" };
    logger::Status(ctx, "Running DNS resolve for %s", config::DnsHostname);

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *results = nullptr;
    const int gai_rc = getaddrinfo(config::DnsHostname, nullptr, &hints, &results);
    if (gai_rc != 0) {
        result.err = gai_rc;
        result.detail = std::string("getaddrinfo failed: ") + gai_strerror(gai_rc);
        logger::Log(
            ctx,
            "scenario=%s host=%s failed: %s",
            result.name.c_str(),
            config::DnsHostname,
            result.detail.c_str());
        return result;
    }

    std::string addresses;
    for (const struct addrinfo *it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET) {
            continue;
        }

        char ip_buffer[Ipv4StringCapacity];
        const auto *sin = reinterpret_cast<const sockaddr_in *>(it->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ip_buffer, sizeof(ip_buffer)) != nullptr) {
            if (!addresses.empty()) {
                addresses += ",";
            }
            addresses += ip_buffer;
        }
    }

    freeaddrinfo(results);

    result.success = !addresses.empty();
    result.detail = "addresses=" + (addresses.empty() ? std::string("<none>") : addresses);
    logger::Log(ctx, "scenario=%s host=%s %s", result.name.c_str(), config::DnsHostname, result.detail.c_str());
    return result;
}

ScenarioResult RunPlainTcpConnect(AppContext& ctx) {
    ScenarioResult result { .name = "plain_tcp_connect" };
    logger::Status(ctx, "Connecting TCP to %s:%u", config::TcpHost, config::TcpPort);

    ResolvedEndpoint endpoint {};
    if (!ResolveIpv4(ctx, config::TcpHost, config::TcpPort, SOCK_STREAM, endpoint, result.detail)) {
        return result;
    }

    const int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
        return result;
    }

    SetSocketTimeouts(sockfd);

    if (connect(sockfd, reinterpret_cast<const struct sockaddr *>(&endpoint.addr), endpoint.addr_len) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    result.success = true;
    result.detail = "connected_ip=" + endpoint.ip;
    close(sockfd);
    return result;
}

ScenarioResult RunHttpGet(AppContext& ctx) {
    ScenarioResult result { .name = "http_get" };
    logger::Status(ctx, "Running HTTP GET for http://%s%s", config::HttpHost, config::HttpPath);

    ResolvedEndpoint endpoint {};
    if (!ResolveIpv4(ctx, config::HttpHost, config::HttpPort, SOCK_STREAM, endpoint, result.detail)) {
        return result;
    }

    const int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
        return result;
    }

    SetSocketTimeouts(sockfd);

    if (connect(sockfd, reinterpret_cast<const struct sockaddr *>(&endpoint.addr), endpoint.addr_len) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    char request[512];
    const int request_len = std::snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: nxrv-requester/1\r\nConnection: close\r\n\r\n",
        config::HttpPath,
        config::HttpHost);
    if (request_len <= 0 || static_cast<std::size_t>(request_len) >= sizeof(request)) {
        result.detail = "request formatting failed";
        close(sockfd);
        return result;
    }

    const ssize_t send_rc = send(sockfd, request, static_cast<std::size_t>(request_len), 0);
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "send failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }
    result.bytes_sent = static_cast<std::size_t>(send_rc);

    std::array<char, config::ReadBufferSize> buffer {};
    const ssize_t recv_rc = recv(sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail = "recv failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success = recv_rc > 0;
    result.detail =
        "connected_ip=" + endpoint.ip +
        " request_preview=" + EscapePreview(request, static_cast<std::size_t>(request_len), 96) +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);

    close(sockfd);
    return result;
}

ScenarioResult RunHttpsGet(AppContext& ctx) {
    ScenarioResult result { .name = "https_get" };

    if (!ctx.ssl_initialized) {
        result.skipped = true;
        result.rc = ctx.ssl_initialize_rc;
        result.detail = "ssl unavailable: " + FormatResult(ctx.ssl_initialize_rc);
        logger::Status(ctx, "Running HTTPS GET for https://%s%s", config::HttpsHost, config::HttpsPath);
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return result;
    }

    logger::Status(ctx, "Running HTTPS GET for https://%s%s", config::HttpsHost, config::HttpsPath);

    ResolvedEndpoint endpoint {};
    if (!ResolveIpv4(ctx, config::HttpsHost, config::HttpsPort, SOCK_STREAM, endpoint, result.detail)) {
        return result;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
        return result;
    }

    SetSocketTimeouts(sockfd);

    if (connect(sockfd, reinterpret_cast<const struct sockaddr *>(&endpoint.addr), endpoint.addr_len) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    SslContext ssl_context {};
    SslConnection ssl_connection {};
    Result rc = sslCreateContext(&ssl_context, SslVersion_Auto);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslCreateContext failed: " + FormatResult(rc);
        close(sockfd);
        return result;
    }

    bool connection_created = false;
    bool connection_has_socket = false;
    int owned_sockfd = sockfd;

    rc = sslContextCreateConnection(&ssl_context, &ssl_connection);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslContextCreateConnection failed: " + FormatResult(rc);
        sslContextClose(&ssl_context);
        close(sockfd);
        return result;
    }
    connection_created = true;

    const int transferred_sockfd = socketSslConnectionSetSocketDescriptor(&ssl_connection, sockfd);
    if (transferred_sockfd < 0 && errno != ENOENT) {
        result.err = errno;
        result.detail = "socket descriptor transfer failed: " + FormatErrno(errno);
        sslConnectionClose(&ssl_connection);
        sslContextClose(&ssl_context);
        close(sockfd);
        return result;
    }
    connection_has_socket = true;
    if (transferred_sockfd >= 0) {
        owned_sockfd = transferred_sockfd;
    } else {
        owned_sockfd = -1;
    }

    rc = sslConnectionSetHostName(&ssl_connection, config::HttpsHost, std::strlen(config::HttpsHost));
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionSetHostName failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    rc = sslConnectionSetVerifyOption(
        &ssl_connection,
        SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionSetVerifyOption failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    rc = sslConnectionSetIoMode(&ssl_connection, SslIoMode_Blocking);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionSetIoMode failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    rc = sslConnectionDoHandshake(&ssl_connection, nullptr, nullptr, nullptr, 0);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionDoHandshake failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    char request[512];
    const int request_len = std::snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: nxrv-requester/1\r\nConnection: close\r\n\r\n",
        config::HttpsPath,
        config::HttpsHost);
    if (request_len <= 0 || static_cast<std::size_t>(request_len) >= sizeof(request)) {
        result.detail = "request formatting failed";
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    std::uint32_t write_size = 0;
    rc = sslConnectionWrite(&ssl_connection, request, static_cast<std::uint32_t>(request_len), &write_size);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionWrite failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }
    result.bytes_sent = write_size;

    std::array<char, config::ReadBufferSize> buffer {};
    std::uint32_t read_size = 0;
    rc = sslConnectionRead(&ssl_connection, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &read_size);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionRead failed: " + FormatResult(rc);
        if (connection_created) {
            sslConnectionClose(&ssl_connection);
        }
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    SslCipherInfo cipher_info {};
    rc = sslConnectionGetCipherInfo(&ssl_connection, &cipher_info);
    const std::string cipher_summary = R_SUCCEEDED(rc)
        ? std::string(cipher_info.protocol_version) + "/" + cipher_info.cipher
        : std::string("cipher-info-failed:") + FormatResult(rc);

    result.bytes_received = read_size;
    result.success = read_size > 0;
    result.detail =
        "connected_ip=" + endpoint.ip +
        " cipher=" + cipher_summary +
        " request_preview=" + EscapePreview(request, static_cast<std::size_t>(request_len), 96) +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);

    if (connection_has_socket) {
        sslConnectionClose(&ssl_connection);
    }
    if (owned_sockfd >= 0) {
        close(owned_sockfd);
    }
    sslContextClose(&ssl_context);
    return result;
}

ScenarioResult RunUdpEcho(AppContext& ctx) {
    ScenarioResult result { .name = "udp_echo" };

    if (config::UdpHost[0] == '\0' || config::UdpPort == 0) {
        result.skipped = true;
        result.detail = "udp target not configured";
        logger::Status(ctx, "Running UDP echo skipped; no target configured");
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return result;
    }

    logger::Status(ctx, "Running UDP echo to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint {};
    if (!ResolveIpv4(ctx, config::UdpHost, config::UdpPort, SOCK_DGRAM, endpoint, result.detail)) {
        return result;
    }

    const int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
        return result;
    }

    SetSocketTimeouts(sockfd);

    const ssize_t send_rc = sendto(
        sockfd,
        config::UdpPayload,
        std::strlen(config::UdpPayload),
        0,
        reinterpret_cast<const struct sockaddr *>(&endpoint.addr),
        endpoint.addr_len);
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "sendto failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }
    result.bytes_sent = static_cast<std::size_t>(send_rc);

    std::array<char, config::ReadBufferSize> buffer {};
    const ssize_t recv_rc = recv(sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail = "recv failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success =
        static_cast<std::size_t>(recv_rc) == std::strlen(config::UdpPayload) &&
        std::memcmp(buffer.data(), config::UdpPayload, result.bytes_received) == 0;
    result.detail =
        "connected_ip=" + endpoint.ip +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 96);
    close(sockfd);
    return result;
}

void LogScenarioResult(AppContext& ctx, const ScenarioResult& result) {
    logger::Log(
        ctx,
        "scenario=%s success=%d skipped=%d rc=%s errno=%s bytes_sent=%zu bytes_received=%zu detail=%s",
        result.name.c_str(),
        result.success ? 1 : 0,
        result.skipped ? 1 : 0,
        FormatResult(result.rc).c_str(),
        FormatErrno(result.err).c_str(),
        result.bytes_sent,
        result.bytes_received,
        result.detail.c_str());
}

} // namespace

std::vector<ScenarioResult> RunScenarios(AppContext& ctx) {
    std::vector<ScenarioResult> results;
    results.reserve(6);

    results.push_back(RunEnvironmentSnapshot(ctx));
    LogScenarioResult(ctx, results.back());

    results.push_back(RunDnsResolve(ctx));
    LogScenarioResult(ctx, results.back());

    results.push_back(RunPlainTcpConnect(ctx));
    LogScenarioResult(ctx, results.back());

    results.push_back(RunHttpGet(ctx));
    LogScenarioResult(ctx, results.back());

    results.push_back(RunHttpsGet(ctx));
    LogScenarioResult(ctx, results.back());

    results.push_back(RunUdpEcho(ctx));
    LogScenarioResult(ctx, results.back());

    return results;
}

} // namespace requester
