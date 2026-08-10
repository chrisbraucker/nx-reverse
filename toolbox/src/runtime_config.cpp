#include "runtime_config.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include <sys/stat.h>

#include <switch.h>

#include "config.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace toolbox {

namespace {

constexpr std::size_t MinimumPayloadBytes = 24;
constexpr std::size_t MaximumProfiles = 8;
constexpr std::size_t MaximumProfileNameLength = 32;
constexpr std::uint32_t MaximumDatagramCount = 4096;
constexpr std::uint32_t MaximumDurationMs = 60000;

std::string Trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool ParseBool(std::string_view text, bool* value) {
    if (text == "true" || text == "1") {
        *value = true;
        return true;
    }
    if (text == "false" || text == "0") {
        *value = false;
        return true;
    }
    return false;
}

template <typename T> bool ParseUnsigned(std::string_view text, T* value) {
    static_assert(std::numeric_limits<T>::is_integer);
    T parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    *value = parsed;
    return true;
}

bool IsValidIpv4(std::string_view text) {
    std::array<unsigned int, 4> octets{};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < octets.size(); ++index) {
        const std::size_t separator = text.find('.', offset);
        const bool final_component = index + 1 == octets.size();
        if ((final_component && separator != std::string_view::npos) || (!final_component && separator == std::string_view::npos)) {
            return false;
        }
        const std::size_t end = final_component ? text.size() : separator;
        if (end == offset || end - offset > 3) {
            return false;
        }
        unsigned int octet{};
        if (!ParseUnsigned(text.substr(offset, end - offset), &octet) || octet > 255) {
            return false;
        }
        octets[index] = octet;
        offset = end + 1;
    }
    return offset == text.size() + 1;
}

bool EnsureDirectory(const char* path) {
    return mkdir(path, 0777) == 0 || errno == EEXIST;
}

bool ParseBsdSystemUdpExpectedOutcome(std::string_view value, BsdSystemUdpExpectedOutcome* expected_outcome) {
    if (value == "echo") {
        *expected_outcome = BsdSystemUdpExpectedOutcome::EchoReply;
        return true;
    }
    if (value == "no_reply_timeout") {
        *expected_outcome = BsdSystemUdpExpectedOutcome::NoReplyTimeout;
        return true;
    }
    if (value == "terminal_closure") {
        *expected_outcome = BsdSystemUdpExpectedOutcome::TerminalClosure;
        return true;
    }
    return false;
}

bool ParseRuntimeScenario(std::string_view value, RuntimeScenario* scenario) {
    if (value == "direct_tunnel_udp") {
        *scenario = RuntimeScenario::DirectTunnelUdp;
        return true;
    }
    if (value == "bsd_system_udp") {
        *scenario = RuntimeScenario::BsdSystemUdp;
        return true;
    }
    if (value == "bsd_system_tcp") {
        *scenario = RuntimeScenario::BsdSystemTcp;
        return true;
    }
    if (value == "tunnel_contract_validation") {
        *scenario = RuntimeScenario::TunnelContractValidation;
        return true;
    }
    return false;
}

bool ParseProfileKey(std::string_view key, std::size_t* index, std::string_view* field) {
    constexpr std::string_view Prefix = "profile.";
    if (!key.starts_with(Prefix)) {
        return false;
    }
    const std::string_view remainder = key.substr(Prefix.size());
    const std::size_t separator = remainder.find('.');
    if (separator == std::string_view::npos || !ParseUnsigned(remainder.substr(0, separator), index)) {
        return false;
    }
    *field = remainder.substr(separator + 1);
    return true;
}

