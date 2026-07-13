#include "scenarios.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.hpp"
#include "logger.hpp"
#include "manual_bsd_lifecycle.hpp"

namespace requester {

namespace {

constexpr std::size_t Ipv4StringCapacity = 16;
constexpr std::size_t SslVerifyErrorCapacity = 8;

struct ResolvedEndpoint {
    sockaddr_storage addr {};
    socklen_t addr_len = 0;
    std::string ip;
};

struct ConnectedTcpSocket {
    int sockfd = -1;
    std::string ip;
};

struct CurlResponseBuffer {
    std::array<char, config::CurlReadBufferSize> data {};
    std::size_t size = 0;
};

std::string FormatSocketAddress(
    const sockaddr_storage& address,
    socklen_t address_len) {
    std::string detail =
        "source_addr_len=" + std::to_string(address_len) +
        " source_family=" + std::to_string(address.ss_family);

    if (address.ss_family != AF_INET || address_len < sizeof(sockaddr_in)) {
        return detail + " source_ip=<unavailable> source_port=<unavailable>";
    }

    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address);
    char ip_buffer[Ipv4StringCapacity] {};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
        return detail +
            " source_ip=<inet_ntop-failed> source_port=" +
            std::to_string(ntohs(ipv4->sin_port));
    }

    return detail +
        " source_ip=" + ip_buffer +
        " source_port=" + std::to_string(ntohs(ipv4->sin_port));
}

using ScenarioFn = ScenarioResult (*)(AppContext& ctx);

struct ScenarioStep {
    const char *name;
    bool enabled;
    ScenarioFn fn;
};

bool ResolveIpv4(
    AppContext& ctx,
    const char *host,
    std::uint16_t port,
    int socktype,
    ResolvedEndpoint& out_endpoint,
    std::string& detail);

size_t CurlWriteCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buffer = static_cast<CurlResponseBuffer *>(userdata);
    if (buffer == nullptr || ptr == nullptr) {
        return 0;
    }

    const std::size_t total = size * nmemb;
    const std::size_t remaining = buffer->data.size() - buffer->size;
    const std::size_t to_copy = std::min(total, remaining);
    if (to_copy > 0) {
        std::memcpy(buffer->data.data() + buffer->size, ptr, to_copy);
        buffer->size += to_copy;
    }
    return total;
}

void ConfigureCurlCommon(CURL *curl, AppContext& ctx) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nxrv-requester/1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L * 128L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1024L * 128L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_TRY));
    curl_easy_setopt(curl, CURLOPT_TRANSFER_ENCODING, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config::SocketTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config::SocketTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (ctx.curl_share != nullptr) {
        curl_easy_setopt(curl, CURLOPT_SHARE, ctx.curl_share);
    }
}

bool RunCurlTransfer(
    AppContext& ctx,
    const char *scenario_name,
    const char *url,
    bool allow_insecure_tls,
    ScenarioResult& result) {
    if (!ctx.curl_initialized) {
        result.skipped = true;
        result.detail = std::string("curl unavailable: ") + curl_easy_strerror(ctx.curl_global_rc);
        logger::Log(ctx, "scenario=%s skipped: %s", scenario_name, result.detail.c_str());
        return true;
    }

    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.detail = "curl_easy_init failed";
        return false;
    }

    CurlResponseBuffer response {};
    ConfigureCurlCommon(curl, ctx);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (allow_insecure_tls) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode perform_rc = curl_easy_perform(curl);
    if (perform_rc != CURLE_OK) {
        result.detail = std::string("curl_easy_perform failed: ") + curl_easy_strerror(perform_rc);
        curl_easy_cleanup(curl);
        return false;
    }

    char *effective_url = nullptr;
    long response_code = 0;
    char *primary_ip = nullptr;
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url));
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code));
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip));

    result.success = true;
    result.bytes_received = response.size;
    result.detail =
        std::string("effective_url=") + (effective_url != nullptr ? effective_url : url) +
        " primary_ip=" + (primary_ip != nullptr ? primary_ip : "unknown") +
        " http_code=" + std::to_string(response_code) +
        " response_preview=" + EscapePreview(response.data.data(), response.size, 160);

    curl_easy_cleanup(curl);
    return true;
}

