#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

#include <curl/curl.h>
#include <switch.h>

namespace requester {

struct EnvironmentSnapshot {
    Result internet_status_rc = 0;
    NifmInternetConnectionType connection_type = static_cast<NifmInternetConnectionType>(0);
    std::uint32_t wifi_strength = 0;
    NifmInternetConnectionStatus connection_status = static_cast<NifmInternetConnectionStatus>(0);

    Result current_ip_rc = 0;
    std::uint32_t current_ip = 0;

    Result ip_config_rc = 0;
    std::uint32_t subnet_mask = 0;
    std::uint32_t gateway = 0;
    std::uint32_t primary_dns = 0;
    std::uint32_t secondary_dns = 0;
};

struct AppContext {
    FILE *log_file = nullptr;
    std::string run_id;
    std::string log_path;
    EnvironmentSnapshot env{};
    Result socket_initialize_rc = 0;
    bool applet_exit_locked = false;
    Result ssl_initialize_rc = 0;
    bool ssl_initialized = false;
    CURLSH *curl_share = nullptr;
    CURLcode curl_global_rc = CURLE_OK;
    bool curl_initialized = false;
};

std::string MakeRunId(void);
std::string MakeLogPath(const std::string& run_id);
bool EnsureLogDirectories(void);
std::string TimestampUtc(void);
std::string MonotonicTimestampNs(void);
std::string FormatIpv4(std::uint32_t addr);
std::string FormatResult(Result rc);
std::string FormatErrno(int value);
std::string FormatHosVersion(void);
std::string FormatInternetConnectionType(NifmInternetConnectionType type);
std::string FormatInternetConnectionStatus(NifmInternetConnectionStatus status);
std::string EscapePreview(const void *data, std::size_t size, std::size_t limit);
void SleepMilliseconds(std::uint32_t milliseconds);

} // namespace requester
