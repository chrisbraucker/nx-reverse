#include "scenarios.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.hpp"
#include "logger.hpp"
#include "manual_bsd_lifecycle.hpp"
#include "wgnx_packet_scenario.hpp"
#include "wgnx_tunnel_scenario.hpp"

namespace toolbox {

namespace {

constexpr std::size_t Ipv4StringCapacity = 16;
constexpr std::size_t SslVerifyErrorCapacity = 8;
constexpr std::size_t ConnectionTestBufferBytes = 64U * 1024U;
constexpr std::size_t MaximumHttpHeaderBytes = 4096;
constexpr int HttpStatusOk = 200;

struct ResolvedEndpoint {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    std::string ip;
};

struct ConnectedTcpSocket {
    int sockfd = -1;
    std::string ip;
};

struct ConnectionTestSpec {
    const char* name;
    const char* host;
    const char* path;
    std::size_t body_bytes;
    bool upload;
};

class ConnectionTestSocketScope {
  public:
    Result Initialize() {
        m_rc = socketInitialize(&config::SocketConfigApplication);
        m_initialized = R_SUCCEEDED(m_rc);
        return m_rc;
    }

    ~ConnectionTestSocketScope() {
        if (m_initialized) {
            socketExit();
        }
    }

  private:
    Result m_rc{};
    bool m_initialized{};
};

struct HttpResponse {
    int status{};
    std::size_t body_bytes{};
};

struct CurlResponseBuffer {
    std::array<char, config::CurlReadBufferSize> data{};
    std::size_t size = 0;
};

std::string FormatSocketAddress(const sockaddr_storage& address, socklen_t address_len) {
    std::string detail = "source_addr_len=" + std::to_string(address_len) + " source_family=" + std::to_string(address.ss_family);

    if (address.ss_family != AF_INET || address_len < sizeof(sockaddr_in)) {
        return detail + " source_ip=<unavailable> source_port=<unavailable>";
    }

    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    char ip_buffer[Ipv4StringCapacity]{};
    if (inet_ntop(AF_INET, &ipv4->sin_addr, ip_buffer, sizeof(ip_buffer)) == nullptr) {
        return detail + " source_ip=<inet_ntop-failed> source_port=" + std::to_string(ntohs(ipv4->sin_port));
    }

    return detail + " source_ip=" + ip_buffer + " source_port=" + std::to_string(ntohs(ipv4->sin_port));
}

using ScenarioFn = ScenarioResult (*)(AppContext& ctx);

struct ScenarioStep {
    ScenarioDescriptor descriptor;
    bool enabled;
    ScenarioFn fn;
};

bool ResolveIpv4(AppContext& ctx, const char* host, std::uint16_t port, int socktype, ResolvedEndpoint& out_endpoint, std::string& detail);

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<CurlResponseBuffer*>(userdata);
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

void ConfigureCurlCommon(CURL* curl, AppContext& ctx) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nxrv-toolbox/1");
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

bool RunCurlTransfer(AppContext& ctx, const char* scenario_name, const char* url, bool allow_insecure_tls, ScenarioResult& result) {
    if (!ctx.curl_initialized) {
        result.skipped = true;
        result.detail = std::string("curl unavailable: ") + curl_easy_strerror(ctx.curl_global_rc);
        logger::Log(ctx, "scenario=%s skipped: %s", scenario_name, result.detail.c_str());
        return true;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.detail = "curl_easy_init failed";
        return false;
    }

    CurlResponseBuffer response{};
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

    char* effective_url = nullptr;
    long response_code = 0;
    char* primary_ip = nullptr;
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url));
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code));
    static_cast<void>(curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip));

    result.success = true;
    result.bytes_received = response.size;
    result.detail = std::string("effective_url=") + (effective_url != nullptr ? effective_url : url) +
                    " primary_ip=" + (primary_ip != nullptr ? primary_ip : "unknown") + " http_code=" + std::to_string(response_code) +
                    " response_preview=" + EscapePreview(response.data.data(), response.size, 160);

    curl_easy_cleanup(curl);
    return true;
}

