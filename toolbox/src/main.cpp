#include <switch.h>

#include "logger.hpp"
#include "nxrv/build_info.hpp"
#include "app_ui.hpp"
#include "runtime.hpp"
#include "runtime_config.hpp"
#include "wgnx/protocol.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace {

void LogDiagnostics(toolbox::AppContext& context, const toolbox::RuntimeConfig& config, const toolbox::ConfigLoadReport& report) {
    toolbox::logger::Log(context, "toolbox start title=%s version=%s", APP_TITLE, nxrv::build_info::VersionWithBuild);
    toolbox::logger::Log(context, "hos_version=%s", toolbox::FormatHosVersion().c_str());
    toolbox::logger::Log(context, "run_id=%s log_path=%s", context.run_id.c_str(), context.log_path.c_str());
    toolbox::logger::Log(context, "wgnx_api ctl=%u tun=%u", wgnx::IpcApiVersion, wgnx::tunnel::TunApiVersion);
    const toolbox::RuntimeProfile* const profile = toolbox::ActiveRuntimeProfile(config);
    toolbox::logger::Log(
        context,
        "runtime_config source=%s path=%s scenario=%s profile=%s next_workload=%u "
        "tunnel_destination=%s bsd_destination=%s udp_port=%u tcp_port=%u payload=%zu datagrams=%u pacing_ms=%u flows=%u "
        "udp_deadline_ms=%u tcp_deadline_ms=%u seed=%u echo=%u",
        report.loaded_from_file ? "file" : "compiled_defaults",
        toolbox::RuntimeConfigPath,
        toolbox::RuntimeScenarioName(config.scenario),
        profile == nullptr ? "none" : profile->name.c_str(),
        config.next_workload_id,
        profile == nullptr ? "" : profile->tunnel_destination_ipv4.c_str(),
        profile == nullptr ? "" : profile->bsd_destination_ipv4.c_str(),
        profile == nullptr ? 0 : profile->udp_destination_port,
        profile == nullptr ? 0 : profile->tcp_destination_port,
        config.udp.payload_bytes,
        config.udp.datagram_count,
        config.udp.pacing_ms,
        config.udp.concurrent_flows,
        config.udp.receive_deadline_ms,
        config.tcp.receive_deadline_ms,
        config.udp.payload_seed,
        static_cast<unsigned>(config.udp.echo_replies)
    );
    for (const std::string& diagnostic : report.diagnostics) {
        toolbox::logger::Status(context, "configuration warning: %s", diagnostic.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    toolbox::logger::Bootstrap("main: enter");
    toolbox::AppContext context{};

    if (!toolbox::EnsureLogDirectories() || !toolbox::EnsureRuntimeConfigDirectories()) {
        toolbox::logger::Bootstrap("main: directory setup failed");
        return 1;
    }
    if (!toolbox::logger::OpenLog(context)) {
        toolbox::logger::Bootstrap("main: log open failed");
        return 1;
    }

    const toolbox::RuntimeConfig defaults = toolbox::CompiledRuntimeDefaults();
    toolbox::ConfigLoadReport loaded_config = toolbox::LoadRuntimeConfig(defaults);
    LogDiagnostics(context, loaded_config.config, loaded_config);

    const int rc = toolbox::RunAppUi(context, defaults, loaded_config);
    toolbox::logger::Log(context, "toolbox exit rc=%d", rc);
    toolbox::logger::CloseLog(context);
    toolbox::logger::Bootstrap("main: exit rc=%d", rc);
    return rc;
}
