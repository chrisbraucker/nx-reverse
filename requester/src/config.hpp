#pragma once

#include <cstddef>
#include <cstdint>

namespace requester::config {

constexpr char LogDirectory[] = "sdmc:/nxrv/requester";

constexpr bool EnableAppletExitLock = false;
constexpr bool EnableSocketInitialize = false;
constexpr bool EnableNifmInitialize = false;
constexpr bool EnableSslInitialize = false;
constexpr bool EnableCurlInitialize = false;
constexpr bool EnableCurlShare = false;

/*
 * Raw single-session bsd:s lifecycle matching libnx initialization: open the
 * root and monitor services, register the transfer memory, then associate the
 * monitor session with the returned client ID. A single session does not need
 * a cloned root session.
 */
constexpr bool EnableScenarioManualBsdLifecycle = false;
constexpr bool ManualBsdOpenMonitorSession = true;
constexpr bool ManualBsdCreateTransferMemory = true;
constexpr bool ManualBsdRegisterClient = true;
constexpr bool ManualBsdStartMonitoring = true;
constexpr bool ManualBsdCloneRootSession = false;

static_assert(!EnableScenarioManualBsdLifecycle || !EnableSocketInitialize);
static_assert(!ManualBsdRegisterClient || ManualBsdCreateTransferMemory);
static_assert(!ManualBsdStartMonitoring || ManualBsdRegisterClient);
static_assert(!ManualBsdStartMonitoring || ManualBsdOpenMonitorSession);
static_assert(!ManualBsdCloneRootSession || ManualBsdRegisterClient);

/* Exercise outbound and inbound UDP without socket options. */
constexpr bool EnableScenarioEnvironmentSnapshot = false;
constexpr bool EnableScenarioDnsResolve = false;
constexpr bool EnableScenarioPlainTcpConnect = false;
constexpr bool EnableScenarioIdleTcpHold = false;
constexpr bool EnableScenarioHttpGet = false;
constexpr bool EnableScenarioHttpsGet = false;
constexpr bool EnableScenarioCurlHttpGet = false;
constexpr bool EnableScenarioCurlHttpsGet = false;
constexpr bool EnableScenarioUdpSocketOnly = false;
constexpr bool EnableScenarioUdpSocketSetSockOpt = false;
constexpr bool EnableScenarioUdpSetSockOptReuseAddr = false;
constexpr bool EnableScenarioUdpSetSockOptRecvTimeout = false;
constexpr bool EnableScenarioUdpSetSockOptSendTimeout = false;
constexpr bool EnableScenarioUdpSendToOnly = false;
constexpr bool EnableUdpSendToOnlyTimeouts = false;
constexpr bool EnableScenarioUdpConnectSendOnly = false;
constexpr bool EnableScenarioUdpEcho = false;
constexpr bool EnableUdpEchoSocketTimeouts = false;
constexpr bool EnableUdpEchoPoll = true;
constexpr bool EnableScenarioConcurrentTcpBurst = false;
constexpr bool EnableScenarioWgnxUdpEcho = true;

/* Direct inner-packet test through the experimental wgnx:ctl API. */
constexpr char WgnxTunnelSourceIpv4[] = "10.0.0.2";
constexpr char WgnxUdpEchoDestinationIpv4[] = "10.1.0.2";
constexpr std::uint16_t WgnxUdpSourcePort = 39000;
constexpr std::uint16_t WgnxUdpEchoDestinationPort = 29000;
constexpr char WgnxUdpPayloadPrefix[] = "nxrv-wgnx-udp:";
constexpr std::uint32_t WgnxPacketTimeoutMs = 5000;
constexpr std::uint32_t WgnxPacketPollIntervalMs = 25;
constexpr bool WgnxSubmitMalformedIpv4Checksum = false;

constexpr SocketInitConfig SocketConfigApplication = {
    .tcp_tx_buf_size = 1024 * 64,
    .tcp_rx_buf_size = 1024 * 64,
    .tcp_tx_buf_max_size = 1024 * 1024 * 4,
    .tcp_rx_buf_max_size = 1024 * 1024 * 4,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 1,
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
    .num_bsd_sessions = 1,
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
constexpr std::uint32_t ExitDelayMs = 5000;
constexpr std::size_t ReadBufferSize = 1024;
constexpr std::size_t CurlReadBufferSize = 2048;

} // namespace requester::config