std::string CollectSslVerifyDiagnostics(SslConnection& ssl_connection) {
    std::array<Result, SslVerifyErrorCapacity> verify_errors{};
    std::uint32_t reported_error_count = 0;
    std::uint32_t copied_error_count = 0;

    const Result verify_errors_rc = sslConnectionGetVerifyCertErrors(
        &ssl_connection,
        &reported_error_count,
        &copied_error_count,
        verify_errors.data(),
        static_cast<std::uint32_t>(verify_errors.size())
    );
    const Result verify_error_rc = sslConnectionGetVerifyCertError(&ssl_connection);

    std::string detail = " verify_cert_error=" + FormatResult(verify_error_rc) + " verify_errors_rc=" + FormatResult(verify_errors_rc) +
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

const char* SocketOptionName(int level, int optname) {
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

int TraceSetSockOpt(AppContext& ctx, int sockfd, int level, int optname, const void* optval, socklen_t optlen, int& out_errno) {
    logger::Log(
        ctx,
        "setsockopt begin fd=%d level=%d opt=%d opt_name=%s optval=%p optlen=%u",
        sockfd,
        level,
        optname,
        SocketOptionName(level, optname),
        optval,
        static_cast<unsigned>(optlen)
    );

    errno = 0;
    const int rc = setsockopt(sockfd, level, optname, optval, optlen);
    const int saved_errno = errno;
    out_errno = saved_errno;

    logger::Log(
        ctx,
        "setsockopt complete fd=%d level=%d opt=%d opt_name=%s rc=%d "
        "errno=%d detail=%s",
        sockfd,
        level,
        optname,
        SocketOptionName(level, optname),
        rc,
        saved_errno,
        rc == 0 ? "success" : FormatErrno(saved_errno).c_str()
    );
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
    const struct timeval timeout{
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
    const struct timeval timeout{
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
    return TraceSetSockOptRecvTimeout(ctx, sockfd, result) && TraceSetSockOptSendTimeout(ctx, sockfd, result);
}

bool ConnectResolvedTcp(AppContext& ctx, const char* host, std::uint16_t port, ConnectedTcpSocket& out_socket, std::string& detail) {
    ResolvedEndpoint endpoint{};
    if (!ResolveIpv4(ctx, host, port, SOCK_STREAM, endpoint, detail)) {
        return false;
    }

    const int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        detail = "socket failed: " + FormatErrno(errno);
        return false;
    }

    ScenarioResult setopt_result{.name = "plain_tcp_connect_setsockopt"};
    if (!SetSocketTimeouts(ctx, sockfd, setopt_result)) {
        detail = setopt_result.detail;
        close(sockfd);
        return false;
    }

    if (connect(sockfd, reinterpret_cast<const struct sockaddr*>(&endpoint.addr), endpoint.addr_len) != 0) {
        detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return false;
    }

    out_socket.sockfd = sockfd;
    out_socket.ip = endpoint.ip;
    detail = "connected_ip=" + endpoint.ip;
    return true;
}

bool SendTcpPayload(int sockfd, const void* payload, std::size_t payload_size, std::size_t& bytes_sent, std::string& detail) {
    const ssize_t send_rc = send(sockfd, payload, payload_size, 0);
    if (send_rc < 0) {
        detail = "send failed: " + FormatErrno(errno);
        return false;
    }

    bytes_sent = static_cast<std::size_t>(send_rc);
    return true;
}

bool ReceiveTcpPayload(int sockfd, std::array<char, config::ReadBufferSize>& buffer, std::size_t& bytes_received, std::string& detail) {
    const ssize_t recv_rc = recv(sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        detail = "recv failed: " + FormatErrno(errno);
        return false;
    }

    bytes_received = static_cast<std::size_t>(recv_rc);
    return true;
}

bool ResolveIpv4(AppContext& ctx, const char* host, std::uint16_t port, int socktype, ResolvedEndpoint& out_endpoint, std::string& detail) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = socktype;
    hints.ai_protocol = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;

    char port_buffer[16];
    std::snprintf(port_buffer, sizeof(port_buffer), "%u", port);

    struct addrinfo* results = nullptr;
    const int gai_rc = getaddrinfo(host, port_buffer, &hints, &results);
    if (gai_rc != 0) {
        detail = std::string("getaddrinfo failed: ") + gai_strerror(gai_rc);
        logger::Log(ctx, "resolve host=%s port=%u socktype=%d failed: %s", host, port, socktype, detail.c_str());
        return false;
    }

    for (const struct addrinfo* it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET || it->ai_addrlen > sizeof(out_endpoint.addr)) {
            continue;
        }

        std::memcpy(&out_endpoint.addr, it->ai_addr, it->ai_addrlen);
        out_endpoint.addr_len = static_cast<socklen_t>(it->ai_addrlen);

        char ip_buffer[Ipv4StringCapacity];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
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
    ScenarioResult result{.name = "environment_snapshot"};

    if (!ctx.nifm_initialized) {
        result.skipped = true;
        result.rc = ctx.nifm_initialize_rc;
        result.detail = "nifm unavailable: " + FormatResult(ctx.nifm_initialize_rc);
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return result;
    }

    logger::Status(ctx, "Running NIFM status check");

    ctx.env.internet_status_rc =
        nifmGetInternetConnectionStatus(&ctx.env.connection_type, &ctx.env.wifi_strength, &ctx.env.connection_status);
    ctx.env.current_ip_rc = nifmGetCurrentIpAddress(&ctx.env.current_ip);
    ctx.env.ip_config_rc = nifmGetCurrentIpConfigInfo(
        &ctx.env.current_ip,
        &ctx.env.subnet_mask,
        &ctx.env.gateway,
        &ctx.env.primary_dns,
        &ctx.env.secondary_dns
    );

    result.rc = ctx.env.internet_status_rc;
    result.success = R_SUCCEEDED(ctx.env.current_ip_rc);
    result.detail = "status_rc=" + FormatResult(ctx.env.internet_status_rc) +
                    " type=" + FormatInternetConnectionType(ctx.env.connection_type) +
                    " wifi_strength=" + std::to_string(ctx.env.wifi_strength) +
                    " connection_status=" + FormatInternetConnectionStatus(ctx.env.connection_status) +
                    " ip_rc=" + FormatResult(ctx.env.current_ip_rc) + " current_ip=" + FormatIpv4(ctx.env.current_ip) +
                    " ip_config_rc=" + FormatResult(ctx.env.ip_config_rc) + " subnet=" + FormatIpv4(ctx.env.subnet_mask) +
                    " gateway=" + FormatIpv4(ctx.env.gateway) + " dns1=" + FormatIpv4(ctx.env.primary_dns) +
                    " dns2=" + FormatIpv4(ctx.env.secondary_dns);

    logger::Log(ctx, "scenario=%s %s", result.name.c_str(), result.detail.c_str());
    return result;
}

ScenarioResult RunDnsResolve(AppContext& ctx) {
    ScenarioResult result{.name = "dns_resolve"};
    logger::Status(ctx, "Running DNS resolve for %s", config::DnsHostname);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* results = nullptr;
    const int gai_rc = getaddrinfo(config::DnsHostname, nullptr, &hints, &results);
    if (gai_rc != 0) {
        result.err = gai_rc;
        result.detail = std::string("getaddrinfo failed: ") + gai_strerror(gai_rc);
        logger::Log(ctx, "scenario=%s host=%s failed: %s", result.name.c_str(), config::DnsHostname, result.detail.c_str());
        return result;
    }

    std::string addresses;
    for (const struct addrinfo* it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET) {
            continue;
        }

        char ip_buffer[Ipv4StringCapacity];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
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
    ScenarioResult result{.name = "plain_tcp_connect"};
    logger::Status(ctx, "Connecting TCP to %s:%u", config::TcpHost, config::TcpPort);

    ConnectedTcpSocket connection{};
    if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, result.detail)) {
        return result;
    }

    std::array<char, config::ReadBufferSize> buffer{};
    if (!SendTcpPayload(connection.sockfd, config::TcpPayload, std::strlen(config::TcpPayload), result.bytes_sent, result.detail)) {
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
    result.detail = "connected_ip=" + connection.ip +
                    " request_preview=" + EscapePreview(config::TcpPayload, std::strlen(config::TcpPayload), 96) +
                    " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);
    close(connection.sockfd);
    return result;
}

ScenarioResult RunIdleTcpHold(AppContext& ctx) {
    ScenarioResult result{.name = "tcp_idle_hold"};
    logger::Status(ctx, "Holding idle TCP socket to %s:%u for %u ms", config::TcpHost, config::TcpPort, config::IdleSocketHoldMs);

    ConnectedTcpSocket connection{};
    if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, result.detail)) {
        return result;
    }

    if (config::IdleSocketHoldMs > 0) {
        SleepMilliseconds(config::IdleSocketHoldMs);
    }

    result.success = true;
    result.detail = "connected_ip=" + connection.ip + " hold_ms=" + std::to_string(config::IdleSocketHoldMs);
    close(connection.sockfd);
    return result;
}

