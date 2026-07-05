#include "probe.hpp"

#include <array>
#include <cstdio>

#include <switch.h>

#include "ipc_helpers.hpp"
#include "logger.hpp"

namespace wgnx::net_probe {

namespace {

using DriverProbeFn = Result (*)(Service *driver_service, void *context);
using InterfaceExperimentFn = void (*)(const char *service_name, Service *bsd_nu, Service *network_interface);
constexpr std::size_t kNetworkInterfaceInfoSize = 0xB0;
constexpr std::size_t kOpenNetworkInterfaceKeySize = 0x10;
constexpr std::size_t kMaxCapturedInterfaces = 4;
constexpr std::size_t kAssignAdmissionOffset = 0xA8;

struct DriverMetadataSnapshot {
    bool have_driver_info = false;
    std::uint64_t driver_info = 0;
    bool have_interface_list = false;
    std::uint32_t interface_count = 0;
    std::array<std::uint8_t, kNetworkInterfaceInfoSize * kMaxCapturedInterfaces> interface_list = {};
};

struct DriverMetadataContext {
    const char *service_name;
    DriverMetadataSnapshot *snapshot;
};

struct OpenInterfaceProbeContext {
    const char *service_name;
    const DriverMetadataSnapshot *snapshot;
    Service *out_network_interface;
};

struct InternetConnectionSnapshot {
    Result rc = 0;
    bool queried = false;
    NifmInternetConnectionType connection_type = static_cast<NifmInternetConnectionType>(0);
    u32 wifi_strength = 0;
    NifmInternetConnectionStatus connection_status = static_cast<NifmInternetConnectionStatus>(0);
};

void LogResultParts(const char *scope, const char *label, const Result rc) {
    logger::Log(
        "[%s] %s detail: module=%u description=%u value=0x%06X",
        scope,
        label,
        R_MODULE(rc),
        R_DESCRIPTION(rc),
        R_VALUE(rc));
}

void LogBufferPreview(
    const char *scope,
    const char *label,
    const std::uint8_t *buffer,
    const std::size_t buffer_size) {
    if (buffer == nullptr || buffer_size == 0) {
        return;
    }

    logger::Log("[%s] %s preview (%zu bytes total)", scope, label, buffer_size);

    constexpr std::size_t kBytesPerLine = 16;
    const std::size_t preview_size = (buffer_size < 64) ? buffer_size : 64;
    for (std::size_t offset = 0; offset < preview_size; offset += kBytesPerLine) {
        const std::size_t line_size =
            ((preview_size - offset) < kBytesPerLine) ? (preview_size - offset) : kBytesPerLine;

        char line[(kBytesPerLine * 3) + 1] = {};
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < line_size; ++i) {
            const int written = std::snprintf(
                line + cursor,
                sizeof(line) - cursor,
                (i == 0) ? "%02X" : " %02X",
                buffer[offset + i]);
            if (written <= 0) {
                break;
            }
            cursor += static_cast<std::size_t>(written);
            if (cursor >= sizeof(line)) {
                break;
            }
        }

        logger::Log("[%s] %s +0x%02zX: %s", scope, label, offset, line);
    }
}

void ProbeInternetConnection(InternetConnectionSnapshot *snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    *snapshot = {};

    const Result init_rc = nifmInitialize(NifmServiceType_Admin);
    logger::Log("nifmInitialize(Admin) -> 0x%08X", init_rc);
    if (R_FAILED(init_rc)) {
        snapshot->rc = init_rc;
        return;
    }

    snapshot->queried = true;
    snapshot->rc = nifmGetInternetConnectionStatus(
        &snapshot->connection_type,
        &snapshot->wifi_strength,
        &snapshot->connection_status);
    logger::Log("nifmGetInternetConnectionStatus -> 0x%08X", snapshot->rc);
    if (R_SUCCEEDED(snapshot->rc)) {
        logger::Log(
            "nifm status: type=%u wifi_strength=%u status=%u",
            static_cast<unsigned>(snapshot->connection_type),
            snapshot->wifi_strength,
            static_cast<unsigned>(snapshot->connection_status));
    }

    nifmExit();
    logger::Log("nifmExit");
}

void ProbeDriverMetadata(
    Service *driver_service,
    const char *service_name,
    DriverMetadataSnapshot *snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    *snapshot = {};

    logger::Log("[%s] Probing driver metadata", service_name);

    std::uint64_t driver_info = 0;
    Result rc = ipc::GetDriverInfo(driver_service, &driver_info);
    snapshot->have_driver_info = R_SUCCEEDED(rc);
    snapshot->driver_info = driver_info;
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "GetDriverInfo", rc);
    }

    std::uint32_t interface_count = 0;
    rc = ipc::GetNetworkInterfaceList(
        driver_service,
        snapshot->interface_list.data(),
        snapshot->interface_list.size(),
        &interface_count);
    snapshot->have_interface_list = R_SUCCEEDED(rc);
    snapshot->interface_count = interface_count;
    if (R_SUCCEEDED(rc)) {
        logger::Log(
            "[%s] GetNetworkInterfaceList capacity=%zu entries (%zu bytes each) emitted=%u",
            service_name,
            snapshot->interface_list.size() / kNetworkInterfaceInfoSize,
            kNetworkInterfaceInfoSize,
            interface_count);

        const std::size_t emitted_bytes =
            static_cast<std::size_t>(interface_count) * kNetworkInterfaceInfoSize;
        const std::size_t preview_size =
            (emitted_bytes < snapshot->interface_list.size()) ? emitted_bytes : snapshot->interface_list.size();
        if (preview_size > 0) {
            LogBufferPreview(
                service_name,
                "GetNetworkInterfaceList",
                snapshot->interface_list.data(),
                preview_size);
        } else {
            logger::Log("[%s] GetNetworkInterfaceList returned no interface records", service_name);
        }
    } else {
        LogResultParts(service_name, "GetNetworkInterfaceList", rc);
    }

    Handle state_changed_event = INVALID_HANDLE;
    rc = ipc::GetStateChangedEvent(driver_service, &state_changed_event);
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "GetStateChangedEvent", rc);
    } else if (state_changed_event != INVALID_HANDLE) {
        svcCloseHandle(state_changed_event);
    }

    Handle list_updated_event = INVALID_HANDLE;
    rc = ipc::GetNetworkInterfaceListUpdatedEvent(driver_service, &list_updated_event);
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "GetNetworkInterfaceListUpdatedEvent", rc);
    } else if (list_updated_event != INVALID_HANDLE) {
        svcCloseHandle(list_updated_event);
    }
}

