#include "manual_bsd_lifecycle.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

#include <switch.h>

#include "config.hpp"
#include "logger.hpp"

namespace requester {

namespace {

struct BsdServiceConfig {
    u32 version;
    u32 tcp_tx_buf_size;
    u32 tcp_rx_buf_size;
    u32 tcp_tx_buf_max_size;
    u32 tcp_rx_buf_max_size;
    u32 udp_tx_buf_size;
    u32 udp_rx_buf_size;
    u32 sb_efficiency;
};

struct RegisterClientRequest {
    BsdServiceConfig config;
    u64 pid_placeholder;
    u64 transfer_memory_size;
};

static_assert(sizeof(BsdServiceConfig) == 0x20);
static_assert(sizeof(RegisterClientRequest) == 0x30);

u32 SelectBsdVersion() {
    if (hosversionBefore(3, 0, 0)) {
        return 1;
    }
    if (hosversionBefore(4, 0, 0)) {
        return 2;
    }
    if (hosversionBefore(5, 0, 0)) {
        return 3;
    }
    if (hosversionBefore(6, 0, 0)) {
        return 4;
    }
    if (hosversionBefore(8, 0, 0)) {
        return 5;
    }
    if (hosversionBefore(9, 0, 0)) {
        return 6;
    }
    if (hosversionBefore(13, 0, 0)) {
        return 7;
    }
    if (hosversionBefore(16, 0, 0)) {
        return 8;
    }
    return 9;
}

size_t CalculateTransferMemorySize(const SocketInitConfig& config) {
    const u32 tcp_tx_max = config.tcp_tx_buf_max_size != 0
        ? config.tcp_tx_buf_max_size
        : config.tcp_tx_buf_size;
    const u32 tcp_rx_max = config.tcp_rx_buf_max_size != 0
        ? config.tcp_rx_buf_max_size
        : config.tcp_rx_buf_size;
    u32 sum = tcp_tx_max + tcp_rx_max + config.udp_tx_buf_size + config.udp_rx_buf_size;
    sum = (sum + 0xFFFU) & ~0xFFFU;
    return static_cast<size_t>(config.sb_efficiency) * sum;
}

void LogService(AppContext& ctx, const char *phase, const Service& service, Result rc) {
    logger::Log(
        ctx,
        "manual_bsd phase=%s rc=%s handle=0x%X own_handle=%u object_id=%u pointer_buffer_size=%u",
        phase,
        FormatResult(rc).c_str(),
        service.session,
        service.own_handle,
        service.object_id,
        service.pointer_buffer_size);
}

void CloseServiceTraced(AppContext& ctx, const char *name, Service& service) {
    const Handle handle = service.session;
    const u32 own_handle = service.own_handle;
    const u32 object_id = service.object_id;
    Result close_request_rc = 0;
    Result close_handle_rc = 0;

    logger::Log(
        ctx,
        "manual_bsd phase=cleanup_%s_begin handle=0x%X own_handle=%u object_id=%u",
        name,
        handle,
        own_handle,
        object_id);

    if (own_handle != 0 || object_id != 0) {
        cmifMakeCloseRequest(armGetTls(), own_handle != 0 ? 0 : object_id);
        logger::Log(
            ctx,
            "manual_bsd phase=cleanup_%s_close_request_begin handle=0x%X object_id=%u",
            name,
            handle,
            own_handle != 0 ? 0 : object_id);
        close_request_rc = svcSendSyncRequest(handle);
        logger::Log(
            ctx,
            "manual_bsd phase=cleanup_%s_close_request_complete handle=0x%X rc=%s",
            name,
            handle,
            FormatResult(close_request_rc).c_str());

        if (own_handle != 0) {
            logger::Log(
                ctx,
                "manual_bsd phase=cleanup_%s_close_handle_begin handle=0x%X",
                name,
                handle);
            close_handle_rc = svcCloseHandle(handle);
            logger::Log(
                ctx,
                "manual_bsd phase=cleanup_%s_close_handle_complete handle=0x%X rc=%s",
                name,
                handle,
                FormatResult(close_handle_rc).c_str());
        }
    }

    service = {};
    logger::Log(
        ctx,
        "manual_bsd phase=cleanup_%s_complete close_request_rc=%s close_handle_rc=%s",
        name,
        FormatResult(close_request_rc).c_str(),
        FormatResult(close_handle_rc).c_str());
}

struct ManualBsdState {
    explicit ManualBsdState(AppContext& context) : ctx(context) {}