std::string CollectSslVerifyDiagnostics(SslConnection& ssl_connection) {
    std::array<Result, SslVerifyErrorCapacity> verify_errors {};
    std::uint32_t reported_error_count = 0;
    std::uint32_t copied_error_count = 0;

    const Result verify_errors_rc = sslConnectionGetVerifyCertErrors(
        &ssl_connection,
        &reported_error_count,
        &copied_error_count,
        verify_errors.data(),
        static_cast<std::uint32_t>(verify_errors.size()));
    const Result verify_error_rc = sslConnectionGetVerifyCertError(&ssl_connection);

    std::string detail =
        " verify_cert_error=" + FormatResult(verify_error_rc) +
        " verify_errors_rc=" + FormatResult(verify_errors_rc) +
        " verify_errors_reported=" + std::to_string(reported_error_count) +
        " verify_errors_copied=" + std::to_string(copied_error_count);

    if (R_SUCCEEDED(verify_errors_rc) && reported_error_count > 0) {
        const std::size_t count = std::min<std::size_t>(reported_error_count, verify_errors.size());
        detail += " verify_errors=[";
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                detail += ",";
            }
            detail += FormatResult(verify_errors[i]);
        }
        if (reported_error_count > verify_errors.size()) {
            detail += ",...";
        }
        detail += "]";
    }

    return detail;
}

const char *SocketOptionName(int level, int optname) {
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                return "SOL_SOCKET/SO_REUSEADDR";
            case SO_RCVTIMEO:
                return "SOL_SOCKET/SO_RCVTIMEO";
            case SO_SNDTIMEO:
                return "SOL_SOCKET/SO_SNDTIMEO";
            default:
                return "SOL_SOCKET/unknown";
        }
    }
    return "unknown";
}

int TraceSetSockOpt(
    AppContext& ctx,
    int sockfd,
    int level,
    int optname,
    const void *optval,
    socklen_t optlen,
    int& out_errno) {
    logger::Log(
        ctx,
        "setsockopt begin fd=%d level=%d opt=%d opt_name=%s optval=%p optlen=%u",
        sockfd,
        level,
        optname,
        SocketOptionName(level, optname),
        optval,
        static_cast<unsigned>(optlen));

    errno = 0;
    const int rc = setsockopt(sockfd, level, optname, optval, optlen);
    const int saved_errno = errno;
    out_errno = saved_errno;

    logger::Log(
        ctx,
        "setsockopt complete fd=%d level=%d opt=%d opt_name=%s rc=%d errno=%d detail=%s",
        sockfd,
        level,
        optname,
        SocketOptionName(level, optname),
        rc,
        saved_errno,
        rc == 0 ? "success" : FormatErrno(saved_errno).c_str());
    return rc;
}

bool TraceSetSockOptReuseAddr(AppContext& ctx, int sockfd, ScenarioResult& result) {
    const int enabled = 1;
    int saved_errno = 0;
    const int rc = TraceSetSockOpt(ctx, sockfd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled), saved_errno);
    if (rc != 0) {
        result.err = saved_errno;
        result.detail = std::string("SO_REUSEADDR failed: ") + FormatErrno(saved_errno);
        return false;
    }
    return true;
}

bool TraceSetSockOptRecvTimeout(AppContext& ctx, int sockfd, ScenarioResult& result) {
    const struct timeval timeout {
        .tv_sec = static_cast<long>(config::SocketTimeoutMs / 1000U),
        .tv_usec = static_cast<long>((config::SocketTimeoutMs % 1000U) * 1000U),
    };

    int saved_errno = 0;
    const int rc = TraceSetSockOpt(ctx, sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout), saved_errno);
    if (rc != 0) {
        result.err = saved_errno;
        result.detail = std::string("SO_RCVTIMEO failed: ") + FormatErrno(saved_errno);
        return false;
    }
    return true;
}