bool ApplyLegacySetting(RuntimeConfig* config, std::string_view key, std::string_view value) {
    RuntimeProfile* profile = ActiveRuntimeProfile(config);
    if (profile == nullptr) {
        return false;
    }
    if (key == "tunnel_udp.enabled") {
        bool enabled{};
        if (!ParseBool(value, &enabled)) {
            return false;
        }
        if (enabled) {
            config->scenario = RuntimeScenario::DirectTunnelUdp;
        }
        return true;
    }
    if (key == "bsd_system_udp.enabled") {
        bool enabled{};
        if (!ParseBool(value, &enabled)) {
            return false;
        }
        if (enabled) {
            config->scenario = RuntimeScenario::BsdSystemUdp;
        }
        return true;
    }
    if (key == "tunnel_contract.enabled") {
        bool enabled{};
        if (!ParseBool(value, &enabled)) {
            return false;
        }
        if (enabled) {
            config->scenario = RuntimeScenario::TunnelContractValidation;
        }
        return true;
    }
    if (key == "tunnel_udp.destination_ipv4") {
        if (!IsValidIpv4(value)) {
            return false;
        }
        profile->tunnel_destination_ipv4 = value;
        profile->bsd_destination_ipv4 = value;
        return true;
    }
    if (key == "tunnel_udp.destination_port") {
        return ParseUnsigned(value, &profile->udp_destination_port) && profile->udp_destination_port != 0;
    }
    if (key == "tunnel_udp.workload_id") {
        return ParseUnsigned(value, &config->next_workload_id);
    }
    if (key == "tunnel_udp.payload_bytes") {
        return ParseUnsigned(value, &config->udp.payload_bytes);
    }
    if (key == "tunnel_udp.datagram_count") {
        return ParseUnsigned(value, &config->udp.datagram_count);
    }
    if (key == "tunnel_udp.pacing_ms") {
        return ParseUnsigned(value, &config->udp.pacing_ms);
    }
    if (key == "tunnel_udp.concurrent_flows") {
        return ParseUnsigned(value, &config->udp.concurrent_flows);
    }
    if (key == "tunnel_udp.receive_deadline_ms") {
        return ParseUnsigned(value, &config->udp.receive_deadline_ms);
    }
    if (key == "tunnel_udp.payload_seed") {
        return ParseUnsigned(value, &config->udp.payload_seed);
    }
    if (key == "tunnel_udp.echo_replies") {
        return ParseBool(value, &config->udp.echo_replies);
    }
    if (key == "tunnel_contract.verify_cloned_session_lifetime") {
        return ParseBool(value, &config->tunnel_contract.verify_cloned_session_lifetime);
    }
    if (key == "tunnel_contract.verify_mixed_batch") {
        return ParseBool(value, &config->tunnel_contract.verify_mixed_batch);
    }
    if (key == "bsd_system_udp.verify_post_route_rejection") {
        return ParseBool(value, &config->bsd_system_udp.verify_post_route_rejection);
    }
    if (key == "bsd_system_udp.expected_outcome") {
        return ParseBsdSystemUdpExpectedOutcome(value, &config->bsd_system_udp.expected_outcome);
    }
    if (key == "bsd_system_udp.require_writable_recovery") {
        return ParseBool(value, &config->bsd_system_udp.require_writable_recovery);
    }
    return false;
}

