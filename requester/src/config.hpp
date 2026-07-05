#pragma once

#include <cstddef>
#include <cstdint>

namespace requester::config {

constexpr char LogDirectory[] = "sdmc:/nxrv/requester";

/* Defaults are intentionally simple and should be replaced with controlled
 * harness endpoints when collecting targeted traces. */
constexpr char DnsHostname[] = "example.com";

constexpr char TcpHost[] = "example.com";
constexpr std::uint16_t TcpPort = 80;

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
constexpr std::uint32_t ExitDelayMs = 3000;
constexpr std::size_t ReadBufferSize = 1024;

} // namespace requester::config
