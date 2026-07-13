#include <switch.h>

#include <curl/curl.h>
#include <vector>

#include "config.hpp"
#include "logger.hpp"
#include "runtime.hpp"
#include "scenarios.hpp"

namespace {

using namespace requester;

void PrintSummary(AppContext& ctx, const std::vector<ScenarioResult>& results) {
    for (const auto& result : results) {
        const char *state = result.skipped ? "SKIP" : (result.success ? "OK" : "FAIL");
        logger::Status(
            ctx,
            "[%s] %s | sent=%zu recv=%zu | %s",
            state,
            result.name.c_str(),
            result.bytes_sent,
            result.bytes_received,
            result.detail.c_str());
    }
}

} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    logger::Bootstrap("main: enter");
    consoleInit(nullptr);
    logger::Bootstrap("main: consoleInit complete");

    AppContext ctx {};

    std::printf("%s\n", APP_TITLE);
    consoleUpdate(nullptr);

    logger::Bootstrap("main: EnsureLogDirectories begin");
    if (!EnsureLogDirectories()) {
        logger::Bootstrap("main: EnsureLogDirectories failed");
        std::printf("Failed to create %s\n", config::LogDirectory);
        consoleUpdate(nullptr);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }
    logger::Bootstrap("main: EnsureLogDirectories complete");

    logger::Bootstrap("main: OpenLog begin");
    if (!logger::OpenLog(ctx)) {
        logger::Bootstrap("main: OpenLog failed");
        std::printf("Failed to open requester log\n");
        consoleUpdate(nullptr);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }
    logger::Bootstrap("main: OpenLog complete path=%s", ctx.log_path.c_str());

    logger::Log(ctx, "requester start title=%s version=%s-%s", APP_TITLE, VERSION, BUILD_ID);
    logger::Log(ctx, "hos_version=%s", FormatHosVersion().c_str());

    const bool is_application = appletGetAppletType() == AppletType_Application;
    logger::Bootstrap(
        "main: applet type resolved mode=%s",
        is_application ? "application" : "applet");
    const SocketInitConfig &socket_config =
        is_application ? config::SocketConfigApplication : config::SocketConfigApplet;
    logger::Log(
        ctx,
        "execution_config applet_exit_lock=%u socket=%u nifm=%u ssl=%u curl=%u curl_share=%u scenario_manual_bsd=%u manual_bsd_open_monitor=%u manual_bsd_create_tmem=%u manual_bsd_register=%u manual_bsd_start_monitoring=%u manual_bsd_clone_root=%u scenario_environment=%u scenario_dns=%u scenario_plain_tcp=%u scenario_idle_tcp=%u scenario_http=%u scenario_https=%u scenario_curl_http=%u scenario_curl_https=%u scenario_udp_socket_only=%u scenario_udp_socket_setsockopt=%u scenario_udp_setsockopt_reuseaddr=%u scenario_udp_setsockopt_recv_timeout=%u scenario_udp_setsockopt_send_timeout=%u scenario_udp_sendto_only=%u udp_sendto_only_timeouts=%u scenario_udp_connect_send_only=%u scenario_udp=%u scenario_tcp_multi=%u scenario_wgnx_udp=%u",
        static_cast<unsigned>(config::EnableAppletExitLock),
        static_cast<unsigned>(config::EnableSocketInitialize),
        static_cast<unsigned>(config::EnableNifmInitialize),
        static_cast<unsigned>(config::EnableSslInitialize),
        static_cast<unsigned>(config::EnableCurlInitialize),
        static_cast<unsigned>(config::EnableCurlShare),
        static_cast<unsigned>(config::EnableScenarioManualBsdLifecycle),
        static_cast<unsigned>(config::ManualBsdOpenMonitorSession),
        static_cast<unsigned>(config::ManualBsdCreateTransferMemory),
        static_cast<unsigned>(config::ManualBsdRegisterClient),
        static_cast<unsigned>(config::ManualBsdStartMonitoring),
        static_cast<unsigned>(config::ManualBsdCloneRootSession),
        static_cast<unsigned>(config::EnableScenarioEnvironmentSnapshot),
        static_cast<unsigned>(config::EnableScenarioDnsResolve),
        static_cast<unsigned>(config::EnableScenarioPlainTcpConnect),
        static_cast<unsigned>(config::EnableScenarioIdleTcpHold),
        static_cast<unsigned>(config::EnableScenarioHttpGet),
        static_cast<unsigned>(config::EnableScenarioHttpsGet),
        static_cast<unsigned>(config::EnableScenarioCurlHttpGet),
        static_cast<unsigned>(config::EnableScenarioCurlHttpsGet),
        static_cast<unsigned>(config::EnableScenarioUdpSocketOnly),
        static_cast<unsigned>(config::EnableScenarioUdpSocketSetSockOpt),
        static_cast<unsigned>(config::EnableScenarioUdpSetSockOptReuseAddr),
        static_cast<unsigned>(config::EnableScenarioUdpSetSockOptRecvTimeout),
        static_cast<unsigned>(config::EnableScenarioUdpSetSockOptSendTimeout),
        static_cast<unsigned>(config::EnableScenarioUdpSendToOnly),
        static_cast<unsigned>(config::EnableUdpSendToOnlyTimeouts),
        static_cast<unsigned>(config::EnableScenarioUdpConnectSendOnly),
        static_cast<unsigned>(config::EnableScenarioUdpEcho),
        static_cast<unsigned>(config::EnableScenarioConcurrentTcpBurst),
        static_cast<unsigned>(config::EnableScenarioWgnxUdpEcho));
    logger::Log(
        ctx,
        "wgnx_packet_config api=%u source=%s:%u destination=%s:%u timeout_ms=%u poll_ms=%u malformed_ipv4_checksum=%u",
        config::WgnxExpectedApiVersion,
        config::WgnxTunnelSourceIpv4,
        config::WgnxUdpSourcePort,
        config::WgnxUdpEchoDestinationIpv4,
        config::WgnxUdpEchoDestinationPort,
        config::WgnxPacketTimeoutMs,
        config::WgnxPacketPollIntervalMs,
        static_cast<unsigned>(config::WgnxSubmitMalformedIpv4Checksum));
    logger::Log(
        ctx,
        "socket_config mode=%s tcp_tx=%u tcp_rx=%u tcp_tx_max=%u tcp_rx_max=%u udp_tx=%u udp_rx=%u sb_efficiency=%u num_bsd_sessions=%u service_type=%u",
        is_application ? "application" : "applet",
        socket_config.tcp_tx_buf_size,
        socket_config.tcp_rx_buf_size,
        socket_config.tcp_tx_buf_max_size,
        socket_config.tcp_rx_buf_max_size,
        socket_config.udp_tx_buf_size,
        socket_config.udp_rx_buf_size,
        socket_config.sb_efficiency,
        socket_config.num_bsd_sessions,
        static_cast<unsigned>(socket_config.bsd_service_type));

    if (config::EnableAppletExitLock) {
        logger::Bootstrap("main: appletLockExit begin");
        Result rc = appletLockExit();
        logger::Bootstrap("main: appletLockExit rc=%s", FormatResult(rc).c_str());
        logger::Log(ctx, "appletLockExit -> %s", FormatResult(rc).c_str());
        if (R_SUCCEEDED(rc)) {
            ctx.applet_exit_locked = true;
        } else {
            logger::Status(ctx, "appletLockExit failed: %s", FormatResult(rc).c_str());
        }
    } else {
        logger::Bootstrap("main: appletLockExit skipped");
        logger::Log(ctx, "appletLockExit skipped by config");
    }

    if (config::EnableSocketInitialize) {
        logger::Bootstrap("main: socketInitialize begin");
        ctx.socket_initialize_rc = socketInitialize(&socket_config);
        ctx.socket_initialized = R_SUCCEEDED(ctx.socket_initialize_rc);
        logger::Bootstrap("main: socketInitialize rc=%s", FormatResult(ctx.socket_initialize_rc).c_str());
        logger::Log(ctx, "socketInitialize(custom) -> %s", FormatResult(ctx.socket_initialize_rc).c_str());
        if (R_FAILED(ctx.socket_initialize_rc)) {
            logger::Status(ctx, "socketInitialize(custom) failed: %s", FormatResult(ctx.socket_initialize_rc).c_str());
            if (ctx.applet_exit_locked) {
                appletUnlockExit();
            }
            logger::CloseLog(ctx);
            SleepMilliseconds(config::ExitDelayMs);
            consoleExit(nullptr);
            return 1;
        }
    } else {
        logger::Bootstrap("main: socketInitialize skipped");
        logger::Log(ctx, "socketInitialize(custom) skipped by config");
    }

    if (config::EnableNifmInitialize) {
        logger::Bootstrap("main: nifmInitialize begin");
        ctx.nifm_initialize_rc = nifmInitialize(NifmServiceType_User);
        ctx.nifm_initialized = R_SUCCEEDED(ctx.nifm_initialize_rc);
        logger::Bootstrap("main: nifmInitialize rc=%s", FormatResult(ctx.nifm_initialize_rc).c_str());
        logger::Log(ctx, "nifmInitialize(User) -> %s", FormatResult(ctx.nifm_initialize_rc).c_str());
        if (R_FAILED(ctx.nifm_initialize_rc)) {
            logger::Status(ctx, "nifmInitialize(User) failed: %s", FormatResult(ctx.nifm_initialize_rc).c_str());
            if (ctx.socket_initialized) {
                socketExit();
                ctx.socket_initialized = false;
            }
            if (ctx.applet_exit_locked) {
                appletUnlockExit();
            }
            logger::CloseLog(ctx);
            SleepMilliseconds(config::ExitDelayMs);
            consoleExit(nullptr);
            return 1;
        }
    } else {
        logger::Bootstrap("main: nifmInitialize skipped");
        logger::Log(ctx, "nifmInitialize(User) skipped by config");
    }

    if (config::EnableSslInitialize) {
        logger::Bootstrap("main: sslInitialize begin");
        ctx.ssl_initialize_rc = sslInitialize(3);
        ctx.ssl_initialized = R_SUCCEEDED(ctx.ssl_initialize_rc);
        logger::Bootstrap("main: sslInitialize rc=%s", FormatResult(ctx.ssl_initialize_rc).c_str());
        logger::Log(ctx, "sslInitialize(3) -> %s", FormatResult(ctx.ssl_initialize_rc).c_str());
    } else {
        logger::Bootstrap("main: sslInitialize skipped");
        logger::Log(ctx, "sslInitialize(3) skipped by config");
    }

    if (config::EnableCurlInitialize) {
        logger::Bootstrap("main: curl_global_init begin");
        ctx.curl_global_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        ctx.curl_initialized = ctx.curl_global_rc == CURLE_OK;
        logger::Bootstrap(
            "main: curl_global_init rc=%d (%s)",
            static_cast<int>(ctx.curl_global_rc),
            curl_easy_strerror(ctx.curl_global_rc));
        logger::Log(
            ctx,
            "curl_global_init(CURL_GLOBAL_DEFAULT) -> %d (%s)",
            static_cast<int>(ctx.curl_global_rc),
            curl_easy_strerror(ctx.curl_global_rc));
    } else {
        logger::Bootstrap("main: curl_global_init skipped");
        logger::Log(ctx, "curl_global_init(CURL_GLOBAL_DEFAULT) skipped by config");
    }
    if (ctx.curl_initialized && config::EnableCurlShare) {
        ctx.curl_share = curl_share_init();
        logger::Log(ctx, "curl_share_init -> %p", static_cast<void *>(ctx.curl_share));
        if (ctx.curl_share != nullptr) {
            static_cast<void>(curl_share_setopt(ctx.curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE));
            static_cast<void>(curl_share_setopt(ctx.curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS));
            static_cast<void>(curl_share_setopt(ctx.curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION));
            static_cast<void>(curl_share_setopt(ctx.curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT));
            static_cast<void>(curl_share_setopt(ctx.curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL));
        }
    } else if (ctx.curl_initialized) {
        logger::Log(ctx, "curl_share_init skipped by config");
    }

    logger::Status(ctx, "Requester run id: %s", ctx.run_id.c_str());
    logger::Status(ctx, "Log path: %s", ctx.log_path.c_str());
    logger::Status(ctx, "HOS Version: %s", FormatHosVersion().c_str());

    logger::Bootstrap("main: RunScenarios begin");
    const auto results = RunScenarios(ctx);
    logger::Bootstrap("main: RunScenarios complete count=%zu", results.size());
    PrintSummary(ctx, results);

    logger::Status(ctx, "Requester finished; exiting in %u seconds", config::ExitDelayMs / 1000U);
    logger::Log(ctx, "requester finished");
    logger::Bootstrap("main: cleanup begin");

    if (ctx.curl_share != nullptr) {
        curl_share_cleanup(ctx.curl_share);
        ctx.curl_share = nullptr;
    }
    if (ctx.curl_initialized) {
        curl_global_cleanup();
        ctx.curl_initialized = false;
    }
    if (ctx.ssl_initialized) {
        sslExit();
    }
    if (ctx.nifm_initialized) {
        nifmExit();
        ctx.nifm_initialized = false;
    }
    if (ctx.socket_initialized) {
        socketExit();
        ctx.socket_initialized = false;
    }
    if (ctx.applet_exit_locked) {
        appletUnlockExit();
        ctx.applet_exit_locked = false;
    }
    logger::CloseLog(ctx);
    logger::Bootstrap("main: cleanup complete");

    SleepMilliseconds(config::ExitDelayMs);
    consoleExit(nullptr);
    logger::Bootstrap("main: exit");
    return 0;
}
