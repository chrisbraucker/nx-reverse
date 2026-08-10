#pragma once

#include <switch.h>

namespace toolbox::config {

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

constexpr SocketInitConfig SocketConfigBsdSystemMitm = {
    .tcp_tx_buf_size = 1024 * 64,
    .tcp_rx_buf_size = 1024 * 64,
    .tcp_tx_buf_max_size = 1024 * 1024 * 4,
    .tcp_rx_buf_max_size = 1024 * 1024 * 4,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 1,
    .bsd_service_type = BsdServiceType_System,
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

} // namespace toolbox::config
