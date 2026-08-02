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
    toolbox::logger::Log(
        context,
        "runtime_config source=%s path=%s udp_data_path=%s "
        "destination=%s:%u "
        "payload=%zu datagrams=%u pacing_ms=%u flows=%u deadline_ms=%u seed=%u "
        "echo=%u tunnel_contract_enabled=%u clone_lifetime=%u mixed_batch=%u",
        report.loaded_from_file ? "file" : "compiled_defaults",
        toolbox::RuntimeConfigPath,
        config.bsd_system_udp.enabled ? "bsd:s" : "tunnel",
        config.tunnel_udp.destination_ipv4.c_str(),
        config.tunnel_udp.destination_port,
        config.tunnel_udp.payload_bytes,
        config.tunnel_udp.datagram_count,
        config.tunnel_udp.pacing_ms,
        config.tunnel_udp.concurrent_flows,
        config.tunnel_udp.receive_deadline_ms,
        config.tunnel_udp.payload_seed,
        static_cast<unsigned>(config.tunnel_udp.echo_replies),
        static_cast<unsigned>(config.tunnel_contract.enabled),
        static_cast<unsigned>(config.tunnel_contract.verify_cloned_session_lifetime),
        static_cast<unsigned>(config.tunnel_contract.verify_mixed_batch)
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

    const int rc = toolbox::RunAppUi(context, defaults, std::move(loaded_config));
    toolbox::logger::Log(context, "toolbox exit rc=%d", rc);
    toolbox::logger::CloseLog(context);
    toolbox::logger::Bootstrap("main: exit rc=%d", rc);
    return rc;
}