ScenarioResult RunHttpGet(AppContext& ctx) {
    ScenarioResult result{.name = "http_get"};
    logger::Status(ctx, "Running HTTP GET for http://%s%s", config::HttpHost, config::HttpPath);

    ConnectedTcpSocket connection{};
    if (!ConnectResolvedTcp(ctx, config::HttpHost, config::HttpPort, connection, result.detail)) {
        return result;
    }

    char request[512];
    const int request_len = std::snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
        "nxrv-toolbox/1\r\nConnection: close\r\n\r\n",
        config::HttpPath,
        config::HttpHost
    );
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

    std::array<char, config::ReadBufferSize> buffer{};
    const ssize_t recv_rc = recv(connection.sockfd, buffer.data(), buffer.size(), 0);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail = "recv failed: " + FormatErrno(errno);
        close(connection.sockfd);
        return result;
    }

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success = recv_rc > 0;
    result.detail = "connected_ip=" + connection.ip +
                    " request_preview=" + EscapePreview(request, static_cast<std::size_t>(request_len), 96) +
                    " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);

    close(connection.sockfd);
    return result;
}

ScenarioResult RunHttpsGet(AppContext& ctx) {
    ScenarioResult result{.name = "https_get"};

    if (!ctx.ssl_initialized) {
        result.skipped = true;
        result.rc = ctx.ssl_initialize_rc;
        result.detail = "ssl unavailable: " + FormatResult(ctx.ssl_initialize_rc);
        logger::Status(ctx, "Running HTTPS GET for https://%s%s", config::HttpsHost, config::HttpsPath);
        logger::Log(ctx, "scenario=%s skipped: %s", result.name.c_str(), result.detail.c_str());
        return result;
    }

    logger::Status(ctx, "Running HTTPS GET for https://%s%s", config::HttpsHost, config::HttpsPath);

    ResolvedEndpoint endpoint{};
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

    if (connect(sockfd, reinterpret_cast<const struct sockaddr*>(&endpoint.addr), endpoint.addr_len) != 0) {
        result.err = errno;
        result.detail = "connect failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    SslContext ssl_context{};
    SslConnection ssl_connection{};
    Result rc = sslCreateContext(&ssl_context, SslVersion_Auto);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslCreateContext failed: " + FormatResult(rc);
        close(sockfd);
        return result;
    }

    rc = sslContextCreateConnection(&ssl_context, &ssl_connection);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslContextCreateConnection failed: " + FormatResult(rc);
        sslContextClose(&ssl_context);
        close(sockfd);
        return result;
    }
    const int transferred_sockfd = socketSslConnectionSetSocketDescriptor(&ssl_connection, sockfd);
    if (transferred_sockfd < 0 && errno != ENOENT) {
        result.err = errno;
        result.detail = "socket descriptor transfer failed: " + FormatErrno(errno);
        sslConnectionClose(&ssl_connection);
        sslContextClose(&ssl_context);
        close(sockfd);
        return result;
    }
    const int owned_sockfd = transferred_sockfd >= 0 ? transferred_sockfd : -1;

    rc = sslConnectionSetHostName(&ssl_connection, config::HttpsHost, std::strlen(config::HttpsHost));
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionSetHostName failed: " + FormatResult(rc);
        sslConnectionClose(&ssl_connection);
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    rc = sslConnectionSetVerifyOption(&ssl_connection, SslVerifyOption_PeerCa | SslVerifyOption_HostName | SslVerifyOption_DateCheck);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionSetVerifyOption failed: " + FormatResult(rc);
        sslConnectionClose(&ssl_connection);
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
        sslConnectionClose(&ssl_connection);
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    rc = sslConnectionDoHandshake(&ssl_connection, nullptr, nullptr, nullptr, 0);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionDoHandshake failed: " + FormatResult(rc) + CollectSslVerifyDiagnostics(ssl_connection);
        sslConnectionClose(&ssl_connection);
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
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
        "nxrv-toolbox/1\r\nConnection: close\r\n\r\n",
        config::HttpsPath,
        config::HttpsHost
    );
    if (request_len <= 0 || static_cast<std::size_t>(request_len) >= sizeof(request)) {
        result.detail = "request formatting failed";
        sslConnectionClose(&ssl_connection);
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
        sslConnectionClose(&ssl_connection);
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }
    result.bytes_sent = write_size;

    std::array<char, config::ReadBufferSize> buffer{};
    std::uint32_t read_size = 0;
    rc = sslConnectionRead(&ssl_connection, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &read_size);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "sslConnectionRead failed: " + FormatResult(rc);
        sslConnectionClose(&ssl_connection);
        if (owned_sockfd >= 0) {
            close(owned_sockfd);
        }
        sslContextClose(&ssl_context);
        return result;
    }

    SslCipherInfo cipher_info{};
    rc = sslConnectionGetCipherInfo(&ssl_connection, &cipher_info);
    const std::string cipher_summary = R_SUCCEEDED(rc) ? std::string(cipher_info.protocol_version) + "/" + cipher_info.cipher
                                                       : std::string("cipher-info-failed:") + FormatResult(rc);

    result.bytes_received = read_size;
    result.success = read_size > 0;
    result.detail = "connected_ip=" + endpoint.ip + " cipher=" + cipher_summary +
                    " request_preview=" + EscapePreview(request, static_cast<std::size_t>(request_len), 96) +
                    " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 160);

    sslConnectionClose(&ssl_connection);
    if (owned_sockfd >= 0) {
        close(owned_sockfd);
    }
    sslContextClose(&ssl_context);
    return result;
}