Result ProbeDriverMetadataEntry(Service *driver_service, void *context) {
    auto *probe_context = static_cast<DriverMetadataContext *>(context);
    if (probe_context == nullptr || probe_context->service_name == nullptr || probe_context->snapshot == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    ProbeDriverMetadata(driver_service, probe_context->service_name, probe_context->snapshot);
    return 0;
}

Result TryOpenNetworkInterfacesFromSnapshot(
    Service *driver_service,
    const char *service_name,
    const DriverMetadataSnapshot &snapshot,
    Service *out_network_interface) {
    if (!snapshot.have_interface_list) {
        logger::Log("[%s] Skipping OpenNetworkInterface: interface list unavailable", service_name);
        return 0;
    }

    if (snapshot.interface_count == 0) {
        logger::Log("[%s] No interface records available for OpenNetworkInterface", service_name);
        return 0;
    }

    const std::size_t available_records = snapshot.interface_list.size() / kNetworkInterfaceInfoSize;
    const std::size_t probe_count =
        (snapshot.interface_count < available_records) ? snapshot.interface_count : available_records;

    Result rc = 0;
    for (std::size_t index = 0; index < probe_count; ++index) {
        const std::size_t record_offset = index * kNetworkInterfaceInfoSize;
        const std::uint8_t *record = snapshot.interface_list.data() + record_offset;

        logger::Log(
            "[%s] Trying OpenNetworkInterface with record %zu/%zu",
            service_name,
            index + 1,
            probe_count);
        LogBufferPreview(service_name, "OpenNetworkInterface key", record, kOpenNetworkInterfaceKeySize);

        rc = ipc::OpenNetworkInterfaceRaw(
            driver_service,
            record,
            kOpenNetworkInterfaceKeySize,
            out_network_interface);
        if (R_SUCCEEDED(rc)) {
            return rc;
        }

        LogResultParts(service_name, "OpenNetworkInterface", rc);
    }

    return rc;
}

Result ProbeOpenNetworkInterfaceEntry(Service *driver_service, void *context) {
    auto *probe_context = static_cast<OpenInterfaceProbeContext *>(context);
    if (probe_context == nullptr || probe_context->service_name == nullptr ||
        probe_context->snapshot == nullptr || probe_context->out_network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    return TryOpenNetworkInterfacesFromSnapshot(
        driver_service,
        probe_context->service_name,
        *probe_context->snapshot,
        probe_context->out_network_interface);
}

void TryAssignWithBsd(
    const char *service_name,
    Service *bsd_nu,
    Service *network_interface);

void LogOptionalCopyHandle(
    const char *service_name,
    const char *label,
    const ipc::OptionalCopyHandle &handle_info) {
    logger::Log(
        "[%s] %s: handle=0x%08X valid=%u",
        service_name,
        label,
        handle_info.handle,
        static_cast<unsigned>(handle_info.valid));
}

void CloseOptionalCopyHandle(ipc::OptionalCopyHandle *handle_info) {
    if (handle_info == nullptr || !handle_info->valid || handle_info->handle == INVALID_HANDLE) {
        return;
    }

    svcCloseHandle(handle_info->handle);
    handle_info->handle = INVALID_HANDLE;
    handle_info->valid = false;
}

void LogCommandHandleSet(
    const char *service_name,
    const char *label,
    const ipc::NetworkInterfaceCommandHandleSet &result) {
    logger::Log("[%s] %s scalar=0x%08X", service_name, label, result.scalar);
    LogOptionalCopyHandle(service_name, "first", result.first);
    LogOptionalCopyHandle(service_name, "second", result.second);
    LogOptionalCopyHandle(service_name, "third", result.third);
}

void CloseCommandHandleSet(ipc::NetworkInterfaceCommandHandleSet *result) {
    if (result == nullptr) {
        return;
    }

    CloseOptionalCopyHandle(&result->first);
    CloseOptionalCopyHandle(&result->second);
    CloseOptionalCopyHandle(&result->third);
    result->scalar = 0;
}

void LogAssignAdmissionByte(
    const char *service_name,
    const std::uint8_t *descriptor,
    const std::size_t descriptor_size) {
    if (descriptor == nullptr || descriptor_size <= kAssignAdmissionOffset) {
        logger::Log("[%s] Command0x80 descriptor too small for byte 0x%zX", service_name, kAssignAdmissionOffset);
        return;
    }

    logger::Log(
        "[%s] Command0x80 descriptor[0x%zX] = 0x%02X",
        service_name,
        kAssignAdmissionOffset,
        descriptor[kAssignAdmissionOffset]);

    if (descriptor_size >= 0xB0) {
        char line[(16 * 3) + 1] = {};
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t offset = 0xA0 + i;
            const int written = std::snprintf(
                line + cursor,
                sizeof(line) - cursor,
                (i == 0) ? "%02X" : " %02X",
                descriptor[offset]);
            if (written <= 0) {
                break;
            }

            cursor += static_cast<std::size_t>(written);
            if (cursor >= sizeof(line)) {
                break;
            }
        }

        logger::Log("[%s] Command0x80 descriptor[0xA0..0xAF]: %s", service_name, line);
    }
}

bool RunTargetedAssignPreflight(
    const char *service_name,
    Service *network_interface,
    const char *label_prefix) {
    if (service_name == nullptr || network_interface == nullptr || !serviceIsActive(network_interface)) {
        return false;
    }

    logger::Log("[%s] %s: targeted preflight 0x82 -> 0x83 -> 5 -> 0x80", service_name, label_prefix);

    ipc::NetworkInterfaceCommandHandleSet command_82 = {};
    ipc::NetworkInterfaceCommandHandleSet command_83 = {};
    std::array<std::uint8_t, kNetworkInterfaceInfoSize> descriptor = {};
    bool command_5_succeeded = false;
    bool success = false;

    Result rc = ipc::CallNetworkInterfaceCommand0x82(network_interface, &command_82);
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "Command0x82", rc);
        goto cleanup;
    }
    LogCommandHandleSet(service_name, "Command0x82", command_82);

    rc = ipc::CallNetworkInterfaceCommand0x83(network_interface, &command_83);
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "Command0x83", rc);
        goto cleanup;
    }
    LogCommandHandleSet(service_name, "Command0x83", command_83);

    rc = ipc::CallNetworkInterfaceCommand5(network_interface);
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "Command5", rc);
        goto cleanup;
    }
    command_5_succeeded = true;

    rc = ipc::CallNetworkInterfaceCommand0x80(network_interface, descriptor.data(), descriptor.size());
    if (R_FAILED(rc)) {
        LogResultParts(service_name, "Command0x80", rc);
        goto cleanup;
    }

    LogAssignAdmissionByte(service_name, descriptor.data(), descriptor.size());
    success = true;

