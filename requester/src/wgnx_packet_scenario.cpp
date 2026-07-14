#include "wgnx_packet_scenario.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include <arpa/inet.h>
#include <switch.h>

#include "config.hpp"
#include "logger.hpp"
#include "wgnx/client.hpp"

namespace requester {

namespace {

constexpr std::size_t Ipv4HeaderSize = 20;
constexpr std::size_t UdpHeaderSize = 8;
constexpr std::size_t TokenSize = 16;
constexpr std::uint8_t UdpProtocol = 17;

using Ipv4Address = std::array<std::uint8_t, 4>;

struct ExpectedReply {
    Ipv4Address local_address{};
    Ipv4Address remote_address{};
    std::uint16_t local_port = 0;
    std::uint16_t remote_port = 0;
    std::span<const std::uint8_t> payload{};
};

void StoreBigEndian16(std::uint8_t *out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

std::uint16_t LoadBigEndian16(const std::uint8_t *in) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[0]) << 8) |
        static_cast<std::uint16_t>(in[1]));
}

std::uint32_t AddChecksumBytes(std::uint32_t sum, const std::uint8_t *data, std::size_t size) {
    std::size_t offset = 0;
    while (offset + 1 < size) {
        sum += (static_cast<std::uint32_t>(data[offset]) << 8) |
               static_cast<std::uint32_t>(data[offset + 1]);
        offset += 2;
    }
    if (offset < size) {
        sum += static_cast<std::uint32_t>(data[offset]) << 8;
    }
    return sum;
}

std::uint16_t FinishChecksum(std::uint32_t sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum & 0xFFFFU);
}

std::uint16_t ComputeChecksum(const std::uint8_t *data, std::size_t size) {
    return FinishChecksum(AddChecksumBytes(0, data, size));
}

std::uint16_t ComputeUdpChecksum(
    const Ipv4Address& source,
    const Ipv4Address& destination,
    const std::uint8_t *udp,
    std::size_t udp_size) {
    std::uint32_t sum = AddChecksumBytes(0, source.data(), source.size());
    sum = AddChecksumBytes(sum, destination.data(), destination.size());
    const std::array<std::uint8_t, 4> pseudo_tail = {
        0,
        UdpProtocol,
        static_cast<std::uint8_t>(udp_size >> 8),
        static_cast<std::uint8_t>(udp_size & 0xFFU),
    };
    sum = AddChecksumBytes(sum, pseudo_tail.data(), pseudo_tail.size());
    sum = AddChecksumBytes(sum, udp, udp_size);
    return FinishChecksum(sum);
}

bool ParseIpv4(const char *text, Ipv4Address& out) {
    return text != nullptr && inet_pton(AF_INET, text, out.data()) == 1;
}

std::string FormatIpv4(const Ipv4Address& address) {
    char text[16]{};
    if (inet_ntop(AF_INET, address.data(), text, sizeof(text)) == nullptr) {
        return "<invalid>";
    }
    return text;
}

const char *PacketStatusName(std::uint32_t raw_status) {
    return wgnx::GetPacketApiStatusName(static_cast<wgnx::PacketApiStatus>(raw_status));
}