bool ApplySetting(RuntimeConfig* config, std::string_view key, std::string_view value, bool* migrated_legacy, std::string* error) {
    auto invalid = [&] {
        *error = "invalid value for " + std::string(key);
        return false;
    };

    if (key == "run.scenario") {
        return ParseRuntimeScenario(value, &config->scenario) || invalid();
    }
    if (key == "run.next_workload_id") {
        return ParseUnsigned(value, &config->next_workload_id) || invalid();
    }
    if (key == "run.active_profile") {
        return ParseUnsigned(value, &config->active_profile) || invalid();
    }
    if (key == "profiles.count") {
        std::size_t count{};
        if (!ParseUnsigned(value, &count) || count == 0 || count > MaximumProfiles) {
            return invalid();
        }
        const RuntimeProfile default_profile = config->profiles.front();
        config->profiles.resize(count, default_profile);
        return true;
    }
    std::size_t profile_index{};
    std::string_view profile_field;
    if (ParseProfileKey(key, &profile_index, &profile_field)) {
        if (profile_index >= config->profiles.size()) {
            return invalid();
        }
        RuntimeProfile& profile = config->profiles[profile_index];
        if (profile_field == "name") {
            if (value.size() > MaximumProfileNameLength) {
                return invalid();
            }
            profile.name = value;
            return true;
        }
        if (profile_field == "tunnel_destination_ipv4") {
            if (!IsValidIpv4(value)) {
                return invalid();
            }
            profile.tunnel_destination_ipv4 = value;
            return true;
        }
        if (profile_field == "bsd_destination_ipv4") {
            if (!IsValidIpv4(value)) {
                return invalid();
            }
            profile.bsd_destination_ipv4 = value;
            return true;
        }
        if (profile_field == "udp_destination_port") {
            return (ParseUnsigned(value, &profile.udp_destination_port) && profile.udp_destination_port != 0) || invalid();
        }
        if (profile_field == "tcp_destination_port") {
            return (ParseUnsigned(value, &profile.tcp_destination_port) && profile.tcp_destination_port != 0) || invalid();
        }
        *error = "unrecognized profile setting " + std::string(key);
        return false;
    }
    if (key == "udp.payload_bytes") {
        return ParseUnsigned(value, &config->udp.payload_bytes) || invalid();
    }
    if (key == "udp.datagram_count") {
        return ParseUnsigned(value, &config->udp.datagram_count) || invalid();
    }
    if (key == "udp.pacing_ms") {
        return ParseUnsigned(value, &config->udp.pacing_ms) || invalid();
    }
    if (key == "udp.concurrent_flows") {
        return ParseUnsigned(value, &config->udp.concurrent_flows) || invalid();
    }
    if (key == "udp.receive_deadline_ms") {
        return ParseUnsigned(value, &config->udp.receive_deadline_ms) || invalid();
    }
    if (key == "udp.payload_seed") {
        return ParseUnsigned(value, &config->udp.payload_seed) || invalid();
    }
    if (key == "udp.echo_replies") {
        return ParseBool(value, &config->udp.echo_replies) || invalid();
    }
    if (key == "tcp.receive_deadline_ms") {
        return ParseUnsigned(value, &config->tcp.receive_deadline_ms) || invalid();
    }
    if (key == "tunnel_contract.verify_cloned_session_lifetime") {
        return ParseBool(value, &config->tunnel_contract.verify_cloned_session_lifetime) || invalid();
    }
    if (key == "tunnel_contract.verify_mixed_batch") {
        return ParseBool(value, &config->tunnel_contract.verify_mixed_batch) || invalid();
    }
    if (key == "bsd_system_udp.verify_post_route_rejection") {
        return ParseBool(value, &config->bsd_system_udp.verify_post_route_rejection) || invalid();
    }
    if (key == "bsd_system_udp.expected_outcome") {
        return ParseBsdSystemUdpExpectedOutcome(value, &config->bsd_system_udp.expected_outcome) || invalid();
    }
    if (key == "bsd_system_udp.require_writable_recovery") {
        return ParseBool(value, &config->bsd_system_udp.require_writable_recovery) || invalid();
    }
    if (ApplyLegacySetting(config, key, value)) {
        *migrated_legacy = true;
        return true;
    }
    *error = "unrecognized configuration key " + std::string(key);
    return false;
}

} // namespace

RuntimeConfig CompiledRuntimeDefaults() {
    BsdSystemUdpExpectedOutcome bsd_expected_outcome{};
    static_cast<void>(ParseBsdSystemUdpExpectedOutcome(config::BsdSystemUdpExpectedOutcome, &bsd_expected_outcome));
    RuntimeScenario scenario = RuntimeScenario::DirectTunnelUdp;
    if (config::EnableScenarioBsdSystemUdpWorkload) {
        scenario = RuntimeScenario::BsdSystemUdp;
    } else if (config::EnableScenarioWgnxTunnelContractValidation) {
        scenario = RuntimeScenario::TunnelContractValidation;
    }
    return {
        .scenario = scenario,
        .next_workload_id = config::WgnxTunnelWorkloadId,
        .active_profile = 0,
        .profiles = {{
            .name = "Default",
            .tunnel_destination_ipv4 = config::WgnxTunnelDestinationIpv4,
            .bsd_destination_ipv4 = config::WgnxTunnelDestinationIpv4,
            .udp_destination_port = config::WgnxTunnelDestinationPort,
            .tcp_destination_port = config::BsdSystemTcpDestinationPort,
        }},
        .udp =
            {
                .payload_bytes = config::WgnxTunnelPayloadBytes,
                .datagram_count = config::WgnxTunnelDatagramCount,
                .pacing_ms = config::WgnxTunnelPacingMs,
                .concurrent_flows = config::WgnxTunnelConcurrentFlows,
                .receive_deadline_ms = config::WgnxTunnelReceiveDeadlineMs,
                .payload_seed = config::WgnxTunnelPayloadSeed,
                .echo_replies = config::WgnxTunnelEchoReplies,
            },
        .tcp = {.receive_deadline_ms = config::BsdSystemTcpReceiveDeadlineMs},
        .tunnel_contract =
            {
                .verify_cloned_session_lifetime = config::WgnxTunnelContractVerifyClonedSessionLifetime,
                .verify_mixed_batch = config::WgnxTunnelContractVerifyMixedBatch,
            },
        .bsd_system_udp = {
            .verify_post_route_rejection = config::BsdSystemUdpVerifyPostRouteRejection,
            .expected_outcome = bsd_expected_outcome,
            .require_writable_recovery = config::BsdSystemUdpRequireWritableRecovery,
        },
    };
}