bool TraceSetSockOptSendTimeout(AppContext& ctx, int sockfd, ScenarioResult& result) {
    const struct timeval timeout {
        .tv_sec = static_cast<long>(config::SocketTimeoutMs / 1000U),
        .tv_usec = static_cast<long>((config::SocketTimeoutMs % 1000U) * 1000U),
    };

    int saved_errno = 0;
    const int rc = TraceSetSockOpt(ctx, sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout), saved_errno);
    if (rc != 0) {
        result.err = saved_errno;
        result.detail = std::string("SO_SNDTIMEO failed: ") + FormatErrno(saved_errno);
        return false;
    }
    return true;
}

bool SetSocketTimeouts(AppContext& ctx, int sockfd, ScenarioResult& result) {
    return TraceSetSockOptRecvTimeout(ctx, sockfd, result) &&
        TraceSetSockOptSendTimeout(ctx, sockfd, result);
}

bool ConnectResolvedTcp(
    AppContext& ctx,
    const char *host,
    std::uint16_t port,
    ConnectedTcpSocket& out_socket,
    std::string& detail) {
    ResolvedEndpoint endpoint {};
    if (!ResolveIpv4(ctx, host, port, SOCK_STREAM, endpoint, detail)) {
        return false;
    }

    const int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        detail = "socket failed: " + FormatErrno(errno);
        return false;
    }

    ScenarioResult setopt_result { .name = "plain_tcp_connect_setsockopt" };
    if (!SetSocketTimeouts(ctx, sockfd, setopt_result)) {
        detail = setopt_result.detail;
        close(sockfd);
        return false;
    }

    if (connect(sockfd, reinterpret_cast<const struct sockaddr *>(&endpoint.addr), endpoint.addr_len) != 0) {
        detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return false;
    }

    out_socket.sockfd = sockfd;
    out_socket.ip = endpoint.ip;
    detail = "connected_ip=" + endpoint.ip;
    return true;
}

bool SendTcpPayload(
    int sockfd,
    const void *payload,
    std::size_t payload_size,
    std::size_t& bytes_sent,
    std::string& detail) {
    const ssize_t send_rc = send(sockfd, payload, payload_size, 0);
    if (send_rc < 0) {
        detail = "send failed: " + FormatErrno(errno);
        return false;
    }

    bytes_sent = static_cast<std::size_t>(send_rc);
    return true;
}

bool ReceiveTcpPayload(
    int sockfd,
    std::array<char, config::ReadBufferSize>& buffer,
    std::size_t& bytes_received,
    std::string& detail) {
    const ssize_t recv_rc = recv(sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        detail = "recv failed: " + FormatErrno(errno);
        return false;
    }

    bytes_received = static_cast<std::size_t>(recv_rc);
    return true;
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

    if (!ctx.nifm_initialized) {
        result.skipped = true;
        result.rc = ctx.nifm_initialize_rc;
        result.detail = "nifm unavailable: " + FormatResult(ctx.nifm_initialize_rc);
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return result;
    }

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

    ConnectedTcpSocket connection {};
    if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, result.detail)) {
        return result;
    }

    std::array<char, config::ReadBufferSize> buffer {};
    if (!SendTcpPayload(
            connection.sockfd,
            config::TcpPayload,
            std::strlen(config::TcpPayload),
            result.bytes_sent,
            result.detail)) {
        result.err = errno;
        close(connection.sockfd);
        return result;
    }

    if (!ReceiveTcpPayload(connection.sockfd, buffer, result.bytes_received, result.detail)) {
        result.err = errno;
        close(connection.sockfd);
        return result;
    }

    result.success = result.bytes_received > 0;
    result.detail =
        "connected_ip=" + connection.ip +
        " request_preview=" + EscapePreview(config::TcpPayload, std::strlen(config::TcpPayload), 96) +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);
    close(connection.sockfd);
    return result;
}

ScenarioResult RunIdleTcpHold(AppContext& ctx) {
    ScenarioResult result { .name = "tcp_idle_hold" };
    logger::Status(
        ctx,
        "Holding idle TCP socket to %s:%u for %u ms",
        config::TcpHost,
        config::TcpPort,
        config::IdleSocketHoldMs);

    ConnectedTcpSocket connection {};
    if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, result.detail)) {
        return result;
    }

    if (config::IdleSocketHoldMs > 0) {
        SleepMilliseconds(config::IdleSocketHoldMs);
    }

    result.success = true;
    result.detail =
        "connected_ip=" + connection.ip +
        " hold_ms=" + std::to_string(config::IdleSocketHoldMs);
    close(connection.sockfd);
    return result;
}

