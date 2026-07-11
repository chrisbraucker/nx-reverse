#pragma once

#include <cstddef>
#include <cstdint>

namespace requester::config {

constexpr char LogDirectory[] = "sdmc:/nxrv/requester";

constexpr SocketInitConfig SocketConfigApplication = {
    .tcp_tx_buf_size = 1024 * 64,
    .tcp_rx_buf_size = 1024 * 64,
    .tcp_tx_buf_max_size = 1024 * 1024 * 4,
    .tcp_rx_buf_max_size = 1024 * 1024 * 4,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_Auto,
};

constexpr SocketInitConfig SocketConfigApplet = {
    .tcp_tx_buf_size = 1024 * 32,
    .tcp_rx_buf_size = 1024 * 64,
    .tcp_tx_buf_max_size = 1024 * 256,
    .tcp_rx_buf_max_size = 1024 * 256,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 4,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_Auto,
};

/* Defaults are intentionally simple and should be replaced with controlled
 * harness endpoints when collecting targeted traces. */
constexpr char DnsHostname[] = "example.com";

constexpr char TcpHost[] = "example.com";
constexpr std::uint16_t TcpPort = 80;
constexpr char TcpPayload[] = "nxrv-requester-tcp";

constexpr char HttpHost[] = "example.com";
constexpr std::uint16_t HttpPort = 80;
constexpr char HttpPath[] = "/";

constexpr char HttpsHost[] = "example.com";
constexpr std::uint16_t HttpsPort = 443;
constexpr char HttpsPath[] = "/";

constexpr char UdpHost[] = "";
constexpr std::uint16_t UdpPort = 0;
constexpr char UdpPayload[] = "nxrv-requester-udp";

constexpr std::uint32_t SocketTimeoutMs = 5000;
constexpr std::uint32_t ScenarioStepDelayMs = 250;
constexpr std::uint32_t IdleSocketHoldMs = 1500;
constexpr std::uint32_t ConcurrentSocketHoldMs = 1000;
constexpr std::uint32_t ConcurrentSocketCount = 3;
constexpr std::uint32_t ExitDelayMs = 3000;
constexpr std::size_t ReadBufferSize = 1024;
constexpr std::size_t CurlReadBufferSize = 2048;

} // namespace requester::config