cleanup:
    if (command_5_succeeded) {
        const Result cleanup_rc = ipc::CallNetworkInterfaceCommand6(network_interface);
        if (R_FAILED(cleanup_rc)) {
            LogResultParts(service_name, "Command6(cleanup)", cleanup_rc);
        }
    }

    CloseCommandHandleSet(&command_83);
    CloseCommandHandleSet(&command_82);
    return success;
}

void ExperimentAssignOnly(
    const char *service_name,
    Service *bsd_nu,
    Service *network_interface) {
    logger::Log("[%s] Experiment: assign-only", service_name);
    TryAssignWithBsd(service_name, bsd_nu, network_interface);
}

void ExperimentTargetedPreflightOnly(
    const char *service_name,
    Service *,
    Service *network_interface) {
    logger::Log("[%s] Experiment: targeted-preflight-only", service_name);
    static_cast<void>(RunTargetedAssignPreflight(service_name, network_interface, "targeted-preflight-only"));
}

void ExperimentTargetedPreflightThenAssign(
    const char *service_name,
    Service *bsd_nu,
    Service *network_interface) {
    logger::Log("[%s] Experiment: targeted-preflight-then-assign", service_name);
    const bool preflight_ok = RunTargetedAssignPreflight(
        service_name,
        network_interface,
        "targeted-preflight-then-assign");
    if (!preflight_ok) {
        logger::Log("[%s] targeted preflight failed before assign", service_name);
        return;
    }

    TryAssignWithBsd(service_name, bsd_nu, network_interface);
}