std::size_t BuildUdpPacket(
    std::span<std::uint8_t> output,
    const Ipv4Address& source,
    const Ipv4Address& destination,
    std::uint16_t source_port,
    std::uint16_t destination_port,
    std::span<const std::uint8_t> payload,
    std::uint16_t ipv4_identification) {
    const std::size_t udp_size = UdpHeaderSize + payload.size();
    const std::size_t packet_size = Ipv4HeaderSize + udp_size;
    if (packet_size > output.size() || packet_size > wgnx::MaxInnerIpv4PacketSize ||
        packet_size > UINT16_MAX) {
        return 0;
    }

    output = output.first(packet_size);
    std::fill(output.begin(), output.end(), std::uint8_t{0});
    output[0] = 0x45;
    StoreBigEndian16(output.data() + 2, static_cast<std::uint16_t>(packet_size));
    StoreBigEndian16(output.data() + 4, ipv4_identification);
    StoreBigEndian16(output.data() + 6, 0x4000U);
    output[8] = 64;
    output[9] = UdpProtocol;
    std::memcpy(output.data() + 12, source.data(), source.size());
    std::memcpy(output.data() + 16, destination.data(), destination.size());
    StoreBigEndian16(output.data() + 10, ComputeChecksum(output.data(), Ipv4HeaderSize));

    std::uint8_t *udp = output.data() + Ipv4HeaderSize;
    StoreBigEndian16(udp, source_port);
    StoreBigEndian16(udp + 2, destination_port);
    StoreBigEndian16(udp + 4, static_cast<std::uint16_t>(udp_size));
    std::memcpy(udp + UdpHeaderSize, payload.data(), payload.size());
    std::uint16_t udp_checksum = ComputeUdpChecksum(source, destination, udp, udp_size);
    if (udp_checksum == 0) {
        udp_checksum = 0xFFFFU;
    }
    StoreBigEndian16(udp + 6, udp_checksum);
    return packet_size;
}

bool ValidateUdpReply(
    std::span<const std::uint8_t> packet,
    const ExpectedReply& expected,
    std::string& detail) {
    if (packet.size() < Ipv4HeaderSize + UdpHeaderSize) {
        detail = "packet too short";
        return false;
    }

    const std::uint8_t version = packet[0] >> 4;
    const std::size_t ipv4_header_size = static_cast<std::size_t>(packet[0] & 0x0FU) * 4;
    if (version != 4 || ipv4_header_size < Ipv4HeaderSize || ipv4_header_size > packet.size()) {
        detail = "invalid IPv4 version or header length";
        return false;
    }
    const std::size_t total_size = LoadBigEndian16(packet.data() + 2);
    if (total_size != packet.size() || total_size < ipv4_header_size + UdpHeaderSize) {
        detail = "invalid IPv4 total length";
        return false;
    }
    if (ComputeChecksum(packet.data(), ipv4_header_size) != 0) {
        detail = "invalid IPv4 checksum";
        return false;
    }
    if ((LoadBigEndian16(packet.data() + 6) & 0x3FFFU) != 0) {
        detail = "fragmented IPv4 reply unsupported";
        return false;
    }
    if (packet[9] != UdpProtocol) {
        detail = "not UDP protocol=" + std::to_string(packet[9]);
        return false;
    }

    Ipv4Address source{};
    Ipv4Address destination{};
    std::memcpy(source.data(), packet.data() + 12, source.size());
    std::memcpy(destination.data(), packet.data() + 16, destination.size());
    if (source != expected.remote_address || destination != expected.local_address) {
        detail = "address mismatch source=" + FormatIpv4(source) +
            " destination=" + FormatIpv4(destination);
        return false;
    }

    const std::uint8_t *udp = packet.data() + ipv4_header_size;
    const std::size_t udp_size = LoadBigEndian16(udp + 4);
    if (udp_size != total_size - ipv4_header_size || udp_size < UdpHeaderSize) {
        detail = "invalid UDP length";
        return false;
    }
    const std::uint16_t source_port = LoadBigEndian16(udp);
    const std::uint16_t destination_port = LoadBigEndian16(udp + 2);
    if (source_port != expected.remote_port || destination_port != expected.local_port) {
        detail = "port mismatch source=" + std::to_string(source_port) +
            " destination=" + std::to_string(destination_port);
        return false;
    }
    if (LoadBigEndian16(udp + 6) == 0) {
        detail = "missing UDP checksum";
        return false;
    }
    if (ComputeUdpChecksum(source, destination, udp, udp_size) != 0) {
        detail = "invalid UDP checksum";
        return false;
    }

    const std::span<const std::uint8_t> payload(udp + UdpHeaderSize, udp_size - UdpHeaderSize);
    if (payload.size() != expected.payload.size() ||
        !std::equal(payload.begin(), payload.end(), expected.payload.begin())) {
        detail = "payload mismatch preview=" + EscapePreview(payload.data(), payload.size(), 96);
        return false;
    }

    detail = "validated UDP echo";
    return true;
}

