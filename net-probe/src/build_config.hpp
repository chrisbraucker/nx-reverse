#pragma once

namespace wgnx::net_probe::build_config {

/*
 * Edit these booleans directly when assembling a probe build.
 *
 * Keep the full policy in one place so the compiled binary's intended MITM
 * surface is obvious from source review alone.
 */

inline constexpr bool EnableMitmNifmUser = false;
inline constexpr bool EnableMitmNifmSystem = false;
inline constexpr bool EnableMitmBsdUser = false;
inline constexpr bool EnableMitmBsdSystem = true;
inline constexpr bool EnableMitmBsdAdmin = false;
inline constexpr bool EnableMitmSslUser = false;
inline constexpr bool EnableMitmSslSystem = false;

/*
 * Historically fragile system clients stay denylisted by default. Flip an
 * individual override only when doing a targeted transparency investigation.
 */
inline constexpr bool AllowBsdSystemNpns = false;
inline constexpr bool AllowBsdSystemEupld = false;
inline constexpr bool AllowBsdSystemOlsc = false;
inline constexpr bool AllowBsdSystemBsdSockets = false;
inline constexpr bool AllowBsdSystemSsl = false;
inline constexpr bool AllowBsdSystemNim = false;
inline constexpr bool AllowBsdSystemSphairaWrapper = false;
inline constexpr bool AllowBsdSystemRequesterForwarder = true;
inline constexpr bool AllowBsdAdminQlaunch = false;
inline constexpr bool AllowSslNpns = false;
inline constexpr bool AllowSslEupld = false;
inline constexpr bool AllowSslOlsc = false;

/*
 * Diagnostic for BSD input-buffer crashes. When true, the Atmosphere-libs fork
 * reports a zero-size pointer buffer from QueryPointerBufferSize for requester
 * bsd:s MITM sessions only. Keep this false when testing whether the larger
 * MITM server stack is sufficient for normal pointer-buffer descriptors.
 */
inline constexpr bool AdvertiseZeroPointerBufferForRequesterBsd = false;

/*
 * Low-level bsd:s RegisterClient forwarding is controlled inside the
 * Atmosphere-libs fork by WGNX_BSD_REGISTER_CLIENT_FORWARD_MODE:
 *
 *   0 = Atmosphere-tagged forward
 *   1 = block with CMIF error
 *   2 = spoof downstream success only
 *   3 = spoof downstream success and register a probe-owned direct upstream
 *
 * Mode 0 matches Atmosphere's normal MITM PID passthrough protocol: the
 * forwarder adds the 0xFFFE tag and Mesosphere strips it while preserving the
 * original requester PID. The default uses this path for the raw requester
 * lifecycle ladder.
 * Override with a library rebuild:
 *   make clean-libs
 *   make EXTRA_DEFINES=-DWGNX_BSD_REGISTER_CLIENT_FORWARD_MODE=2
 */

enum class RequesterForwarderBsdMitmMode : unsigned int {
    None = 0,
    FirstOnly = 1,
    SecondOnly = 2,
    Both = 3,
};

/*
 * Temporary split-test for the installed requester HOME forwarder
 * (0x05720820ABC97000). libnx opens two bsd:s root sessions during
 * socketInitialize(): the first performs RegisterClient and the second performs
 * StartMonitoring. Use FirstOnly for RegisterClient diagnostics and SecondOnly
 * for the known-stable StartMonitoring side-session check.
 */
inline constexpr RequesterForwarderBsdMitmMode RequesterForwarderBsdMitm =
    RequesterForwarderBsdMitmMode::Both;

enum class BsdSendToMutationMode : unsigned int {
    Disabled = 0,
    ShadowCopy = 1,
    RewritePort = 2,
    /* Rewrite both the destination IPv4 address and port. */
    RewriteIpv4 = 3,
};

/*
 * Requester-only SendTo transformation ladder. ShadowCopy replaces the
 * forwarded destination send-static descriptor with probe-owned storage while
 * preserving every sockaddr byte. RewritePort changes only the destination
 * port in that copy after the original endpoint has matched exactly.
 * RewriteIpv4 changes both the destination address and port. Set the rewrite
 * address to the controlled host before selecting that mode.
 */
inline constexpr BsdSendToMutationMode RequesterBsdSendToMutation =
    BsdSendToMutationMode::RewriteIpv4;
inline constexpr u8 RequesterUdpEchoIpv4[4] = {10, 0, 0, 1};
inline constexpr u16 RequesterUdpEchoPort = 29000;
inline constexpr u8 RequesterUdpEchoRewriteIpv4[4] = {10, 0, 0, 2};
inline constexpr u16 RequesterUdpEchoRewritePort = 29001;

} // namespace wgnx::net_probe::build_config