Result RunWithFreshDriverService(
    Service *driver_service_creator,
    const char *service_name,
    const char *label,
    DriverProbeFn probe_fn,
    void *context) {
    Service driver_service = {};
    const Result create_rc = ipc::CreateDriverService(driver_service_creator, &driver_service);
    if (R_FAILED(create_rc)) {
        logger::Log("[%s] %s: CreateDriverService failed", service_name, label);
        LogResultParts(service_name, label, create_rc);
        return create_rc;
    }

    const Result probe_rc = probe_fn(&driver_service, context);
    if (R_FAILED(probe_rc)) {
        LogResultParts(service_name, label, probe_rc);
    }

    ipc::CloseService(&driver_service);
    return probe_rc;
}

Result OpenFreshNetworkInterface(
    Service *driver_service_creator,
    const char *service_name,
    const DriverMetadataSnapshot *snapshot,
    Service *out_network_interface) {
    if (driver_service_creator == nullptr || service_name == nullptr || snapshot == nullptr ||
        out_network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_network_interface = {};
    OpenInterfaceProbeContext open_context = {
        .service_name = service_name,
        .snapshot = snapshot,
        .out_network_interface = out_network_interface,
    };
    return RunWithFreshDriverService(
        driver_service_creator,
        service_name,
        "OpenNetworkInterface",
        &ProbeOpenNetworkInterfaceEntry,
        &open_context);
}

void RunFreshNetworkInterfaceExperiment(
    Service *driver_service_creator,
    const char *service_name,
    const DriverMetadataSnapshot *snapshot,
    Service *bsd_nu,
    const char *experiment_label,
    InterfaceExperimentFn experiment_fn) {
    if (driver_service_creator == nullptr || service_name == nullptr || snapshot == nullptr ||
        experiment_label == nullptr || experiment_fn == nullptr) {
        return;
    }

    Service network_interface = {};
    const Result open_rc = OpenFreshNetworkInterface(
        driver_service_creator,
        service_name,
        snapshot,
        &network_interface);
    if (R_FAILED(open_rc) || !serviceIsActive(&network_interface)) {
        logger::Log("[%s] Experiment '%s': fresh open failed", service_name, experiment_label);
        if (R_FAILED(open_rc)) {
            LogResultParts(service_name, experiment_label, open_rc);
        }
        return;
    }

    experiment_fn(service_name, bsd_nu, &network_interface);
    ipc::CloseService(&network_interface);
}

void TryAssignWithBsd(
    const char *service_name,
    Service *bsd_nu,
    Service *network_interface) {
    if (bsd_nu == nullptr || !serviceIsActive(bsd_nu) || network_interface == nullptr ||
        !serviceIsActive(network_interface)) {
        return;
    }

    logger::Log("[%s] Attempting bsd:nu assignment", service_name);

    Service bsd_user = {};
    Service assigned_interface = {};

    const Result user_rc = ipc::CreateUserService(bsd_nu, &bsd_user);
    if (R_FAILED(user_rc)) {
        LogResultParts(service_name, "CreateUserService", user_rc);
        return;
    }

    const Result assign_rc = ipc::AssignNetworkInterface(
        &bsd_user,
        network_interface->session,
        &assigned_interface);
    if (R_FAILED(assign_rc)) {
        LogResultParts(service_name, "AssignNetworkInterface", assign_rc);
    } else {
        const Result add_session_rc = ipc::AddSession(&assigned_interface, network_interface->session);
        if (R_FAILED(add_session_rc)) {
            LogResultParts(service_name, "AddSession", add_session_rc);
        }
    }

    ipc::CloseService(&assigned_interface);
    ipc::CloseService(&bsd_user);
}

Result ProbeDriverService(
    const char *service_name,
    Service *bsd_nu,
    const InternetConnectionSnapshot *) {
    Service driver_service_creator = {};

    const Result open_rc = ipc::OpenNamedService(&driver_service_creator, service_name);
    if (R_FAILED(open_rc)) {
        LogResultParts(service_name, "smGetService", open_rc);
        return open_rc;
    }

    DriverMetadataSnapshot metadata = {};
    DriverMetadataContext metadata_context = {
        .service_name = service_name,
        .snapshot = &metadata,
    };
    static_cast<void>(RunWithFreshDriverService(
        &driver_service_creator,
        service_name,
        "ProbeDriverMetadata",
        &ProbeDriverMetadataEntry,
        &metadata_context));

    logger::Log("[%s] Running targeted fresh-session experiments", service_name);
    RunFreshNetworkInterfaceExperiment(
        &driver_service_creator,
        service_name,
        &metadata,
        bsd_nu,
        "targeted-preflight-only",
        &ExperimentTargetedPreflightOnly);
    RunFreshNetworkInterfaceExperiment(
        &driver_service_creator,
        service_name,
        &metadata,
        bsd_nu,
        "targeted-preflight-then-assign",
        &ExperimentTargetedPreflightThenAssign);
    RunFreshNetworkInterfaceExperiment(
        &driver_service_creator,
        service_name,
        &metadata,
        bsd_nu,
        "assign-only",
        &ExperimentAssignOnly);

    ipc::CloseService(&driver_service_creator);
    return open_rc;
}

} // namespace

