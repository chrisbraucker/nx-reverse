#include "runtime.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

#include <sys/stat.h>

#include "config.hpp"

namespace requester {

namespace {

std::string FormatTimestamp(const struct tm& tm_value, const char *format) {
    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), format, &tm_value) == 0) {
        return "unknown";
    }
    return buffer;
}

bool EnsureDirectory(const char *path) {
    if (mkdir(path, 0777) == 0) {
        return true;
    }
    return errno == EEXIST;
}

} // namespace

std::string MakeRunId(void) {
    const std::time_t now = std::time(nullptr);
    struct tm tm_value {};
    gmtime_r(&now, &tm_value);
    return FormatTimestamp(tm_value, "%Y-%m-%dT%H-%M-%SZ");
}

std::string MakeLogPath(const std::string& run_id) {
    std::string path = config::LogDirectory;
    path += "/requester-";
    path += run_id;
    path += ".log";
    return path;
}

bool EnsureLogDirectories(void) {
    return EnsureDirectory("sdmc:/nxrv") && EnsureDirectory(config::LogDirectory);
}

std::string TimestampUtc(void) {
    const std::time_t now = std::time(nullptr);
    struct tm tm_value {};
    gmtime_r(&now, &tm_value);
    return FormatTimestamp(tm_value, "%Y-%m-%dT%H:%M:%SZ");
}

std::string MonotonicTimestampNs(void) {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    char buffer[64];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%llu",
        static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
            static_cast<unsigned long long>(ts.tv_nsec));
    return buffer;
}

std::string FormatIpv4(std::uint32_t addr) {
    char buffer[32];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u.%u.%u.%u",
        addr & 0xFFU,
        (addr >> 8) & 0xFFU,
        (addr >> 16) & 0xFFU,
        (addr >> 24) & 0xFFU);
    return buffer;
}

std::string FormatResult(Result rc) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", rc);
    return buffer;
}

std::string FormatErrno(int value) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%d (%s)", value, std::strerror(value));
    return buffer;
}

std::string FormatHosVersion(void) {
    const std::uint32_t hos = hosversionGet();
    char buffer[48];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%u.%u.%u%s",
        (hos >> 16) & 0xFFU,
        (hos >> 8) & 0xFFU,
        hos & 0xFFU,
        hosversionIsAtmosphere() ? "|AMS" : "");
    return buffer;
}

std::string FormatInternetConnectionType(NifmInternetConnectionType type) {
    switch (type) {
        case NifmInternetConnectionType_WiFi:
            return "wifi";
        case NifmInternetConnectionType_Ethernet:
            return "ethernet";
        default:
            return "unknown";
    }
}

std::string FormatInternetConnectionStatus(NifmInternetConnectionStatus status) {
    switch (status) {
        case NifmInternetConnectionStatus_ConnectingUnknown1:
            return "connecting-0";
        case NifmInternetConnectionStatus_ConnectingUnknown2:
            return "connecting-1";
        case NifmInternetConnectionStatus_ConnectingUnknown3:
            return "connecting-2";
        case NifmInternetConnectionStatus_ConnectingUnknown4:
            return "connecting-3";
        case NifmInternetConnectionStatus_Connected:
            return "connected";
        default:
            return "unknown";
    }
}

std::string EscapePreview(const void *data, std::size_t size, std::size_t limit) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::ostringstream oss;

    const std::size_t count = size < limit ? size : limit;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char ch = bytes[i];
        switch (ch) {
            case '\r':
                oss << "\\r";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\t':
                oss << "\\t";
                break;
            case '\\':
                oss << "\\\\";
                break;
            default:
                if (ch >= 0x20 && ch <= 0x7E) {
                    oss << static_cast<char>(ch);
                } else {
                    char buffer[5];
                    std::snprintf(buffer, sizeof(buffer), "\\x%02X", ch);
                    oss << buffer;
                }
                break;
        }
    }

    if (size > limit) {
        oss << "...";
    }

    return oss.str();
}

void SleepMilliseconds(std::uint32_t milliseconds) {
    svcSleepThread(static_cast<std::int64_t>(milliseconds) * 1000000LL);
}

} // namespace requester
