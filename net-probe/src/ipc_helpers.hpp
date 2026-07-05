#pragma once

#include <cstdint>
#include <cstddef>

#include <switch.h>

namespace wgnx::net_probe::ipc {

struct OptionalCopyHandle {
    Handle handle = INVALID_HANDLE;
    bool valid = false;
};

struct NetworkInterfaceCommandHandleSet {
    OptionalCopyHandle first = {};
    std::uint32_t scalar = 0;
    OptionalCopyHandle second = {};
    OptionalCopyHandle third = {};
};

Result InitializeSm();
void FinalizeSm();
Result OpenNamedService(Service *out_service, const char *name);
Result CreateDriverService(Service *driver_service_creator, Service *out_driver_service);
Result GetDriverInfo(Service *driver_service, std::uint64_t *out_driver_info);
Result GetNetworkInterfaceList(
    Service *driver_service,
    void *buffer,
    std::size_t buffer_size,
    std::uint32_t *out_entry_count);
Result GetStateChangedEvent(Service *driver_service, Handle *out_event_handle);
Result GetNetworkInterfaceListUpdatedEvent(Service *driver_service, Handle *out_event_handle);
Result OpenNetworkInterfaceRaw(
    Service *driver_service,
    const void *buffer,
    std::size_t buffer_size,
    Service *out_network_interface);
Result DuplicateNetworkInterface(Service *network_interface, Service *out_duplicate_interface);
Result BringUpNetworkInterface(Service *network_interface);
Result BringDownNetworkInterface(Service *network_interface);
Result StartCommunication(Service *network_interface);
Result StopCommunication(Service *network_interface);
Result GetNetworkInterfaceInfo(
    Service *network_interface,
    void *buffer,
    std::size_t buffer_size);
Result GetNetworkInterfaceStateChangedEvent(Service *network_interface, Handle *out_event_handle);
Result IoctlNetworkInterface(Service *network_interface, std::uint32_t selector);
Result IoctlGetHandleNetworkInterface(
    Service *network_interface,
    std::uint32_t selector,
    Handle *out_handle);
Result CallNetworkInterfaceCommand5(Service *network_interface);
Result CallNetworkInterfaceCommand6(Service *network_interface);
Result CallNetworkInterfaceCommand0x80(
    Service *network_interface,
    void *buffer,
    std::size_t buffer_size);
Result CallNetworkInterfaceCommand0x82(
    Service *network_interface,
    NetworkInterfaceCommandHandleSet *out_result);
Result CallNetworkInterfaceCommand0x83(
    Service *network_interface,
    NetworkInterfaceCommandHandleSet *out_result);
Result CreateUserService(Service *bsd_nu, Service *out_user_service);
Result AssignNetworkInterface(
    Service *user_service,
    Handle network_interface_handle,
    Service *out_assigned_interface);
Result AddSession(Service *assigned_interface, Handle network_interface_handle);
void CloseService(Service *service);
void LogServiceState(const char *label, const Service &service);

} // namespace wgnx::net_probe::ipc