ConfigLoadReport LoadRuntimeConfig(const RuntimeConfig& defaults, const char* path) {
    ConfigLoadReport report{.config = defaults};
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        if (errno != ENOENT) {
            report.diagnostics.push_back("unable to read configuration: " + std::string(std::strerror(errno)));
        }
        return report;
    }

    report.loaded_from_file = true;
    bool migrated_legacy = false;
    char line[512];
    std::size_t line_number = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        ++line_number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            report.diagnostics.push_back("configuration line " + std::to_string(line_number) + " has no '=' separator");
            continue;
        }
        const std::string key = Trim(std::string_view(trimmed).substr(0, separator));
        const std::string value = Trim(std::string_view(trimmed).substr(separator + 1));
        if (key.empty() || value.empty()) {
            report.diagnostics.push_back("configuration line " + std::to_string(line_number) + " has an empty key or value");
            continue;
        }
        std::string error;
        if (!ApplySetting(&report.config, key, value, &migrated_legacy, &error)) {
            report.diagnostics.push_back("configuration line " + std::to_string(line_number) + ": " + error);
        }
    }
    std::fclose(file);
    if (migrated_legacy) {
        report.diagnostics.push_back("legacy runtime configuration loaded; save to rewrite it as profiles and scenario settings");
    }
    std::string validation_error;
    if (!ValidateRuntimeConfig(report.config, &validation_error)) {
        report.config = defaults;
        report.diagnostics.push_back("configuration is invalid; using compiled defaults: " + validation_error);
    }
    return report;
}

bool ValidateRuntimeConfig(const RuntimeConfig& config, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (config.profiles.empty() || config.profiles.size() > MaximumProfiles || config.active_profile >= config.profiles.size()) {
        return fail("an active profile must be selected");
    }
    for (const RuntimeProfile& profile : config.profiles) {
        if (profile.name.empty() || profile.name.size() > MaximumProfileNameLength) {
            return fail("profile names must be between one and 32 characters");
        }
        if (!IsValidIpv4(profile.tunnel_destination_ipv4) || !IsValidIpv4(profile.bsd_destination_ipv4)) {
            return fail("profile destinations must be IPv4 addresses");
        }
        if (profile.udp_destination_port == 0) {
            return fail("profile UDP destination ports must be non-zero");
        }
        if (profile.tcp_destination_port == 0) {
            return fail("profile TCP destination ports must be non-zero");
        }
    }
    const UdpScenarioConfig& udp = config.udp;
    if (udp.payload_bytes < MinimumPayloadBytes || udp.payload_bytes > wgnx::tunnel::MaximumUdpPayloadStorageBytes) {
        return fail("udp.payload_bytes is outside the supported range");
    }
    if (udp.datagram_count == 0 || udp.datagram_count > MaximumDatagramCount) {
        return fail("udp.datagram_count is outside the supported range");
    }
    if (udp.pacing_ms > MaximumDurationMs || udp.receive_deadline_ms == 0 || udp.receive_deadline_ms > MaximumDurationMs) {
        return fail("udp timeout is outside the supported range");
    }
    if (udp.concurrent_flows == 0 || udp.concurrent_flows > wgnx::tunnel::MaximumFlowsPerClient) {
        return fail("udp.concurrent_flows is outside the supported range");
    }
    if (config.tcp.receive_deadline_ms == 0 || config.tcp.receive_deadline_ms > MaximumDurationMs) {
        return fail("tcp.receive_deadline_ms is outside the supported range");
    }
    if (config.scenario == RuntimeScenario::BsdSystemUdp) {
        if (config.bsd_system_udp.expected_outcome == BsdSystemUdpExpectedOutcome::NoReplyTimeout &&
            (udp.echo_replies || udp.datagram_count != 1 || udp.concurrent_flows != 1)) {
            return fail("BSD no-reply timeout requires no echo, one datagram, and one flow");
        }
        if (config.bsd_system_udp.expected_outcome == BsdSystemUdpExpectedOutcome::TerminalClosure &&
            (config.bsd_system_udp.require_writable_recovery || !udp.echo_replies || udp.datagram_count != 1 ||
             udp.concurrent_flows != 1)) {
            return fail("BSD terminal closure requires echo, one datagram, one flow, and no writable recovery");
        }
    }
    if (config.scenario == RuntimeScenario::TunnelContractValidation && !config.tunnel_contract.verify_cloned_session_lifetime &&
        !config.tunnel_contract.verify_mixed_batch) {
        return fail("tunnel contract validation must enable at least one check");
    }
    return true;
}