    Result Cleanup() {
        Result first_failure = 0;

        if (serviceIsActive(&clone)) {
            CloseServiceTraced(ctx, "clone", clone);
        }
        if (serviceIsActive(&monitor)) {
            CloseServiceTraced(ctx, "monitor", monitor);
        }
        if (serviceIsActive(&root)) {
            CloseServiceTraced(ctx, "root", root);
        }
        if (transfer_memory_created) {
            logger::Log(
                ctx,
                "manual_bsd phase=cleanup_tmem_begin handle=0x%X size=%zu",
                transfer_memory.handle,
                transfer_memory.size);
            const Result rc = tmemClose(&transfer_memory);
            logger::Log(
                ctx,
                "manual_bsd phase=cleanup_tmem_complete rc=%s handle=0x%X",
                FormatResult(rc).c_str(),
                transfer_memory.handle);
            transfer_memory_created = false;
            if (R_FAILED(rc)) {
                first_failure = rc;
            }
        }

        logger::Log(ctx, "manual_bsd phase=cleanup_complete rc=%s", FormatResult(first_failure).c_str());
        return first_failure;
    }

    AppContext& ctx;
    Service root {};
    Service monitor {};
    Service clone {};
    TransferMemory transfer_memory {};
    bool transfer_memory_created = false;
};

ScenarioResult FinishWithFailure(
    ManualBsdState& state,
    ScenarioResult result,
    Result rc,
    const char *phase) {
    result.rc = rc;
    result.detail = std::string(phase) + " failed: " + FormatResult(rc);
    logger::Log(
        state.ctx,
        "manual_bsd phase=failure failed_phase=%s rc=%s",
        phase,
        FormatResult(rc).c_str());
    static_cast<void>(state.Cleanup());
    return result;
}

} // namespace

ScenarioResult RunManualBsdLifecycle(AppContext& ctx) {
    ScenarioResult result { .name = "manual_bsd_lifecycle" };
    ManualBsdState state(ctx);

    const bool is_application = appletGetAppletType() == AppletType_Application;
    const SocketInitConfig& socket_config =
        is_application ? config::SocketConfigApplication : config::SocketConfigApplet;
    const u32 bsd_version = SelectBsdVersion();

    u64 process_id = 0;
    const Result process_id_rc = svcGetProcessId(&process_id, CUR_PROCESS_HANDLE);
    u64 program_id = 0;
    const Result program_id_rc = svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);

    logger::Status(ctx, "Running manual bsd:s RegisterClient lifecycle");
    logger::Log(
        ctx,
        "manual_bsd phase=begin process_id_rc=%s process_id=0x%016llX program_id_rc=%s program_id=0x%016llX app_mode=%s bsd_version=%u open_monitor=%u create_tmem=%u register_client=%u start_monitoring=%u clone_root=%u",
        FormatResult(process_id_rc).c_str(),
        static_cast<unsigned long long>(process_id),
        FormatResult(program_id_rc).c_str(),
        static_cast<unsigned long long>(program_id),
        is_application ? "application" : "applet",
        bsd_version,
        static_cast<unsigned>(config::ManualBsdOpenMonitorSession),
        static_cast<unsigned>(config::ManualBsdCreateTransferMemory),
        static_cast<unsigned>(config::ManualBsdRegisterClient),
        static_cast<unsigned>(config::ManualBsdStartMonitoring),
        static_cast<unsigned>(config::ManualBsdCloneRootSession));

    logger::Log(ctx, "manual_bsd phase=open_root_begin service=bsd:s");
    Result rc = smGetService(&state.root, "bsd:s");
    LogService(ctx, "open_root_complete", state.root, rc);
    if (R_FAILED(rc)) {
        return FinishWithFailure(state, std::move(result), rc, "open_root");
    }

    if (config::ManualBsdOpenMonitorSession) {
        logger::Log(ctx, "manual_bsd phase=open_monitor_begin service=bsd:s");
        rc = smGetService(&state.monitor, "bsd:s");
        LogService(ctx, "open_monitor_complete", state.monitor, rc);
        if (R_FAILED(rc)) {
            return FinishWithFailure(state, std::move(result), rc, "open_monitor");
        }
    } else {
        logger::Log(ctx, "manual_bsd phase=open_monitor_skipped");
    }

