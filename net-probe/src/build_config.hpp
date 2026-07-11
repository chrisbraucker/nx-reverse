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
inline constexpr bool AllowBsdAdminQlaunch = false;
inline constexpr bool AllowSslNpns = false;
inline constexpr bool AllowSslEupld = false;
inline constexpr bool AllowSslOlsc = false;

} // namespace wgnx::net_probe::build_config
