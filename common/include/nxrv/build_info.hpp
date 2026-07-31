#pragma once

#ifndef NXRV_VERSION
#define NXRV_VERSION "unknown"
#endif

#ifndef NXRV_BUILD_ID
#define NXRV_BUILD_ID "unknown"
#endif

namespace nxrv::build_info {

inline constexpr char Version[] = NXRV_VERSION;
inline constexpr char BuildId[] = NXRV_BUILD_ID;
inline constexpr char VersionWithBuild[] = NXRV_VERSION "-" NXRV_BUILD_ID;

} // namespace nxrv::build_info