std::uint64_t DeadlineAfterMilliseconds(std::uint32_t timeout_ms) {
    const std::uint64_t ticks =
        (armGetSystemTickFreq() * static_cast<std::uint64_t>(timeout_ms)) / 1000U;
    return armGetSystemTick() + ticks;
}

} // namespace

ScenarioResult RunWgnxPacketUdpEcho(AppContext& ctx) {
    ScenarioResult result{.name = "wgnx_packet_udp_echo"};
    logger::Status(
        ctx,
        "Running wgnx:ctl UDP echo %s:%u -> %s:%u",
        config::WgnxTunnelSourceIpv4,
        config::WgnxUdpSourcePort,
        config::WgnxUdpEchoDestinationIpv4,
        config::WgnxUdpEchoDestinationPort);

    if (!wgnx::client::IsServiceRunning()) {
        result.detail = "wgnx:ctl is not running";
        return result;
    }

    std::uint32_t api_version = 0;
    Result rc = wgnx::client::GetApiVersion(&api_version);
    if (R_FAILED(rc)) {
        result.rc = rc;
        result.detail = "GetApiVersion CMIF failure";
        return result;
    }
    if (api_version != wgnx::IpcApiVersion) {
        result.detail = "API version mismatch compiled=" +
            std::to_string(wgnx::IpcApiVersion) +
            " actual=" + std::to_string(api_version);
        return result;
    }

    Ipv4Address local_address{};
    Ipv4Address remote_address{};
    if (!ParseIpv4(config::WgnxTunnelSourceIpv4, local_address) ||
        !ParseIpv4(config::WgnxUdpEchoDestinationIpv4, remote_address)) {
        result.detail = "invalid configured IPv4 address";
        return result;
    }

    constexpr std::size_t PrefixSize = sizeof(config::WgnxUdpPayloadPrefix) - 1;
    std::array<std::uint8_t, PrefixSize + TokenSize> payload{};
    std::memcpy(payload.data(), config::WgnxUdpPayloadPrefix, PrefixSize);
    randomGet(payload.data() + PrefixSize, TokenSize);

    std::array<std::uint8_t, wgnx::MaxInnerIpv4PacketSize> outgoing{};
    const std::uint64_t random_value = randomGet64();
    const std::size_t outgoing_size = BuildUdpPacket(
        outgoing,
        local_address,
        remote_address,
        config::WgnxUdpSourcePort,
        config::WgnxUdpEchoDestinationPort,
        payload,
        static_cast<std::uint16_t>(random_value));
    if (outgoing_size == 0) {
        result.detail = "failed to build inner IPv4/UDP packet";
        return result;
    }
    if (config::WgnxSubmitMalformedIpv4Checksum) {
        outgoing[10] ^= 0xFFU;
    }

    wgnx::PacketSubmissionResult submission{};
    rc = wgnx::client::SubmitInnerIpv4Packet(outgoing.data(), outgoing_size, &submission);
    result.rc = rc;
    logger::Log(
        ctx,
        "scenario=wgnx_packet_udp_echo submit rc=%s status=%s(%u) id=%llu bytes=%u peer=%d activation=%u token=%s",
        FormatResult(rc).c_str(),
        PacketStatusName(submission.status),
        submission.status,
        static_cast<unsigned long long>(submission.packet_id),
        submission.packet_size,
        submission.peer_index,
        submission.activation_generation,
        EscapePreview(payload.data() + PrefixSize, TokenSize, TokenSize).c_str());
    if (R_FAILED(rc)) {
        result.detail = "SubmitInnerIpv4Packet CMIF failure";
        return result;
    }
    if (config::WgnxSubmitMalformedIpv4Checksum) {
        result.success = submission.status ==
            static_cast<std::uint32_t>(wgnx::PacketApiStatus::MalformedPacket);
        result.detail = "malformed IPv4 checksum submission status=" +
            std::string(PacketStatusName(submission.status));
        return result;
    }
    if (submission.status != static_cast<std::uint32_t>(wgnx::PacketApiStatus::Queued)) {
        result.detail = "submission rejected status=" +
            std::string(PacketStatusName(submission.status)) +
            " peer=" + std::to_string(submission.peer_index) +
            " activation=" + std::to_string(submission.activation_generation);
        return result;
    }
    result.bytes_sent = outgoing_size;

    const ExpectedReply expected = {
        .local_address = local_address,
        .remote_address = remote_address,
        .local_port = config::WgnxUdpSourcePort,
        .remote_port = config::WgnxUdpEchoDestinationPort,
        .payload = payload,
    };
    const std::uint64_t deadline = DeadlineAfterMilliseconds(config::WgnxPacketTimeoutMs);
    std::array<std::uint8_t, wgnx::MaxInnerIpv4PacketSize> incoming{};
    std::uint32_t empty_polls = 0;
    std::uint32_t rejected_packets = 0;
    std::string last_rejection;

    while (armGetSystemTick() < deadline) {
        wgnx::PacketReceiveResult received{};
        rc = wgnx::client::ReceiveInnerIpv4Packet(incoming.data(), incoming.size(), &received);
        result.rc = rc;
        if (R_FAILED(rc)) {
            result.detail = "ReceiveInnerIpv4Packet CMIF failure after empty_polls=" +
                std::to_string(empty_polls);
            return result;
        }

        const auto status = static_cast<wgnx::PacketApiStatus>(received.status);
        if (status == wgnx::PacketApiStatus::QueueEmpty) {
            ++empty_polls;
            SleepMilliseconds(config::WgnxPacketPollIntervalMs);
            continue;
        }
        logger::Log(
            ctx,
            "scenario=wgnx_packet_udp_echo receive status=%s(%u) id=%llu bytes=%u peer=%d activation=%u",
            PacketStatusName(received.status),
            received.status,
            static_cast<unsigned long long>(received.packet_id),
            received.packet_size,
            received.peer_index,
            received.activation_generation);
        if (status != wgnx::PacketApiStatus::Success) {
            result.detail = "receive rejected status=" +
                std::string(PacketStatusName(received.status)) +
                " empty_polls=" + std::to_string(empty_polls);
            return result;
        }
        if (received.packet_size > incoming.size()) {
            result.detail = "receive reported oversized packet=" + std::to_string(received.packet_size);
            return result;
        }

        std::string validation_detail;
        const std::span<const std::uint8_t> packet(incoming.data(), received.packet_size);
        if (ValidateUdpReply(packet, expected, validation_detail)) {
            result.success = true;
            result.bytes_received = received.packet_size;
            result.detail =
                "api=" + std::to_string(api_version) +
                " submit_id=" + std::to_string(submission.packet_id) +
                " receive_id=" + std::to_string(received.packet_id) +
                " peer=" + std::to_string(received.peer_index) +
                " activation=" + std::to_string(received.activation_generation) +
                " empty_polls=" + std::to_string(empty_polls) +
                " ignored_packets=" + std::to_string(rejected_packets) +
                " payload=" + EscapePreview(payload.data(), payload.size(), payload.size());
            return result;
        }

        ++rejected_packets;
        last_rejection = validation_detail;
        logger::Log(
            ctx,
            "scenario=wgnx_packet_udp_echo ignored_receive reason=%s preview=%s",
            validation_detail.c_str(),
            EscapePreview(packet.data(), packet.size(), 96).c_str());
    }

    result.detail =
        "timeout_ms=" + std::to_string(config::WgnxPacketTimeoutMs) +
        " empty_polls=" + std::to_string(empty_polls) +
        " ignored_packets=" + std::to_string(rejected_packets);
    if (!last_rejection.empty()) {
        result.detail += " last_rejection=" + last_rejection;
    }
    return result;
}

} // namespace requester