    if (config::ManualBsdCreateTransferMemory) {
        const size_t transfer_memory_size = CalculateTransferMemorySize(socket_config);
        logger::Log(
            ctx,
            "manual_bsd phase=create_tmem_begin size=%zu tcp_tx=%u tcp_rx=%u tcp_tx_max=%u tcp_rx_max=%u udp_tx=%u udp_rx=%u sb_efficiency=%u",
            transfer_memory_size,
            socket_config.tcp_tx_buf_size,
            socket_config.tcp_rx_buf_size,
            socket_config.tcp_tx_buf_max_size,
            socket_config.tcp_rx_buf_max_size,
            socket_config.udp_tx_buf_size,
            socket_config.udp_rx_buf_size,
            socket_config.sb_efficiency);
        rc = tmemCreate(&state.transfer_memory, transfer_memory_size, Perm_None);
        state.transfer_memory_created = R_SUCCEEDED(rc);
        logger::Log(
            ctx,
            "manual_bsd phase=create_tmem_complete rc=%s handle=0x%X size=%zu",
            FormatResult(rc).c_str(),
            state.transfer_memory.handle,
            state.transfer_memory.size);
        if (R_FAILED(rc)) {
            return FinishWithFailure(state, std::move(result), rc, "create_tmem");
        }
    } else {
        logger::Log(ctx, "manual_bsd phase=create_tmem_skipped");
    }

    u64 client_id = UINT64_MAX;
    if (config::ManualBsdRegisterClient) {
        const RegisterClientRequest request {
            .config = {
                .version = bsd_version,
                .tcp_tx_buf_size = socket_config.tcp_tx_buf_size,
                .tcp_rx_buf_size = socket_config.tcp_rx_buf_size,
                .tcp_tx_buf_max_size = socket_config.tcp_tx_buf_max_size,
                .tcp_rx_buf_max_size = socket_config.tcp_rx_buf_max_size,
                .udp_tx_buf_size = socket_config.udp_tx_buf_size,
                .udp_rx_buf_size = socket_config.udp_rx_buf_size,
                .sb_efficiency = socket_config.sb_efficiency,
            },
            .pid_placeholder = 0,
            .transfer_memory_size = state.transfer_memory.size,
        };

        logger::Log(
            ctx,
            "manual_bsd phase=register_client_begin command_id=0 root_handle=0x%X tmem_handle=0x%X tmem_size=%zu output_initial=0x%016llX",
            state.root.session,
            state.transfer_memory.handle,
            state.transfer_memory.size,
            static_cast<unsigned long long>(client_id));
        rc = serviceDispatchInOut(
            &state.root,
            0,
            request,
            client_id,
            .in_send_pid = true,
            .in_num_handles = 1,
            .in_handles = { state.transfer_memory.handle });
        logger::Log(
            ctx,
            "manual_bsd phase=register_client_complete command_id=0 rc=%s root_handle=0x%X client_id=0x%016llX",
            FormatResult(rc).c_str(),
            state.root.session,
            static_cast<unsigned long long>(client_id));
        if (R_FAILED(rc)) {
            return FinishWithFailure(state, std::move(result), rc, "register_client");
        }
    } else {
        logger::Log(ctx, "manual_bsd phase=register_client_skipped");
    }

    if (config::ManualBsdStartMonitoring) {
        logger::Log(
            ctx,
            "manual_bsd phase=start_monitoring_begin command_id=1 monitor_handle=0x%X client_id=0x%016llX",
            state.monitor.session,
            static_cast<unsigned long long>(client_id));
        rc = serviceDispatchIn(
            &state.monitor,
            1,
            client_id,
            .in_send_pid = true);
        logger::Log(
            ctx,
            "manual_bsd phase=start_monitoring_complete command_id=1 rc=%s monitor_handle=0x%X client_id=0x%016llX",
            FormatResult(rc).c_str(),
            state.monitor.session,
            static_cast<unsigned long long>(client_id));
        if (R_FAILED(rc)) {
            return FinishWithFailure(state, std::move(result), rc, "start_monitoring");
        }
    } else {
        logger::Log(ctx, "manual_bsd phase=start_monitoring_skipped");
    }

    if (config::ManualBsdCloneRootSession) {
        logger::Log(
            ctx,
            "manual_bsd phase=clone_root_begin root_handle=0x%X",
            state.root.session);
        rc = serviceClone(&state.root, &state.clone);
        LogService(ctx, "clone_root_complete", state.clone, rc);
        if (R_FAILED(rc)) {
            return FinishWithFailure(state, std::move(result), rc, "clone_root");
        }
    } else {
        logger::Log(ctx, "manual_bsd phase=clone_root_skipped");
    }

    const Result cleanup_rc = state.Cleanup();
    if (R_FAILED(cleanup_rc)) {
        result.rc = cleanup_rc;
        result.detail = "cleanup failed: " + FormatResult(cleanup_rc);
        return result;
    }

    result.success = true;
    result.detail =
        "register_client=" + std::to_string(config::ManualBsdRegisterClient) +
        " client_id=" + std::to_string(client_id) +
        " monitor=" + std::to_string(config::ManualBsdStartMonitoring) +
        " clone=" + std::to_string(config::ManualBsdCloneRootSession);
    return result;
}

} // namespace requester