ScenarioResult RunCurlHttpGet(AppContext& ctx) {
    ScenarioResult result{.name = "curl_http_get"};

    char url[512];
    const int url_len = std::snprintf(url, sizeof(url), "http://%s:%u%s", config::HttpHost, config::HttpPort, config::HttpPath);
    if (url_len <= 0 || static_cast<std::size_t>(url_len) >= sizeof(url)) {
        result.detail = "url formatting failed";
        return result;
    }

    logger::Status(ctx, "Running libcurl HTTP GET for %s", url);
    static_cast<void>(RunCurlTransfer(ctx, result.name.c_str(), url, false, result));
    return result;
}

ScenarioResult RunCurlHttpsGet(AppContext& ctx) {
    ScenarioResult result{.name = "curl_https_get"};

    char url[512];
    const int url_len = std::snprintf(url, sizeof(url), "https://%s:%u%s", config::HttpsHost, config::HttpsPort, config::HttpsPath);
    if (url_len <= 0 || static_cast<std::size_t>(url_len) >= sizeof(url)) {
        result.detail = "url formatting failed";
        return result;
    }

    logger::Status(ctx, "Running libcurl HTTPS GET for %s", url);
    static_cast<void>(RunCurlTransfer(ctx, result.name.c_str(), url, true, result));
    return result;
}

bool SetConnectionTestTimeouts(const int sockfd, ScenarioResult& result) {
    const struct timeval timeout{
        .tv_sec = static_cast<long>(config::ConnectionTestTimeoutMs / 1000U),
        .tv_usec = static_cast<long>((config::ConnectionTestTimeoutMs % 1000U) * 1000U),
    };
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        result.err = errno;
        result.detail = "connection test timeout configuration failed: " + FormatErrno(errno);
        return false;
    }
    return true;
}

bool SendAll(const int sockfd, const void* const data, const std::size_t size, ScenarioResult& result) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t rc = send(sockfd, bytes + sent, size - sent, 0);
        if (rc <= 0) {
            result.err = rc < 0 ? errno : EPIPE;
            result.detail = "send failed: " + FormatErrno(result.err);
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

bool ParseHttpStatus(const std::string& header, int& status) {
    if (header.size() < 12 || !header.starts_with("HTTP/1.") || header[8] != ' ') {
        return false;
    }
    const char* const digits = header.data() + 9;
    if (digits[0] < '0' || digits[0] > '9' || digits[1] < '0' || digits[1] > '9' || digits[2] < '0' || digits[2] > '9') {
        return false;
    }
    status = (digits[0] - '0') * 100 + (digits[1] - '0') * 10 + (digits[2] - '0');
    return true;
}

bool ReceiveHttpResponse(const int sockfd, HttpResponse& response, ScenarioResult& result) {
    std::vector<char> buffer(ConnectionTestBufferBytes);
    std::string header;
    header.reserve(MaximumHttpHeaderBytes);
    bool received_header = false;

    for (;;) {
        const ssize_t rc = recv(sockfd, buffer.data(), buffer.size(), 0);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            result.err = errno;
            result.detail = "receive failed: " + FormatErrno(errno);
            return false;
        }

        const std::size_t received = static_cast<std::size_t>(rc);
        if (received_header) {
            response.body_bytes += received;
            continue;
        }

        const std::size_t available = MaximumHttpHeaderBytes - header.size();
        const std::size_t copied = std::min(available, received);
        header.append(buffer.data(), copied);
        const std::size_t header_end = header.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            if (header.size() == MaximumHttpHeaderBytes) {
                result.detail = "HTTP response headers exceed the bounded parser limit";
                return false;
            }
            continue;
        }
        if (!ParseHttpStatus(header, response.status)) {
            result.detail = "invalid HTTP response status";
            return false;
        }
        received_header = true;
        response.body_bytes += received - (header_end + 4U);
    }

    if (!received_header) {
        result.detail = "connection closed before HTTP response headers";
        return false;
    }
    return true;
}