bool EnsureRuntimeConfigDirectories() {
    return EnsureDirectory("sdmc:/config") && EnsureDirectory("sdmc:/config/nxrv-toolbox");
}

const char* BsdSystemUdpExpectedOutcomeName(BsdSystemUdpExpectedOutcome expected_outcome) {
    switch (expected_outcome) {
    case BsdSystemUdpExpectedOutcome::EchoReply:
        return "echo";
    case BsdSystemUdpExpectedOutcome::NoReplyTimeout:
        return "no_reply_timeout";
    case BsdSystemUdpExpectedOutcome::TerminalClosure:
        return "terminal_closure";
    }
    return "invalid";
}

const char* RuntimeScenarioName(RuntimeScenario scenario) {
    switch (scenario) {
    case RuntimeScenario::DirectTunnelUdp:
        return "Direct tunnel UDP";
    case RuntimeScenario::BsdSystemUdp:
        return "BSD system UDP";
    case RuntimeScenario::BsdSystemTcp:
        return "BSD system TCP exchange";
    case RuntimeScenario::TunnelContractValidation:
        return "Tunnel contract validation";
    }
    return "Unknown scenario";
}

const RuntimeProfile* ActiveRuntimeProfile(const RuntimeConfig& config) {
    return config.active_profile < config.profiles.size() ? &config.profiles[config.active_profile] : nullptr;
}

RuntimeProfile* ActiveRuntimeProfile(RuntimeConfig* config) {
    return config != nullptr && config->active_profile < config->profiles.size() ? &config->profiles[config->active_profile] : nullptr;
}

TunnelUdpWorkloadConfig BuildTunnelUdpWorkload(const RuntimeConfig& config, const RuntimeProfile& profile, std::uint32_t workload_id) {
    return {
        .destination_ipv4 =
            config.scenario == RuntimeScenario::BsdSystemUdp ? profile.bsd_destination_ipv4 : profile.tunnel_destination_ipv4,
        .destination_port = profile.udp_destination_port,
        .workload_id = workload_id,
        .payload_bytes = config.udp.payload_bytes,
        .datagram_count = config.udp.datagram_count,
        .pacing_ms = config.udp.pacing_ms,
        .concurrent_flows = config.udp.concurrent_flows,
        .receive_deadline_ms = config.udp.receive_deadline_ms,
        .payload_seed = config.udp.payload_seed,
        .echo_replies = config.udp.echo_replies,
    };
}