ScenarioResult RunHttpGet(AppContext& ctx) {
    ScenarioResult result { .name = "http_get" };
    logger::Status(ctx, "Running HTTP GET for http://%s%s", config::HttpHost, config::HttpPath);

    ConnectedTcpSocket connection {};
    if (!ConnectResolvedTcp(ctx, config::HttpHost, config::HttpPort, connection, result.detail)) {
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
        close(connection.sockfd);
        return result;
    }

    const ssize_t send_rc = send(connection.sockfd, request, static_cast<std::size_t>(request_len), 0);
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "send failed: " + FormatErrno(errno);
        close(connection.sockfd);
        return result;
    }
    result.bytes_sent = static_cast<std::size_t>(send_rc);

    std::array<char, config::ReadBufferSize> buffer {};
    const ssize_t recv_rc = recv(connection.sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail = "recv failed: " + FormatErrno(errno);
        close(connection.sockfd);
        return result;
    }

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success = recv_rc > 0;
    result.detail =
        "connected_ip=" + connection.ip +
        " request_preview=" + EscapePreview(request, static_cast<std::size_t>(request_len), 96) +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);

    close(connection.sockfd);
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

    if (!SetSocketTimeouts(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }

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
        result.detail = "sslConnectionDoHandshake failed: " + FormatResult(rc) +
            CollectSslVerifyDiagnostics(ssl_connection);
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

ScenarioResult RunCurlHttpGet(AppContext& ctx) {
    ScenarioResult result { .name = "curl_http_get" };

    char url[512];
    const int url_len = std::snprintf(
        url,
        sizeof(url),
        "http://%s:%u%s",
        config::HttpHost,
        config::HttpPort,
        config::HttpPath);
    if (url_len <= 0 || static_cast<std::size_t>(url_len) >= sizeof(url)) {
        result.detail = "url formatting failed";
        return result;
    }

    logger::Status(ctx, "Running libcurl HTTP GET for %s", url);
    static_cast<void>(RunCurlTransfer(ctx, result.name.c_str(), url, false, result));
    return result;
}

ScenarioResult RunCurlHttpsGet(AppContext& ctx) {
    ScenarioResult result { .name = "curl_https_get" };

    char url[512];
    const int url_len = std::snprintf(
        url,
        sizeof(url),
        "https://%s:%u%s",
        config::HttpsHost,
        config::HttpsPort,
        config::HttpsPath);
    if (url_len <= 0 || static_cast<std::size_t>(url_len) >= sizeof(url)) {
        result.detail = "url formatting failed";
        return result;
    }

    logger::Status(ctx, "Running libcurl HTTPS GET for %s", url);
    static_cast<void>(RunCurlTransfer(ctx, result.name.c_str(), url, true, result));
    return result;
}

bool PrepareUdpTarget(AppContext& ctx, ScenarioResult& result, ResolvedEndpoint& endpoint) {
    if (config::UdpHost[0] == '\0' || config::UdpPort == 0) {
        result.skipped = true;
        result.detail = "udp target not configured";
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return false;
    }

    return ResolveIpv4(ctx, config::UdpHost, config::UdpPort, SOCK_DGRAM, endpoint, result.detail);
}

int OpenUdpSocket(ScenarioResult& result) {
    const int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        result.err = errno;
        result.detail = "socket failed: " + FormatErrno(errno);
    }
    return sockfd;
}

ScenarioResult RunUdpSocketOnly(AppContext& ctx) {
    ScenarioResult result { .name = "udp_socket_only" };
    logger::Status(ctx, "Opening UDP socket only");

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    result.success = true;
    result.detail = "socket opened and closed";
    close(sockfd);
    return result;
}

