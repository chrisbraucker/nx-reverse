#include "ipc_helpers.hpp"

#include "logger.hpp"

namespace wgnx::net_probe::ipc {

namespace {

constexpr std::uint32_t InAutoBuffer = SfBufferAttr_HipcAutoSelect | SfBufferAttr_In;
constexpr std::uint32_t OutAutoBuffer = SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out;

void ResetOptionalCopyHandle(OptionalCopyHandle *out_handle) {
    if (out_handle == nullptr) {
        return;
    }

    out_handle->handle = INVALID_HANDLE;
    out_handle->valid = false;
}

void LogResult(const char *label, const Result rc) {
    logger::Log("%s -> 0x%08X", label, rc);
}

void LogHandleValue(const char *label, const Handle handle) {
    logger::Log("%s: handle=0x%08X", label, handle);
}

} // namespace

Result InitializeSm() {
    const Result rc = smInitialize();
    logger::Log("smInitialize -> 0x%08X", rc);
    return rc;
}

void FinalizeSm() {
    logger::Log("smExit");
    smExit();
}

Result OpenNamedService(Service *out_service, const char *name) {
    if (out_service == nullptr || name == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_service = {};
    const Result rc = smGetService(out_service, name);
    logger::Log("smGetService('%s') -> 0x%08X", name, rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState(name, *out_service);
    }
    return rc;
}

Result CreateDriverService(Service *driver_service_creator, Service *out_driver_service) {
    if (driver_service_creator == nullptr || out_driver_service == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_driver_service = {};
    const Result rc = serviceDispatch(
        driver_service_creator,
        0,
        .out_num_objects = 1,
        .out_objects = out_driver_service);
    LogResult("ISfDriverServiceCreator::CreateDriverService", rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState("driver service", *out_driver_service);
    }
    return rc;
}

Result GetDriverInfo(Service *driver_service, std::uint64_t *out_driver_info) {
    if (driver_service == nullptr || out_driver_info == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    std::uint64_t driver_info = 0;
    const Result rc = serviceDispatchOut(driver_service, 128, driver_info);
    logger::Log("ISfDriverService::GetDriverInfo -> 0x%08X", rc);
    if (R_SUCCEEDED(rc)) {
        *out_driver_info = driver_info;
        logger::Log("ISfDriverService::GetDriverInfo output: 0x%016llX", static_cast<unsigned long long>(driver_info));
    }
    return rc;
}

Result GetNetworkInterfaceList(
    Service *driver_service,
    void *buffer,
    std::size_t buffer_size,
    std::uint32_t *out_entry_count) {
    if (driver_service == nullptr || buffer == nullptr || buffer_size == 0 || out_entry_count == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    std::uint32_t entry_count = 0;
    const Result rc = serviceDispatchOut(
        driver_service,
        129,
        entry_count,
        .buffer_attrs = { OutAutoBuffer },
        .buffers = { { buffer, buffer_size } });
    logger::Log("ISfDriverService::GetNetworkInterfaceList(size=0x%zX) -> 0x%08X", buffer_size, rc);
    if (R_SUCCEEDED(rc)) {
        *out_entry_count = entry_count;
        logger::Log("ISfDriverService::GetNetworkInterfaceList output count: %u", entry_count);
    }
    return rc;
}

Result GetStateChangedEvent(Service *driver_service, Handle *out_event_handle) {
    if (driver_service == nullptr || out_event_handle == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_event_handle = INVALID_HANDLE;
    const Result rc = serviceDispatch(
        driver_service,
        130,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = out_event_handle);
    LogResult("ISfDriverService::GetStateChangedEvent", rc);
    if (R_SUCCEEDED(rc)) {
        LogHandleValue("ISfDriverService::GetStateChangedEvent output", *out_event_handle);
    }
    return rc;
}

Result GetNetworkInterfaceListUpdatedEvent(Service *driver_service, Handle *out_event_handle) {
    if (driver_service == nullptr || out_event_handle == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_event_handle = INVALID_HANDLE;
    const Result rc = serviceDispatch(
        driver_service,
        131,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = out_event_handle);
    LogResult("ISfDriverService::GetNetworkInterfaceListUpdatedEvent", rc);
    if (R_SUCCEEDED(rc)) {
        LogHandleValue("ISfDriverService::GetNetworkInterfaceListUpdatedEvent output", *out_event_handle);
    }
    return rc;
}

Result OpenNetworkInterfaceRaw(
    Service *driver_service,
    const void *buffer,
    std::size_t buffer_size,
    Service *out_network_interface) {
    if (driver_service == nullptr || out_network_interface == nullptr || buffer == nullptr || buffer_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_network_interface = {};
    const Result rc = serviceDispatch(
        driver_service,
        0,
        .buffer_attrs = { InAutoBuffer },
        .buffers = { { buffer, buffer_size } },
        .out_num_objects = 1,
        .out_objects = out_network_interface);
    logger::Log("ISfDriverService::OpenNetworkInterface(size=0x%zX) -> 0x%08X", buffer_size, rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState("network interface service", *out_network_interface);
    }
    return rc;
}

Result DuplicateNetworkInterface(Service *network_interface, Service *out_duplicate_interface) {
    if (network_interface == nullptr || out_duplicate_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_duplicate_interface = {};
    const Result rc = serviceDispatch(
        network_interface,
        0,
        .out_num_objects = 1,
        .out_objects = out_duplicate_interface);
    LogResult("ISfNetworkInterfaceService::Duplicate", rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState("duplicated network interface service", *out_duplicate_interface);
    }
    return rc;
}

Result BringUpNetworkInterface(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 1);
    LogResult("ISfNetworkInterfaceService::BringUp", rc);
    return rc;
}

Result BringDownNetworkInterface(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 2);
    LogResult("ISfNetworkInterfaceService::BringDown", rc);
    return rc;
}

Result StartCommunication(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 3);
    LogResult("ISfNetworkInterfaceService::StartCommunication", rc);
    return rc;
}

Result StopCommunication(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 4);
    LogResult("ISfNetworkInterfaceService::StopCommunication", rc);
    return rc;
}

Result GetNetworkInterfaceInfo(
    Service *network_interface,
    void *buffer,
    std::size_t buffer_size) {
    if (network_interface == nullptr || buffer == nullptr || buffer_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(
        network_interface,
        128,
        .buffer_attrs = { OutAutoBuffer },
        .buffers = { { buffer, buffer_size } });
    logger::Log(
        "ISfNetworkInterfaceService::GetNetworkInterfaceInfo(size=0x%zX) -> 0x%08X",
        buffer_size,
        rc);
    return rc;
}

Result GetNetworkInterfaceStateChangedEvent(Service *network_interface, Handle *out_event_handle) {
    if (network_interface == nullptr || out_event_handle == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_event_handle = INVALID_HANDLE;
    const Result rc = serviceDispatch(
        network_interface,
        129,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = out_event_handle);
    LogResult("ISfNetworkInterfaceService::GetStateChangedEvent", rc);
    if (R_SUCCEEDED(rc)) {
        LogHandleValue("ISfNetworkInterfaceService::GetStateChangedEvent output", *out_event_handle);
    }
    return rc;
}

Result IoctlNetworkInterface(Service *network_interface, std::uint32_t selector) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatchIn(network_interface, 384, selector);
    logger::Log(
        "ISfNetworkInterfaceService::Ioctl(selector=0x%08X) -> 0x%08X",
        selector,
        rc);
    return rc;
}

Result IoctlGetHandleNetworkInterface(
    Service *network_interface,
    std::uint32_t selector,
    Handle *out_handle) {
    if (network_interface == nullptr || out_handle == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_handle = INVALID_HANDLE;
    const Result rc = serviceDispatchIn(
        network_interface,
        389,
        selector,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = out_handle);
    logger::Log(
        "ISfNetworkInterfaceService::IoctlGetHandle(selector=0x%08X) -> 0x%08X",
        selector,
        rc);
    if (R_SUCCEEDED(rc)) {
        LogHandleValue("ISfNetworkInterfaceService::IoctlGetHandle output", *out_handle);
    }
    return rc;
}

Result CallNetworkInterfaceCommand5(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 5);
    LogResult("ISfNetworkInterfaceService::Command5", rc);
    return rc;
}

Result CallNetworkInterfaceCommand6(Service *network_interface) {
    if (network_interface == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(network_interface, 6);
    LogResult("ISfNetworkInterfaceService::Command6", rc);
    return rc;
}

Result CallNetworkInterfaceCommand0x80(
    Service *network_interface,
    void *buffer,
    std::size_t buffer_size) {
    if (network_interface == nullptr || buffer == nullptr || buffer_size == 0) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const Result rc = serviceDispatch(
        network_interface,
        0x80,
        .buffer_attrs = { OutAutoBuffer },
        .buffers = { { buffer, buffer_size } });
    logger::Log(
        "ISfNetworkInterfaceService::Command0x80(size=0x%zX) -> 0x%08X",
        buffer_size,
        rc);
    return rc;
}

Result CallNetworkInterfaceCommand0x82(
    Service *network_interface,
    NetworkInterfaceCommandHandleSet *out_result) {
    if (network_interface == nullptr || out_result == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_result = {};
    ResetOptionalCopyHandle(&out_result->first);
    ResetOptionalCopyHandle(&out_result->second);
    ResetOptionalCopyHandle(&out_result->third);

    std::uint32_t scalar = 0;
    Handle handles[8] = {
        INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE,
        INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE,
    };

    const Result rc = serviceDispatchOut(
        network_interface,
        0x82,
        scalar,
        .out_handle_attrs = {
            SfOutHandleAttr_HipcCopy,
            SfOutHandleAttr_HipcCopy,
            SfOutHandleAttr_HipcCopy,
        },
        .out_handles = handles);
    logger::Log("ISfNetworkInterfaceService::Command0x82 -> 0x%08X", rc);
    if (R_SUCCEEDED(rc)) {
        out_result->scalar = scalar;
        out_result->first.handle = handles[0];
        out_result->first.valid = handles[0] != INVALID_HANDLE;
        out_result->second.handle = handles[1];
        out_result->second.valid = handles[1] != INVALID_HANDLE;
        out_result->third.handle = handles[2];
        out_result->third.valid = handles[2] != INVALID_HANDLE;
        logger::Log(
            "ISfNetworkInterfaceService::Command0x82 output: scalar=0x%08X first=0x%08X(%u) second=0x%08X(%u) third=0x%08X(%u)",
            out_result->scalar,
            out_result->first.handle,
            static_cast<unsigned>(out_result->first.valid),
            out_result->second.handle,
            static_cast<unsigned>(out_result->second.valid),
            out_result->third.handle,
            static_cast<unsigned>(out_result->third.valid));
    }
    return rc;
}

Result CallNetworkInterfaceCommand0x83(
    Service *network_interface,
    NetworkInterfaceCommandHandleSet *out_result) {
    if (network_interface == nullptr || out_result == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_result = {};
    ResetOptionalCopyHandle(&out_result->first);
    ResetOptionalCopyHandle(&out_result->second);
    ResetOptionalCopyHandle(&out_result->third);

    std::uint32_t scalar = 0;
    Handle handles[8] = {
        INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE,
        INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE,
    };

    const Result rc = serviceDispatchOut(
        network_interface,
        0x83,
        scalar,
        .out_handle_attrs = {
            SfOutHandleAttr_HipcCopy,
            SfOutHandleAttr_HipcCopy,
            SfOutHandleAttr_HipcCopy,
        },
        .out_handles = handles);
    logger::Log("ISfNetworkInterfaceService::Command0x83 -> 0x%08X", rc);
    if (R_SUCCEEDED(rc)) {
        out_result->scalar = scalar;
        out_result->first.handle = handles[0];
        out_result->first.valid = handles[0] != INVALID_HANDLE;
        out_result->second.handle = handles[1];
        out_result->second.valid = handles[1] != INVALID_HANDLE;
        out_result->third.handle = handles[2];
        out_result->third.valid = handles[2] != INVALID_HANDLE;
        logger::Log(
            "ISfNetworkInterfaceService::Command0x83 output: scalar=0x%08X first=0x%08X(%u) second=0x%08X(%u) third=0x%08X(%u)",
            out_result->scalar,
            out_result->first.handle,
            static_cast<unsigned>(out_result->first.valid),
            out_result->second.handle,
            static_cast<unsigned>(out_result->second.valid),
            out_result->third.handle,
            static_cast<unsigned>(out_result->third.valid));
    }
    return rc;
}

Result CreateUserService(Service *bsd_nu, Service *out_user_service) {
    if (bsd_nu == nullptr || out_user_service == nullptr) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_user_service = {};
    const Result rc = serviceDispatch(
        bsd_nu,
        0,
        .out_num_objects = 1,
        .out_objects = out_user_service);
    LogResult("bsd:nu::CreateUserService", rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState("bsd:nu user", *out_user_service);
    }
    return rc;
}

Result AssignNetworkInterface(
    Service *user_service,
    Handle network_interface_handle,
    Service *out_assigned_interface) {
    if (user_service == nullptr || out_assigned_interface == nullptr || network_interface_handle == INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    *out_assigned_interface = {};
    LogHandleValue("ISfUserService::Assign input", network_interface_handle);
    const Result rc = serviceDispatch(
        user_service,
        0,
        .in_num_handles = 1,
        .in_handles = { network_interface_handle },
        .out_num_objects = 1,
        .out_objects = out_assigned_interface);
    LogResult("ISfUserService::Assign", rc);
    if (R_SUCCEEDED(rc)) {
        LogServiceState("bsd assigned interface", *out_assigned_interface);
    }
    return rc;
}

Result AddSession(Service *assigned_interface, Handle network_interface_handle) {
    if (assigned_interface == nullptr || network_interface_handle == INVALID_HANDLE) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    LogHandleValue("ISfAssignedNetworkInterfaceService::AddSession input", network_interface_handle);
    const Result rc = serviceDispatch(
        assigned_interface,
        0,
        .in_num_handles = 1,
        .in_handles = { network_interface_handle });
    LogResult("ISfAssignedNetworkInterfaceService::AddSession", rc);
    return rc;
}

void CloseService(Service *service) {
    if (service == nullptr || !serviceIsActive(service)) {
        return;
    }

    logger::Log(
        "serviceClose(handle=0x%08X own=%u object=%u ptrbuf=%u)",
        service->session,
        service->own_handle,
        service->object_id,
        service->pointer_buffer_size);
    serviceClose(service);
}

void LogServiceState(const char *label, const Service &service) {
    logger::Log(
        "%s: handle=0x%08X own=%u object=%u ptrbuf=%u",
        label,
        service.session,
        service.own_handle,
        service.object_id,
        service.pointer_buffer_size);
}

} // namespace wgnx::net_probe::ipc