std::string FormatConnectionTestDetail(
    const ConnectedTcpSocket& connection, const HttpResponse& response, const std::size_t transfer_bytes, const std::uint64_t started_ticks
) {
    const std::uint64_t elapsed_ticks = armGetSystemTick() - started_ticks;
    const std::uint64_t tick_frequency = armGetSystemTickFreq();
    const double elapsed_seconds = static_cast<double>(elapsed_ticks) / static_cast<double>(tick_frequency);
    const double throughput = elapsed_seconds > 0.0 ? static_cast<double>(transfer_bytes) / elapsed_seconds / 1'000'000.0 : 0.0;
    char timing[128];
    std::snprintf(timing, sizeof(timing), "elapsed_ms=%.2f throughput=%.2f MB/s", elapsed_seconds * 1000.0, throughput);
    return "connected_ip=" + connection.ip + " http_status=" + std::to_string(response.status) +
           " response_body_bytes=" + std::to_string(response.body_bytes) + " " + timing;
}

ScenarioResult RunConnectionTest(AppContext& ctx, const ConnectionTestSpec& spec) {
    ScenarioResult result{.name = spec.name};
    logger::Status(ctx, "Running %s http://%s%s", spec.name, spec.host, spec.path);

    ConnectionTestSocketScope socket_scope;
    const Result socket_rc = socket_scope.Initialize();
    if (R_FAILED(socket_rc)) {
        result.rc = socket_rc;
        result.detail = "socketInitialize failed: " + FormatResult(socket_rc);
        return result;
    }

    const std::uint64_t started_ticks = armGetSystemTick();
    ConnectedTcpSocket connection{};
    if (!ConnectResolvedTcp(ctx, spec.host, config::ConnectionTestPort, connection, result.detail)) {
        return result;
    }
    if (!SetConnectionTestTimeouts(connection.sockfd, result)) {
        close(connection.sockfd);
        return result;
    }

    std::array<char, 512> request{};
    const int request_size = spec.upload ? std::snprintf(
                                               request.data(),
                                               request.size(),
                                               "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Nintendo NX\r\nAccept: */*\r\n"
                                               "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n",
                                               spec.path,
                                               spec.host,
                                               spec.body_bytes
                                           )
                                         : std::snprintf(
                                               request.data(),
                                               request.size(),
                                               "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Nintendo NX\r\nAccept: */*\r\n\r\n",
                                               spec.path,
                                               spec.host
                                           );
    if (request_size <= 0 || static_cast<std::size_t>(request_size) >= request.size()) {
        result.detail = "request formatting failed";
        close(connection.sockfd);
        return result;
    }
    if (!SendAll(connection.sockfd, request.data(), static_cast<std::size_t>(request_size), result)) {
        close(connection.sockfd);
        return result;
    }

    if (spec.upload) {
        std::vector<std::uint8_t> payload(std::min(spec.body_bytes, ConnectionTestBufferBytes));
        randomGet(payload.data(), payload.size());
        for (std::size_t remaining = spec.body_bytes; remaining > 0;) {
            const std::size_t chunk_size = std::min(remaining, payload.size());
            if (!SendAll(connection.sockfd, payload.data(), chunk_size, result)) {
                close(connection.sockfd);
                return result;
            }
            remaining -= chunk_size;
        }
        result.bytes_sent = spec.body_bytes;
    } else {
        result.bytes_sent = static_cast<std::size_t>(request_size);
    }

    HttpResponse response{};
    if (!ReceiveHttpResponse(connection.sockfd, response, result)) {
        close(connection.sockfd);
        return result;
    }
    close(connection.sockfd);

    result.bytes_received = response.body_bytes;
    const std::size_t expected_response_bytes = spec.upload ? 0 : spec.body_bytes;
    result.success = response.status == HttpStatusOk && response.body_bytes == expected_response_bytes;
    result.detail = FormatConnectionTestDetail(connection, response, spec.upload ? spec.body_bytes : response.body_bytes, started_ticks);
    if (!result.success) {
        result.detail += " expected_response_body_bytes=" + std::to_string(expected_response_bytes);
    }
    return result;
}

ScenarioResult RunConnectionTestDownload30M(AppContext& ctx) {
    return RunConnectionTest(
        ctx,
        {.name = "Download test 30M",
         .host = config::ConnectionTestDownloadHost,
         .path = "/30m",
         .body_bytes = config::ConnectionTestTransfer30MBytes}
    );
}

