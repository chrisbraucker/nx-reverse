#pragma once

#include <stratosphere.hpp>
#include <stratosphere/sf/hipc/sf_hipc_mitm_monitor.hpp>

namespace wgnx::net_probe::mitm_trace {

void Initialize();
u64 AllocateSessionId();
void LogSmLifecycle(const ams::sf::hipc::mitm_monitor::SmLifecycleTraceContext &ctx);
void LogAcceptTrace(const ams::sf::hipc::mitm_monitor::AcceptTraceContext &ctx);
void LogDispatchTrace(const ams::sf::hipc::mitm_monitor::DispatchTraceContext &ctx);
void LogSessionTrace(const ams::sf::hipc::mitm_monitor::SessionTraceContext &ctx);
void LogSessionConnected(const ams::sm::ServiceName &service_name, u64 session_id, const ams::sm::MitmProcessInfo &client_info);
void LogSessionAcceptFailure(const ams::sm::ServiceName &service_name, const ams::sm::MitmProcessInfo &client_info, ams::Result result);
size_t GetTrackedSessionCount();
void LogForwardServiceState(
    const ams::sm::ServiceName &service_name,
    const ams::sm::MitmProcessInfo &client_info,
    u64 session_id,
    const char *phase,
    ams::os::NativeHandle handle,
    bool own_handle,
    u32 object_id,
    u32 pointer_buffer_size);
void LogSessionSnapshot(const char *phase);
void LogDomainSnapshot(const char *phase);
void LogDomainSnapshotForSession(u64 session_id, const char *phase);
void RegisterMonitorHooks();

} // namespace wgnx::net_probe::mitm_trace