ScenarioResult RunUdpSocketSetSockOpt(AppContext& ctx) {
    ScenarioResult result { .name = "udp_socket_setsockopt" };
    logger::Status(ctx, "Opening UDP socket and setting timeouts");

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if (!SetSocketTimeouts(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }
    result.success = true;
    result.detail = "socket opened, timeouts set, and closed";
    close(sockfd);
    return result;
}

ScenarioResult RunUdpSetSockOptReuseAddr(AppContext& ctx) {
    ScenarioResult result { .name = "udp_setsockopt_reuseaddr" };
    logger::Status(ctx, "Opening UDP socket and setting SO_REUSEADDR");

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if (!TraceSetSockOptReuseAddr(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }

    result.success = true;
    result.detail = "socket opened, SO_REUSEADDR set, and closed";
    close(sockfd);
    return result;
}

ScenarioResult RunUdpSetSockOptRecvTimeout(AppContext& ctx) {
    ScenarioResult result { .name = "udp_setsockopt_recv_timeout" };
    logger::Status(ctx, "Opening UDP socket and setting SO_RCVTIMEO");

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if (!TraceSetSockOptRecvTimeout(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }

    result.success = true;
    result.detail = "socket opened, SO_RCVTIMEO set, and closed";
    close(sockfd);
    return result;
}

ScenarioResult RunUdpSetSockOptSendTimeout(AppContext& ctx) {
    ScenarioResult result { .name = "udp_setsockopt_send_timeout" };
    logger::Status(ctx, "Opening UDP socket and setting SO_SNDTIMEO");

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if (!TraceSetSockOptSendTimeout(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }

    result.success = true;
    result.detail = "socket opened, SO_SNDTIMEO set, and closed";
    close(sockfd);
    return result;
}

ScenarioResult RunUdpSendToOnly(AppContext& ctx) {
    ScenarioResult result { .name = "udp_sendto_only" };
    logger::Status(ctx, "Running UDP sendto-only to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint {};
    if (!PrepareUdpTarget(ctx, result, endpoint)) {
        return result;
    }

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if constexpr (config::EnableUdpSendToOnlyTimeouts) {
        if (!SetSocketTimeouts(ctx, sockfd, result)) {
            close(sockfd);
            return result;
        }
    } else {
        logger::Log(ctx, "udp_sendto_only skipping internal SO_RCVTIMEO/SO_SNDTIMEO");
    }

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

    result.success = true;
    result.bytes_sent = static_cast<std::size_t>(send_rc);
    result.detail =
        "target_ip=" + endpoint.ip +
        " internal_timeouts=" + (config::EnableUdpSendToOnlyTimeouts ? "enabled" : "disabled");
    close(sockfd);
    return result;
}

ScenarioResult RunUdpConnectSendOnly(AppContext& ctx) {
    ScenarioResult result { .name = "udp_connect_send_only" };
    logger::Status(ctx, "Running UDP connect+send-only to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint {};
    if (!PrepareUdpTarget(ctx, result, endpoint)) {
        return result;
    }

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if (!SetSocketTimeouts(ctx, sockfd, result)) {
        close(sockfd);
        return result;
    }
    if (connect(sockfd, reinterpret_cast<const struct sockaddr *>(&endpoint.addr), endpoint.addr_len) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    const ssize_t send_rc = send(sockfd, config::UdpPayload, std::strlen(config::UdpPayload), 0);
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "send failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    result.success = true;
    result.bytes_sent = static_cast<std::size_t>(send_rc);
    result.detail = "connected_ip=" + endpoint.ip;
    close(sockfd);
    return result;
}

ScenarioResult RunUdpEcho(AppContext& ctx) {
    ScenarioResult result { .name = "udp_echo" };

    logger::Status(ctx, "Running UDP echo to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint {};
    if (!PrepareUdpTarget(ctx, result, endpoint)) {
        return result;
    }

    const int sockfd = OpenUdpSocket(result);
    if (sockfd < 0) {
        return result;
    }

    if constexpr (config::EnableUdpEchoSocketTimeouts) {
        if (!SetSocketTimeouts(ctx, sockfd, result)) {
            close(sockfd);
            return result;
        }
    } else {
        logger::Log(ctx, "udp_echo skipping internal SO_RCVTIMEO/SO_SNDTIMEO");
    }

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

    if constexpr (config::EnableUdpEchoPoll) {
        pollfd poll_fd {
            .fd = sockfd,
            .events = POLLIN,
            .revents = 0,
        };
        const int poll_rc = poll(&poll_fd, 1, static_cast<int>(config::SocketTimeoutMs));
        if (poll_rc < 0) {
            result.err = errno;
            result.detail = "poll failed: " + FormatErrno(errno);
            close(sockfd);
            return result;
        }
        if (poll_rc == 0) {
            result.err = ETIMEDOUT;
            result.detail = "poll timed out waiting for UDP response";
            close(sockfd);
            return result;
        }
        if ((poll_fd.revents & POLLIN) == 0) {
            result.detail = "poll returned without POLLIN: revents=" +
                std::to_string(poll_fd.revents);
            close(sockfd);
            return result;
        }
    }

    std::array<char, config::ReadBufferSize> buffer {};
    sockaddr_storage source_addr {};
    socklen_t source_addr_len = sizeof(source_addr);
    const ssize_t recv_rc = recvfrom(
        sockfd,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr *>(&source_addr),
        &source_addr_len);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail =
            "recvfrom failed: " + FormatErrno(errno) +
            " returned_source_addr_len=" + std::to_string(source_addr_len);
        close(sockfd);
        return result;
    }

    const std::string source_detail = FormatSocketAddress(source_addr, source_addr_len);
    logger::Log(
        ctx,
        "udp_echo recvfrom bytes_received=%zd %s",
        recv_rc,
        source_detail.c_str());

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success =
        static_cast<std::size_t>(recv_rc) == std::strlen(config::UdpPayload) &&
        std::memcmp(buffer.data(), config::UdpPayload, result.bytes_received) == 0;
    result.detail =
        "target_ip=" + endpoint.ip +
        " socket_timeouts=" + (config::EnableUdpEchoSocketTimeouts ? "enabled" : "disabled") +
        " poll=" + (config::EnableUdpEchoPoll ? "enabled" : "disabled") +
        " " + source_detail +
        " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 96);
    close(sockfd);
    return result;
}

ScenarioResult RunConcurrentTcpBurst(AppContext& ctx) {
    ScenarioResult result { .name = "tcp_multi_connect" };
    logger::Status(
        ctx,
        "Running %u concurrent TCP sessions to %s:%u",
        config::ConcurrentSocketCount,
        config::TcpHost,
        config::TcpPort);

    std::vector<int> sockets;
    sockets.reserve(config::ConcurrentSocketCount);
    std::string first_ip;

    for (std::uint32_t i = 0; i < config::ConcurrentSocketCount; ++i) {
        ConnectedTcpSocket connection {};
        std::string detail;
        if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, detail)) {
            result.err = errno;
            result.detail =
                "connect_index=" + std::to_string(i) + " " + detail;
            for (int sockfd : sockets) {
                close(sockfd);
            }
            return result;
        }

        if (first_ip.empty()) {
            first_ip = connection.ip;
        }
        sockets.push_back(connection.sockfd);
    }

    if (config::ConcurrentSocketHoldMs > 0) {
        SleepMilliseconds(config::ConcurrentSocketHoldMs);
    }

    for (std::size_t i = 0; i < sockets.size(); ++i) {
        char payload[64];
        const int payload_len = std::snprintf(
            payload,
            sizeof(payload),
            "nxrv-requester-tcp-%zu",
            i);
        if (payload_len <= 0 || static_cast<std::size_t>(payload_len) >= sizeof(payload)) {
            result.detail = "payload formatting failed";
            for (int sockfd : sockets) {
                close(sockfd);
            }
            return result;
        }

        std::size_t sent = 0;
        std::string detail;
        if (!SendTcpPayload(sockets[i], payload, static_cast<std::size_t>(payload_len), sent, detail)) {
            result.err = errno;
            result.detail = "send_index=" + std::to_string(i) + " " + detail;
            for (int sockfd : sockets) {
                close(sockfd);
            }
            return result;
        }
        result.bytes_sent += sent;

        std::array<char, config::ReadBufferSize> buffer {};
        std::size_t received = 0;
        if (!ReceiveTcpPayload(sockets[i], buffer, received, detail)) {
            result.err = errno;
            result.detail = "recv_index=" + std::to_string(i) + " " + detail;
            for (int sockfd : sockets) {
                close(sockfd);
            }
            return result;
        }
        result.bytes_received += received;
    }

    for (int sockfd : sockets) {
        close(sockfd);
    }

    result.success = true;
    result.detail =
        "connected_ip=" + first_ip +
        " sockets=" + std::to_string(sockets.size()) +
        " hold_ms=" + std::to_string(config::ConcurrentSocketHoldMs);
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

void RunScenarioStep(
    AppContext& ctx,
    std::vector<ScenarioResult>& results,
    const ScenarioStep& step,
    const char *next_enabled_name) {
    if (!step.enabled) {
        ScenarioResult skipped { .name = step.name };
        skipped.skipped = true;
        skipped.detail = "disabled by config";
        results.push_back(std::move(skipped));
        LogScenarioResult(ctx, results.back());
        return;
    }

    results.push_back(step.fn(ctx));
    LogScenarioResult(ctx, results.back());

    if (next_enabled_name != nullptr && config::ScenarioStepDelayMs > 0) {
        logger::Log(
            ctx,
            "scenario_pause next=%s duration_ms=%u",
            next_enabled_name,
            config::ScenarioStepDelayMs);
        SleepMilliseconds(config::ScenarioStepDelayMs);
    }
}

const char *FindNextEnabledScenarioName(const ScenarioStep *steps, std::size_t count, std::size_t current_index) {
    for (std::size_t i = current_index + 1; i < count; ++i) {
        if (steps[i].enabled) {
            return steps[i].name;
        }
    }
    return nullptr;
}

} // namespace

std::vector<ScenarioResult> RunScenarios(AppContext& ctx) {
    const ScenarioStep steps[] = {
        { "manual_bsd_lifecycle", config::EnableScenarioManualBsdLifecycle, RunManualBsdLifecycle },
        { "environment_snapshot", config::EnableScenarioEnvironmentSnapshot, RunEnvironmentSnapshot },
        { "dns_resolve", config::EnableScenarioDnsResolve, RunDnsResolve },
        { "plain_tcp_connect", config::EnableScenarioPlainTcpConnect, RunPlainTcpConnect },
        { "tcp_idle_hold", config::EnableScenarioIdleTcpHold, RunIdleTcpHold },
        { "http_get", config::EnableScenarioHttpGet, RunHttpGet },
        { "https_get", config::EnableScenarioHttpsGet, RunHttpsGet },
        { "curl_http_get", config::EnableScenarioCurlHttpGet, RunCurlHttpGet },
        { "curl_https_get", config::EnableScenarioCurlHttpsGet, RunCurlHttpsGet },
        { "udp_socket_only", config::EnableScenarioUdpSocketOnly, RunUdpSocketOnly },
        { "udp_socket_setsockopt", config::EnableScenarioUdpSocketSetSockOpt, RunUdpSocketSetSockOpt },
        { "udp_setsockopt_reuseaddr", config::EnableScenarioUdpSetSockOptReuseAddr, RunUdpSetSockOptReuseAddr },
        { "udp_setsockopt_recv_timeout", config::EnableScenarioUdpSetSockOptRecvTimeout, RunUdpSetSockOptRecvTimeout },
        { "udp_setsockopt_send_timeout", config::EnableScenarioUdpSetSockOptSendTimeout, RunUdpSetSockOptSendTimeout },
        { "udp_sendto_only", config::EnableScenarioUdpSendToOnly, RunUdpSendToOnly },
        { "udp_connect_send_only", config::EnableScenarioUdpConnectSendOnly, RunUdpConnectSendOnly },
        { "udp_echo", config::EnableScenarioUdpEcho, RunUdpEcho },
        { "tcp_multi_connect", config::EnableScenarioConcurrentTcpBurst, RunConcurrentTcpBurst },
    };

    std::vector<ScenarioResult> results;
    results.reserve(std::size(steps));

    for (std::size_t i = 0; i < std::size(steps); ++i) {
        RunScenarioStep(
            ctx,
            results,
            steps[i],
            FindNextEnabledScenarioName(steps, std::size(steps), i));
    }

    return results;
}

} // namespace requester