ScenarioResult RunConnectionTestUpload1M(AppContext& ctx) {
    return RunConnectionTest(
        ctx,
        {.name = "Upload test 1M",
         .host = config::ConnectionTestUploadHost,
         .path = "/1m",
         .body_bytes = config::ConnectionTestUpload1MBytes,
         .upload = true}
    );
}

ScenarioResult RunConnectionTestUpload30M(AppContext& ctx) {
    return RunConnectionTest(
        ctx,
        {.name = "Upload test 30M",
         .host = config::ConnectionTestUploadHost,
         .path = "/30m",
         .body_bytes = config::ConnectionTestTransfer30MBytes,
         .upload = true}
    );
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
    ScenarioResult result{.name = "udp_socket_only"};
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
    ScenarioResult result{.name = "udp_socket_setsockopt"};
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
    ScenarioResult result{.name = "udp_setsockopt_reuseaddr"};
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
    ScenarioResult result{.name = "udp_setsockopt_recv_timeout"};
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
    ScenarioResult result{.name = "udp_setsockopt_send_timeout"};
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
    ScenarioResult result{.name = "udp_sendto_only"};
    logger::Status(ctx, "Running UDP sendto-only to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint{};
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
        reinterpret_cast<const struct sockaddr*>(&endpoint.addr),
        endpoint.addr_len
    );
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "sendto failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }

    result.success = true;
    result.bytes_sent = static_cast<std::size_t>(send_rc);
    result.detail = "target_ip=" + endpoint.ip + " internal_timeouts=" + (config::EnableUdpSendToOnlyTimeouts ? "enabled" : "disabled");
    close(sockfd);
    return result;
}

ScenarioResult RunUdpConnectSendOnly(AppContext& ctx) {
    ScenarioResult result{.name = "udp_connect_send_only"};
    logger::Status(ctx, "Running UDP connect+send-only to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint{};
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
    if (connect(sockfd, reinterpret_cast<const struct sockaddr*>(&endpoint.addr), endpoint.addr_len) != 0) {
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
    ScenarioResult result{.name = "udp_echo"};

    logger::Status(ctx, "Running UDP echo to %s:%u", config::UdpHost, config::UdpPort);

    ResolvedEndpoint endpoint{};
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
        reinterpret_cast<const struct sockaddr*>(&endpoint.addr),
        endpoint.addr_len
    );
    if (send_rc < 0) {
        result.err = errno;
        result.detail = "sendto failed: " + FormatErrno(errno);
        close(sockfd);
        return result;
    }
    result.bytes_sent = static_cast<std::size_t>(send_rc);

    if constexpr (config::EnableUdpEchoPoll) {
        pollfd poll_fd{
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
            result.detail = "poll returned without POLLIN: revents=" + std::to_string(poll_fd.revents);
            close(sockfd);
            return result;
        }
    }

    std::array<char, config::ReadBufferSize> buffer{};
    sockaddr_storage source_addr{};
    socklen_t source_addr_len = sizeof(source_addr);
    const ssize_t recv_rc = recvfrom(sockfd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&source_addr), &source_addr_len);
    if (recv_rc < 0) {
        result.err = errno;
        result.detail = "recvfrom failed: " + FormatErrno(errno) + " returned_source_addr_len=" + std::to_string(source_addr_len);
        close(sockfd);
        return result;
    }

    const std::string source_detail = FormatSocketAddress(source_addr, source_addr_len);
    TOOLBOX_LOG_PACKET(ctx, "udp_echo recvfrom bytes_received=%zd %s", recv_rc, source_detail.c_str());

    result.bytes_received = static_cast<std::size_t>(recv_rc);
    result.success = static_cast<std::size_t>(recv_rc) == std::strlen(config::UdpPayload) &&
                     std::memcmp(buffer.data(), config::UdpPayload, result.bytes_received) == 0;
    result.detail = "target_ip=" + endpoint.ip + " socket_timeouts=" + (config::EnableUdpEchoSocketTimeouts ? "enabled" : "disabled") +
                    " poll=" + (config::EnableUdpEchoPoll ? "enabled" : "disabled") + " " + source_detail +
                    " response_preview=" + EscapePreview(buffer.data(), result.bytes_received, 96);
    close(sockfd);
    return result;
}

