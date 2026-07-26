#include <switch.h>

#include "logger.hpp"
#include "requester_ui.hpp"
#include "runtime.hpp"
#include "runtime_config.hpp"
#include "wgnx/protocol.hpp"
#include "wgnx/tunnel_protocol.hpp"

namespace {

void LogDiagnostics(requester::AppContext &context,
                    const requester::RuntimeConfig &config,
                    const requester::ConfigLoadReport &report) {
  requester::logger::Log(context, "requester start title=%s version=%s-%s",
                         APP_TITLE, VERSION, BUILD_ID);
  requester::logger::Log(context, "hos_version=%s",
                         requester::FormatHosVersion().c_str());
  requester::logger::Log(context, "run_id=%s log_path=%s", context.run_id.c_str(),
                         context.log_path.c_str());
  requester::logger::Log(context, "wgnx_api ctl=%u tun=%u",
                         wgnx::IpcApiVersion, wgnx::tunnel::TunApiVersion);
  requester::logger::Log(
      context,
      "runtime_config source=%s path=%s tunnel_udp_enabled=%u destination=%s:%u "
      "payload=%zu datagrams=%u pacing_ms=%u flows=%u deadline_ms=%u seed=%u "
      "echo=%u",
      report.loaded_from_file ? "file" : "compiled_defaults",
      requester::RuntimeConfigPath,
      static_cast<unsigned>(config.tunnel_udp.enabled),
      config.tunnel_udp.destination_ipv4.c_str(),
      config.tunnel_udp.destination_port, config.tunnel_udp.payload_bytes,
      config.tunnel_udp.datagram_count, config.tunnel_udp.pacing_ms,
      config.tunnel_udp.concurrent_flows,
      config.tunnel_udp.receive_deadline_ms, config.tunnel_udp.payload_seed,
      static_cast<unsigned>(config.tunnel_udp.echo_replies));
  for (const std::string &diagnostic : report.diagnostics) {
    requester::logger::Status(context, "configuration warning: %s",
                              diagnostic.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  requester::logger::Bootstrap("main: enter");
  requester::AppContext context{};

  if (!requester::EnsureLogDirectories() ||
      !requester::EnsureRuntimeConfigDirectories()) {
    requester::logger::Bootstrap("main: directory setup failed");
    return 1;
  }
  if (!requester::logger::OpenLog(context)) {
    requester::logger::Bootstrap("main: log open failed");
    return 1;
  }

  const requester::RuntimeConfig defaults = requester::CompiledRuntimeDefaults();
  requester::ConfigLoadReport loaded_config =
      requester::LoadRuntimeConfig(defaults);
  LogDiagnostics(context, loaded_config.config, loaded_config);

  const int rc = requester::RunRequesterUi(context, defaults,
                                            std::move(loaded_config));
  requester::logger::Log(context, "requester exit rc=%d", rc);
  requester::logger::CloseLog(context);
  requester::logger::Bootstrap("main: exit rc=%d", rc);
  return rc;
}