void RunProbe() {
    const u32 hosversion = hosversionGet();
    logger::Log(
        "HOS %u.%u.%u%s",
        (hosversion >> 16) & 0xFF,
        (hosversion >> 8) & 0xFF,
        hosversion & 0xFF,
        hosversionIsAtmosphere() ? " | AMS" : "");

    const Result sm_rc = ipc::InitializeSm();
    if (R_FAILED(sm_rc)) {
        logger::Log("Aborting probe: SM init failed");
        return;
    }

    InternetConnectionSnapshot internet_snapshot = {};
    ProbeInternetConnection(&internet_snapshot);

    Service bsd_nu = {};
    const Result bsd_rc = ipc::OpenNamedService(&bsd_nu, "bsd:nu");

    const Result eth_rc = ProbeDriverService(
        "eth:nd",
        R_SUCCEEDED(bsd_rc) ? &bsd_nu : nullptr,
        &internet_snapshot);
    const Result wlan_rc = ProbeDriverService(
        "wlan:nd",
        R_SUCCEEDED(bsd_rc) ? &bsd_nu : nullptr,
        &internet_snapshot);

    logger::Log("Probe summary: eth:nd=0x%08X wlan:nd=0x%08X bsd:nu=0x%08X", eth_rc, wlan_rc, bsd_rc);

    ipc::CloseService(&bsd_nu);
    ipc::FinalizeSm();
}

} // namespace wgnx::net_probe