ScenarioResult RunConcurrentTcpBurst(AppContext& ctx) {
    ScenarioResult result{.name = "tcp_multi_connect"};
    logger::Status(ctx, "Running %u concurrent TCP sessions to %s:%u", config::ConcurrentSocketCount, config::TcpHost, config::TcpPort);

    std::vector<int> sockets;
    sockets.reserve(config::ConcurrentSocketCount);
    std::string first_ip;

    for (std::uint32_t i = 0; i < config::ConcurrentSocketCount; ++i) {
        ConnectedTcpSocket connection{};
        std::string detail;
        if (!ConnectResolvedTcp(ctx, config::TcpHost, config::TcpPort, connection, detail)) {
            result.err = errno;
            result.detail = "connect_index=" + std::to_string(i) + " " + detail;
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
        const int payload_len = std::snprintf(payload, sizeof(payload), "nxrv-toolbox-tcp-%zu", i);
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

        std::array<char, config::ReadBufferSize> buffer{};
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
    result.detail = "connected_ip=" + first_ip + " sockets=" + std::to_string(sockets.size()) +
                    " hold_ms=" + std::to_string(config::ConcurrentSocketHoldMs);
    return result;
}

void LogScenarioResult(AppContext& ctx, const ScenarioResult& result) {
    logger::Log(
        ctx,
        "scenario=%s success=%d skipped=%d rc=%s errno=%s bytes_sent=%zu "
        "bytes_received=%zu detail=%s",
        result.name.c_str(),
        result.success ? 1 : 0,
        result.skipped ? 1 : 0,
        FormatResult(result.rc).c_str(),
        FormatErrno(result.err).c_str(),
        result.bytes_sent,
        result.bytes_received,
        result.detail.c_str()
    );
}

const std::array<ScenarioStep, 23>& ScenarioSteps() {
    static const std::array<ScenarioStep, 23> steps = {{
        {{"Download test 30M",
          "Fetches 30MiB from the official endpoint natively and reports end-to-end throughput.",
          "GET http://ctest-dl-lp1.cdn.nintendo.net/30m HTTP/1.0, User-Agent: Nintendo NX, Accept: */*"},
         config::EnableScenarioConnectionTestDownload30M,
         RunConnectionTestDownload30M},
        {{"Upload test 1M",
          "Posts a 1 MiB body the official endpoint natively and reports end-to-end throughput.",
          "POST http://ctest-ul-lp1.cdn.nintendo.net/1m HTTP/1.0, User-Agent: Nintendo NX, Accept: */*, "
          "application/x-www-form-urlencoded"},
         config::EnableScenarioConnectionTestUpload1M,
         RunConnectionTestUpload1M},
        {{"Upload test 30M",
          "Posts a 30 MiB body to the official endpoint natively and reports end-to-end throughput.",
          "POST http://ctest-ul-lp1.cdn.nintendo.net/30m HTTP/1.0, User-Agent: Nintendo NX, Accept: */*, "
          "application/x-www-form-urlencoded"},
         config::EnableScenarioConnectionTestUpload30M,
         RunConnectionTestUpload30M},
        {{"manual_bsd_lifecycle",
          "Opens bsd:s and reproduces the selected raw client-registration lifecycle. It closes every service and transfer-memory "
          "handle "
          "before returning.",
          "Root session, transfer memory, and RegisterClient are enabled. Monitor, StartMonitoring, and root clone are disabled."},
         config::EnableScenarioManualBsdLifecycle,
         RunManualBsdLifecycle},
        {{"environment_snapshot",
          "Reads NIFM connection status, the current address, and the configured network information. It does not send network "
          "traffic.",
          "Requires NIFM initialization, which is disabled in this build."},
         config::EnableScenarioEnvironmentSnapshot,
         RunEnvironmentSnapshot},
        {{"dns_resolve",
          "Resolves the compiled IPv4 hostname through Horizon's resolver path. It records every IPv4 address returned.",
          "host=example.com"},
         config::EnableScenarioDnsResolve,
         RunDnsResolve},
        {{"plain_tcp_connect",
          "Connects to the compiled TCP endpoint, sends a small payload, and reads one response. It exercises ordinary BSD TCP "
          "operations.",
          "host=example.com port=80 payload=nxrv-toolbox-tcp timeout=5000 ms"},
         config::EnableScenarioPlainTcpConnect,
         RunPlainTcpConnect},
        {{"tcp_idle_hold",
          "Connects to the compiled TCP endpoint and leaves the socket idle briefly. It is useful for observing connection lifetime "
          "handling.",
          "host=example.com port=80 hold=1500 ms"},
         config::EnableScenarioIdleTcpHold,
         RunIdleTcpHold},
        {{"http_get",
          "Makes one manual HTTP/1.1 GET through a BSD TCP socket. It logs a bounded response preview.",
          "host=example.com port=80 path=/"},
         config::EnableScenarioHttpGet,
         RunHttpGet},
        {{"https_get",
          "Makes one HTTPS GET through libnx ssl after resolving and opening the BSD socket itself. It verifies the peer CA, hostname, "
          "and "
          "date.",
          "host=example.com port=443 path=/ timeout=5000 ms, SSL initialization required"},
         config::EnableScenarioHttpsGet,
         RunHttpsGet},
        {{"curl_http_get",
          "Makes one HTTP GET through libcurl. It is a higher-level comparison against the manual HTTP scenario.",
          "host=example.com port=80 path=/, curl initialization required"},
         config::EnableScenarioCurlHttpGet,
         RunCurlHttpGet},
        {{"curl_https_get",
          "Makes one HTTPS GET through libcurl with certificate verification disabled. Use it only against a controlled endpoint.",
          "host=example.com port=443 path=/ timeout=5000 ms, curl initialization required"},
         config::EnableScenarioCurlHttpsGet,
         RunCurlHttpsGet},
        {{"udp_socket_only",
          "Opens and closes an IPv4 UDP socket without sending traffic. It isolates socket creation and teardown.",
          "No endpoint or socket options."},
         config::EnableScenarioUdpSocketOnly,
         RunUdpSocketOnly},
        {{"udp_socket_setsockopt",
          "Opens a UDP socket and applies receive and send timeouts. It isolates the common timeout socket options.",
          "timeout=5000 ms"},
         config::EnableScenarioUdpSocketSetSockOpt,
         RunUdpSocketSetSockOpt},
        {{"udp_setsockopt_reuseaddr", "Opens a UDP socket and applies SO_REUSEADDR. It isolates that one socket option.", "SO_REUSEADDR=1"},
         config::EnableScenarioUdpSetSockOptReuseAddr,
         RunUdpSetSockOptReuseAddr},
        {{"udp_setsockopt_recv_timeout",
          "Opens a UDP socket and applies SO_RCVTIMEO. It isolates the receive timeout socket option.",
          "SO_RCVTIMEO=5000 ms"},
         config::EnableScenarioUdpSetSockOptRecvTimeout,
         RunUdpSetSockOptRecvTimeout},
        {{"udp_setsockopt_send_timeout",
          "Opens a UDP socket and applies SO_SNDTIMEO. It isolates the send timeout socket option.",
          "SO_SNDTIMEO=5000 ms"},
         config::EnableScenarioUdpSetSockOptSendTimeout,
         RunUdpSetSockOptSendTimeout},
        {{"udp_sendto_only",
          "Resolves the configured UDP target and sends one datagram with sendto. It does not wait for a reply.",
          "host=<unset> port=0 payload=nxrv-toolbox-udp, internal timeouts disabled"},
         config::EnableScenarioUdpSendToOnly,
         RunUdpSendToOnly},
        {{"udp_connect_send_only",
          "Resolves the configured UDP target, connects the socket, and sends one datagram. It does not wait for a reply.",
          "host=<unset> port=0 payload=nxrv-toolbox-udp timeout=5000 ms"},
         config::EnableScenarioUdpConnectSendOnly,
         RunUdpConnectSendOnly},
        {{"udp_echo",
          "Resolves the configured UDP target, sends one datagram, and validates an identical reply. It uses poll before receive.",
          "host=<unset> port=0 payload=nxrv-toolbox-udp poll timeout=5000 ms"},
         config::EnableScenarioUdpEcho,
         RunUdpEcho},
        {{"tcp_multi_connect",
          "Opens several TCP connections, holds them, then exchanges one payload on each. It exercises concurrent BSD TCP sessions.",
          "host=example.com port=80 sockets=3 hold=1000 ms"},
         config::EnableScenarioConcurrentTcpBurst,
         RunConcurrentTcpBurst},
        {{"wgnx_packet_udp_echo",
          "Builds one complete inner IPv4 and UDP packet and submits it through wgnx:ctl. It waits for and validates the matching "
          "returned "
          "packet.",
          "source=10.0.0.2:39000 destination=10.1.0.2:29000 timeout=5000 ms"},
         config::EnableScenarioWgnxUdpEcho,
         RunWgnxPacketUdpEcho},
        {{"wgnx_tunnel_udp_workload",
          "Runs the direct wgnx:tun UDP workload with the compiled defaults. It validates queued datagrams and optional echo replies.",
          "destination=10.1.0.2:29000 payload=48 B datagrams=1 flows=1 echo=true timeout=5000 ms"},
         config::EnableScenarioWgnxTunnelUdpWorkload,
         RunWgnxTunnelUdpWorkload},
    }};
    return steps;
}

void RunScenarioStep(AppContext& ctx, std::vector<ScenarioResult>& results, const ScenarioStep& step, const char* next_enabled_name) {
    if (!step.enabled) {
        ScenarioResult skipped{.name = std::string(step.descriptor.name)};
        skipped.skipped = true;
        skipped.detail = "disabled by config";
        results.push_back(std::move(skipped));
        LogScenarioResult(ctx, results.back());
        return;
    }

    results.push_back(step.fn(ctx));
    LogScenarioResult(ctx, results.back());

    if (next_enabled_name != nullptr && config::ScenarioStepDelayMs > 0) {
        logger::Log(ctx, "scenario_pause next=%s duration_ms=%u", next_enabled_name, config::ScenarioStepDelayMs);
        SleepMilliseconds(config::ScenarioStepDelayMs);
    }
}

const char* FindNextEnabledScenarioName(const ScenarioStep* steps, std::size_t count, std::size_t current_index) {
    for (std::size_t i = current_index + 1; i < count; ++i) {
        if (steps[i].enabled) {
            return steps[i].descriptor.name.data();
        }
    }
    return nullptr;
}

} // namespace

std::span<const ScenarioDescriptor> AvailableScenarios() {
    static const std::array<ScenarioDescriptor, 23> descriptors = [] {
        std::array<ScenarioDescriptor, 23> result{};
        const auto& steps = ScenarioSteps();
        for (std::size_t index = 0; index < steps.size(); ++index) {
            result[index] = steps[index].descriptor;
        }
        return result;
    }();
    return descriptors;
}

ScenarioResult RunScenario(AppContext& ctx, std::string_view name) {
    for (const ScenarioStep& step : ScenarioSteps()) {
        if (step.descriptor.name == name) {
            ScenarioResult result = step.fn(ctx);
            LogScenarioResult(ctx, result);
            return result;
        }
    }

    ScenarioResult result{.name = std::string(name), .skipped = true, .detail = "unknown scenario"};
    LogScenarioResult(ctx, result);
    return result;
}

std::vector<ScenarioResult> RunScenarios(AppContext& ctx) {
    const auto& steps = ScenarioSteps();

    std::vector<ScenarioResult> results;
    results.reserve(std::size(steps));

    for (std::size_t i = 0; i < std::size(steps); ++i) {
        RunScenarioStep(ctx, results, steps[i], FindNextEnabledScenarioName(steps.data(), steps.size(), i));
    }

    return results;
}

} // namespace toolbox