bool SaveRuntimeConfig(const RuntimeConfig& config, std::string* error, const char* path) {
    if (!ValidateRuntimeConfig(config, error)) {
        return false;
    }
    if (!EnsureRuntimeConfigDirectories()) {
        if (error != nullptr) {
            *error = "unable to create configuration directory";
        }
        return false;
    }

    const std::string temporary_path = std::string(path) + ".tmp";
    FILE* file = std::fopen(temporary_path.c_str(), "w");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = "unable to create configuration: " + std::string(std::strerror(errno));
        }
        return false;
    }
    std::fprintf(
        file,
        "# NX Reversing Toolbox runtime configuration\n"
        "run.scenario=%s\n"
        "run.next_workload_id=%u\n"
        "run.active_profile=%zu\n"
        "profiles.count=%zu\n",
        config.scenario == RuntimeScenario::DirectTunnelUdp
            ? "direct_tunnel_udp"
            : (config.scenario == RuntimeScenario::BsdSystemUdp
                   ? "bsd_system_udp"
                   : (config.scenario == RuntimeScenario::BsdSystemTcp ? "bsd_system_tcp" : "tunnel_contract_validation")),
        config.next_workload_id,
        config.active_profile,
        config.profiles.size()
    );
    for (std::size_t index = 0; index < config.profiles.size(); ++index) {
        const RuntimeProfile& profile = config.profiles[index];
        std::fprintf(
            file,
            "profile.%zu.name=%s\n"
            "profile.%zu.tunnel_destination_ipv4=%s\n"
            "profile.%zu.bsd_destination_ipv4=%s\n"
            "profile.%zu.udp_destination_port=%u\n"
            "profile.%zu.tcp_destination_port=%u\n",
            index,
            profile.name.c_str(),
            index,
            profile.tunnel_destination_ipv4.c_str(),
            index,
            profile.bsd_destination_ipv4.c_str(),
            index,
            profile.udp_destination_port,
            index,
            profile.tcp_destination_port
        );
    }
    std::fprintf(
        file,
        "udp.payload_bytes=%zu\n"
        "udp.datagram_count=%u\n"
        "udp.pacing_ms=%u\n"
        "udp.concurrent_flows=%u\n"
        "udp.receive_deadline_ms=%u\n"
        "udp.payload_seed=%u\n"
        "udp.echo_replies=%s\n"
        "tcp.receive_deadline_ms=%u\n"
        "tunnel_contract.verify_cloned_session_lifetime=%s\n"
        "tunnel_contract.verify_mixed_batch=%s\n"
        "bsd_system_udp.verify_post_route_rejection=%s\n"
        "bsd_system_udp.expected_outcome=%s\n"
        "bsd_system_udp.require_writable_recovery=%s\n",
        config.udp.payload_bytes,
        config.udp.datagram_count,
        config.udp.pacing_ms,
        config.udp.concurrent_flows,
        config.udp.receive_deadline_ms,
        config.udp.payload_seed,
        config.udp.echo_replies ? "true" : "false",
        config.tcp.receive_deadline_ms,
        config.tunnel_contract.verify_cloned_session_lifetime ? "true" : "false",
        config.tunnel_contract.verify_mixed_batch ? "true" : "false",
        config.bsd_system_udp.verify_post_route_rejection ? "true" : "false",
        BsdSystemUdpExpectedOutcomeName(config.bsd_system_udp.expected_outcome),
        config.bsd_system_udp.require_writable_recovery ? "true" : "false"
    );
    const bool flush_failed = std::fflush(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    if (flush_failed || close_failed) {
        std::remove(temporary_path.c_str());
        if (error != nullptr) {
            *error = "unable to write configuration";
        }
        return false;
    }
    const std::string backup_path = std::string(path) + ".bak";
    if (std::remove(backup_path.c_str()) != 0 && errno != ENOENT) {
        std::remove(temporary_path.c_str());
        if (error != nullptr) {
            *error = "unable to clear previous configuration backup: " + std::string(std::strerror(errno));
        }
        return false;
    }
    bool moved_previous_configuration = false;
    if (std::rename(path, backup_path.c_str()) == 0) {
        moved_previous_configuration = true;
    } else if (errno != ENOENT) {
        std::remove(temporary_path.c_str());
        if (error != nullptr) {
            *error = "unable to prepare configuration replacement: " + std::string(std::strerror(errno));
        }
        return false;
    }
    if (std::rename(temporary_path.c_str(), path) != 0) {
        const int replace_error = errno;
        const bool restored_previous_configuration = !moved_previous_configuration || std::rename(backup_path.c_str(), path) == 0;
        std::remove(temporary_path.c_str());
        if (error != nullptr) {
            *error = "unable to replace configuration: " + std::string(std::strerror(replace_error));
            if (!restored_previous_configuration) {
                *error += "; unable to restore previous configuration";
            }
        }
        return false;
    }
    if (moved_previous_configuration) {
        std::remove(backup_path.c_str());
    }
    return true;
}

} // namespace toolbox
