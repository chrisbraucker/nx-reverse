#include "mitm_service.hpp"

#include <stratosphere.hpp>

#include "logger.hpp"
#include "mitm_trace.hpp"

#define WGNX_I_PASSIVE_MITM_INTERFACE_INFO(C, H)
AMS_SF_DEFINE_MITM_INTERFACE(wgnx::net_probe::mitm, IPassiveMitmService, WGNX_I_PASSIVE_MITM_INTERFACE_INFO, 0x57474D54);

#define WGNX_I_PROBE_CONTROL_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, ::ams::Result, Shutdown, (), (), ams::hos::Version_Min, ams::hos::Version_Max)
AMS_SF_DEFINE_INTERFACE(wgnx::net_probe::mitm, IProbeControlService, WGNX_I_PROBE_CONTROL_INTERFACE_INFO, 0x57475043);

namespace wgnx::net_probe::mitm {

constexpr ::ams::ncm::ProgramId WireguardProgramId{0x010000000000EAD0ul};
constexpr ::ams::ncm::ProgramId NetProbeProgramId{0x010000000000EAD1ul};
constexpr ::ams::ncm::ProgramId NpnsProgramId = ::ams::ncm::SystemProgramId::Npns;
constexpr ::ams::ncm::ProgramId EupldProgramId = ::ams::ncm::SystemProgramId::Eupld;
constexpr ::ams::ncm::ProgramId OlscProgramId = ::ams::ncm::SystemProgramId::Olsc;
constexpr ::ams::ncm::ProgramId QlaunchProgramId{0x0100000000001000ul};

namespace {

alignas(::ams::os::MemoryPageSize) constinit u8 g_mitm_object_heap[32 * 1024];
constinit ::ams::lmem::HeapHandle g_mitm_object_heap_handle = nullptr;
constinit ::ams::sf::ExpHeapMemoryResource g_mitm_object_memory_resource;
constinit ::ams::os::SdkMutex g_mitm_object_allocator_lock;
constinit ::ams::os::SdkMutex g_mitm_start_lock;
constinit bool g_mitm_object_allocator_ready = false;
constinit ::ams::os::EventType g_shutdown_requested_event = {};
constinit ::ams::os::EventType g_control_thread_ready_event = {};
constinit ::ams::os::EventType g_mitm_thread_ready_event = {};
constinit bool g_shutdown_event_initialized = false;
constinit bool g_thread_ready_events_initialized = false;
constinit bool g_shutdown_requested = false;

void EnsureMitmObjectAllocatorInitialized() {
    if (AMS_LIKELY(g_mitm_object_allocator_ready)) {
        return;
    }

    std::scoped_lock lk(g_mitm_object_allocator_lock);
    if (AMS_LIKELY(g_mitm_object_allocator_ready)) {
        return;
    }

    g_mitm_object_heap_handle = ::ams::lmem::CreateExpHeap(
        g_mitm_object_heap,
        sizeof(g_mitm_object_heap),
        ::ams::lmem::CreateOption_ThreadSafe);
    AMS_ABORT_UNLESS(g_mitm_object_heap_handle != nullptr);

    g_mitm_object_memory_resource.Attach(g_mitm_object_heap_handle);
    g_mitm_object_allocator_ready = true;
    wgnx::net_probe::logger::Log(
        "MITM object allocator ready: heap=%p size=0x%zx",
        g_mitm_object_heap,
        sizeof(g_mitm_object_heap));
}

class PassiveMitmService : public ::ams::sf::MitmServiceImplBase {
    public:
        PassiveMitmService(std::shared_ptr<::Service> &&forward_service, const ::ams::sm::MitmProcessInfo &client_info)
            : ::ams::sf::MitmServiceImplBase(std::move(forward_service), client_info) {
        }

        static bool IsSelfProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == WireguardProgramId || client_info.program_id == NetProbeProgramId;
        }

        static bool IsBsdSystemDenylistedProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == NpnsProgramId
                || client_info.program_id == EupldProgramId
                || client_info.program_id == OlscProgramId;
        }

        static bool IsBsdAdminDenylistedProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == QlaunchProgramId;
        }

        static bool ShouldMitmForService(const char *service_name, const ::ams::sm::MitmProcessInfo &client_info) {
            const bool should_mitm = !IsSelfProgram(client_info);
            wgnx::net_probe::logger::Log(
                "ShouldMitm(%s): pid=0x%016llx program_id=0x%016llx result=%u",
                service_name,
                static_cast<unsigned long long>(client_info.process_id.value),
                static_cast<unsigned long long>(client_info.program_id.value),
                should_mitm ? 1u : 0u);
            return should_mitm;
        }

        static bool ShouldMitmNifmUser(const ::ams::sm::MitmProcessInfo &client_info) {
            return ShouldMitmForService("nifm:u", client_info);
        }

        static bool ShouldMitmNifmSystem(const ::ams::sm::MitmProcessInfo &client_info) {
            return ShouldMitmForService("nifm:s", client_info);
        }

        static bool ShouldMitmBsdUser(const ::ams::sm::MitmProcessInfo &client_info) {
            return ShouldMitmForService("bsd:u", client_info);
        }

        static bool ShouldMitmBsdSystem(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            if (IsBsdSystemDenylistedProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            return ShouldMitmForService("bsd:s", client_info);
        }

        static bool ShouldMitmBsdAdmin(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(bsd:a): pid=0x%016llx program_id=0x%016llx skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            if (IsBsdAdminDenylistedProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(bsd:a): pid=0x%016llx program_id=0x%016llx skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            return ShouldMitmForService("bsd:a", client_info);
        }

        static bool ShouldMitmSslUser(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(ssl): pid=0x%016llx program_id=0x%016llx skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            if (IsBsdSystemDenylistedProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(ssl): pid=0x%016llx program_id=0x%016llx skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            return ShouldMitmForService("ssl", client_info);
        }

        static bool ShouldMitmSslSystem(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(ssl:s): pid=0x%016llx program_id=0x%016llx skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            if (IsBsdSystemDenylistedProgram(client_info)) {
                wgnx::net_probe::logger::Log(
                    "ShouldMitm(ssl:s): pid=0x%016llx program_id=0x%016llx skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value));
                return false;
            }

            return ShouldMitmForService("ssl:s", client_info);
        }
};

class ProbeControlService {
    public:
        ::ams::Result Shutdown() {
            wgnx::net_probe::logger::Log("ProbeControlService::Shutdown requested");
            RequestShutdown();
            R_SUCCEED();
        }
};
static_assert(IsIProbeControlService<ProbeControlService>);

struct MitmServerOptions {
    static constexpr size_t PointerBufferSize = 0x1000;
    /*
     * Passive MITM sessions across nifm, bsd, and ssl all share one server manager.
     * Long-lived system sessions can consume multiple domain slots before userland
     * clients like sphaira connect, so keep the pool comfortably above the observed
     * boot-time baseline to avoid false transparency failures from ResultOutOfDomains.
     */
    static constexpr size_t MaxDomains = 32;
    static constexpr size_t MaxDomainObjects = 1024;
    static constexpr bool CanDeferInvokeRequest = false;
    static constexpr bool CanManageMitmServers = true;
};

enum PortIndex {
    PortIndex_NifmUser = 0,
    PortIndex_NifmSystem = 1,
    PortIndex_BsdUser = 2,
    PortIndex_BsdSystem = 3,
    PortIndex_BsdAdmin = 4,
    PortIndex_SslUser = 5,
    PortIndex_SslSystem = 6,
    PortIndex_Count = 7,
};

using ShouldMitmCallback = bool (*)(const ::ams::sm::MitmProcessInfo &);

struct MitmTargetConfig {
    const char *service_name;
    ShouldMitmCallback should_mitm;
    bool enabled;
};

constexpr MitmTargetConfig g_mitm_target_configs[PortIndex_Count] = {
    { "nifm:u", &PassiveMitmService::ShouldMitmNifmUser, true },
    { "nifm:s", &PassiveMitmService::ShouldMitmNifmSystem, true },
    { "bsd:u",  &PassiveMitmService::ShouldMitmBsdUser, true },
    { "bsd:s",  &PassiveMitmService::ShouldMitmBsdSystem, true },
    { "bsd:a",  &PassiveMitmService::ShouldMitmBsdAdmin, false },
    { "ssl",    &PassiveMitmService::ShouldMitmSslUser, true },
    { "ssl:s",  &PassiveMitmService::ShouldMitmSslSystem, true },
};

constexpr size_t NumSessions = 48;
constexpr size_t ControlMaxSessions = 4;

class MitmServerManager final : public ::ams::sf::hipc::ServerManager<PortIndex_Count, MitmServerOptions, NumSessions> {
    private:
        static const MitmTargetConfig &GetTargetConfigForPortIndex(int port_index) {
            AMS_ABORT_UNLESS(port_index >= 0);
            AMS_ABORT_UNLESS(static_cast<size_t>(port_index) < std::size(g_mitm_target_configs));
            return g_mitm_target_configs[port_index];
        }

        static ::ams::sm::ServiceName GetServiceNameForPortIndex(int port_index) {
            return ::ams::sm::ServiceName::Encode(GetTargetConfigForPortIndex(port_index).service_name);
        }

        static const char *GetServiceNameTextForPortIndex(int port_index) {
            return GetTargetConfigForPortIndex(port_index).service_name;
        }

        ::ams::Result AcceptMitmConnection(int port_index, Server *server) {
            const char *service_name_text = GetServiceNameTextForPortIndex(port_index);
            wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): waiting for acknowledge", service_name_text);

            std::shared_ptr<::Service> forward_service;
            ::ams::sm::MitmProcessInfo client_info = {};
            server->AcknowledgeMitmSession(std::addressof(forward_service), std::addressof(client_info));
            const auto service_name = GetServiceNameForPortIndex(port_index);
            const u64 session_id = mitm_trace::AllocateSessionId();
            wgnx::net_probe::logger::Log(
                "AcceptMitmConnection(%s): acknowledged pid=0x%016llx program_id=0x%016llx forward=%p",
                service_name_text,
                static_cast<unsigned long long>(client_info.process_id.value),
                static_cast<unsigned long long>(client_info.program_id.value),
                forward_service.get());
            if (forward_service != nullptr) {
                mitm_trace::LogForwardServiceState(
                    service_name,
                    client_info,
                    session_id,
                    "acknowledged_forward",
                    forward_service->session,
                    forward_service->own_handle,
                    forward_service->object_id,
                    forward_service->pointer_buffer_size);
            }

            EnsureMitmObjectAllocatorInitialized();
            std::shared_ptr<::Service> local_forward = forward_service;
            wgnx::net_probe::logger::Log(
                "AcceptMitmConnection(%s): reusing forward service local_forward=%p",
                service_name_text,
                local_forward.get());
            if (local_forward != nullptr) {
                mitm_trace::LogForwardServiceState(
                    service_name,
                    client_info,
                    session_id,
                    "local_forward_before_create",
                    local_forward->session,
                    local_forward->own_handle,
                    local_forward->object_id,
                    local_forward->pointer_buffer_size);
            }
            auto service_object = ::ams::sf::CreateSharedObjectEmplaced<IPassiveMitmService, PassiveMitmService>(
                static_cast<::ams::MemoryResource *>(std::addressof(g_mitm_object_memory_resource)),
                std::move(local_forward),
                client_info);
            wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): calling AcceptMitmImpl session_id=%llu", service_name_text, static_cast<unsigned long long>(session_id));

            if (forward_service != nullptr) {
                mitm_trace::LogForwardServiceState(
                    service_name,
                    client_info,
                    session_id,
                    "forward_before_accept",
                    forward_service->session,
                    forward_service->own_handle,
                    forward_service->object_id,
                    forward_service->pointer_buffer_size);
            }
            const ::ams::Result rc = this->AcceptMitmImpl(server, std::move(service_object), client_info, session_id, std::move(forward_service));
            if (R_FAILED(rc)) {
                wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): AcceptMitmImpl failed rc=0x%08X", service_name_text, rc.GetValue());
                mitm_trace::LogSessionAcceptFailure(service_name, client_info, rc);
                R_THROW(rc);
            }

            wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): AcceptMitmImpl succeeded session_id=%llu", service_name_text, static_cast<unsigned long long>(session_id));
            mitm_trace::LogSessionConnected(service_name, session_id, client_info);
            R_SUCCEED();
        }

        virtual ::ams::Result OnNeedsToAccept(int port_index, Server *server) override {
            R_RETURN(this->AcceptMitmConnection(port_index, server));
        }
};

constinit ::ams::util::TypedStorage<MitmServerManager> g_mitm_server_manager_storage = {};
constinit MitmServerManager *g_mitm_server_manager = nullptr;
using ControlServerManager = ::ams::sf::hipc::ServerManager<1>;
constinit ::ams::util::TypedStorage<ControlServerManager> g_control_server_manager_storage = {};
constinit ControlServerManager *g_control_server_manager = nullptr;
constinit ::ams::sf::UnmanagedServiceObject<IProbeControlService, ProbeControlService> g_control_service_object;

alignas(::ams::os::ThreadStackAlignment) constinit u8 g_mitm_server_thread_stack[16 * 1024];
constinit ::ams::os::ThreadType g_mitm_server_thread;
alignas(::ams::os::ThreadStackAlignment) constinit u8 g_control_server_thread_stack[8 * 1024];
constinit ::ams::os::ThreadType g_control_server_thread;
constinit bool g_mitm_thread_started = false;
constinit bool g_control_thread_started = false;
constinit bool g_servers_started = false;

void MitmServerThreadMain(void *) {
    ::ams::os::SignalEvent(std::addressof(g_mitm_thread_ready_event));
    wgnx::net_probe::logger::Log("MITM thread entering server loop");
    g_mitm_server_manager->LoopProcess();
    wgnx::net_probe::logger::Log("MITM thread leaving server loop");
}

void ControlServerThreadMain(void *) {
    ::ams::os::SignalEvent(std::addressof(g_control_thread_ready_event));
    wgnx::net_probe::logger::Log("Control thread entering server loop");
    g_control_server_manager->LoopProcess();
    wgnx::net_probe::logger::Log("Control thread leaving server loop");
}

void LogHasMitmState(const char *service_name, const char *phase) {
    bool has_mitm = false;
    const ::ams::Result rc = ::ams::sm::mitm::HasMitm(std::addressof(has_mitm), ::ams::sm::ServiceName::Encode(service_name));
    if (R_SUCCEEDED(rc)) {
        wgnx::net_probe::logger::Log("HasMitm(%s) %s: present=%u", service_name, phase, has_mitm ? 1u : 0u);
    } else {
        wgnx::net_probe::logger::Log("HasMitm(%s) %s failed rc=0x%08X", service_name, phase, rc.GetValue());
    }
}

void LogAllHasMitmStates(const char *phase) {
    for (const auto &target : g_mitm_target_configs) {
        LogHasMitmState(target.service_name, phase);
    }
}

void EnsureShutdownEventInitialized() {
    if (AMS_LIKELY(g_shutdown_event_initialized)) {
        return;
    }

    ::ams::os::InitializeEvent(std::addressof(g_shutdown_requested_event), false, ::ams::os::EventClearMode_ManualClear);
    g_shutdown_event_initialized = true;
}

void EnsureThreadReadyEventsInitialized() {
    if (AMS_LIKELY(g_thread_ready_events_initialized)) {
        return;
    }

    ::ams::os::InitializeEvent(std::addressof(g_control_thread_ready_event), false, ::ams::os::EventClearMode_ManualClear);
    ::ams::os::InitializeEvent(std::addressof(g_mitm_thread_ready_event), false, ::ams::os::EventClearMode_ManualClear);
    g_thread_ready_events_initialized = true;
}

bool WaitForThreadReady(const char *thread_name, ::ams::os::EventType *event, ::ams::TimeSpan timeout) {
    AMS_ABORT_UNLESS(event != nullptr);

    wgnx::net_probe::logger::Log(
        "StartServers: waiting for %s ready timeout_ms=%lld",
        thread_name,
        static_cast<long long>(timeout.GetMilliSeconds()));
    if (!::ams::os::TimedWaitEvent(event, timeout)) {
        wgnx::net_probe::logger::Log("StartServers: %s ready timeout", thread_name);
        return false;
    }

    wgnx::net_probe::logger::Log("StartServers: %s ready", thread_name);
    return true;
}

void StopAndDestroyControlServerManager() {
    if (g_control_server_manager == nullptr) {
        return;
    }

    wgnx::net_probe::logger::Log("StopAndDestroyControlServerManager: stopping control server");
    g_control_server_manager->RequestStopProcessing();
    if (g_control_thread_started) {
        ::ams::os::WaitThread(std::addressof(g_control_server_thread));
        g_control_thread_started = false;
    }
    std::destroy_at(g_control_server_manager);
    g_control_server_manager = nullptr;
}

void StopAndDestroyMitmServerManager(bool log_snapshots) {
    if (g_mitm_server_manager == nullptr) {
        return;
    }

    if (log_snapshots) {
        mitm_trace::LogSessionSnapshot("before_stop");
        LogAllHasMitmStates("before_stop");
    }

    wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: stopping MITM server");
    g_mitm_server_manager->RequestStopProcessing();
    if (g_mitm_thread_started) {
        ::ams::os::WaitThread(std::addressof(g_mitm_server_thread));
        g_mitm_thread_started = false;
    }

    if (log_snapshots) {
        mitm_trace::LogSessionSnapshot("after_stop_wait");
    }

    std::destroy_at(g_mitm_server_manager);
    g_mitm_server_manager = nullptr;

    if (log_snapshots) {
        LogAllHasMitmStates("after_stop");
        mitm_trace::LogSessionSnapshot("after_stop");
    }
}

} // namespace

bool StartServers() {
    constexpr auto ThreadReadyTimeout = ::ams::TimeSpan::FromMilliSeconds(1000);

    std::scoped_lock lk(g_mitm_start_lock);
    if (g_servers_started) {
        wgnx::net_probe::logger::Log("StartServers: MITM already started");
        return true;
    }

    wgnx::net_probe::logger::Log("StartServers: begin");
    EnsureShutdownEventInitialized();
    EnsureThreadReadyEventsInitialized();
    ::ams::os::ClearEvent(std::addressof(g_shutdown_requested_event));
    ::ams::os::ClearEvent(std::addressof(g_control_thread_ready_event));
    ::ams::os::ClearEvent(std::addressof(g_mitm_thread_ready_event));
    g_shutdown_requested = false;

    mitm_trace::Initialize();
    mitm_trace::RegisterMonitorHooks();

    wgnx::net_probe::logger::Log("StartServers: constructing control server manager");
    g_control_server_manager = ::ams::util::ConstructAt(g_control_server_manager_storage);
    const ::ams::Result control_register_rc = g_control_server_manager->RegisterObjectForServer(
        g_control_service_object.GetShared(),
        ::ams::sm::ServiceName::Encode("wgp:ctl"),
        ControlMaxSessions);
    if (R_FAILED(control_register_rc)) {
        wgnx::net_probe::logger::Log("RegisterObjectForServer(wgp:ctl) failed rc=0x%08X", control_register_rc.GetValue());
        std::destroy_at(g_control_server_manager);
        g_control_server_manager = nullptr;
        return false;
    }
    wgnx::net_probe::logger::Log("Registered control service wgp:ctl");

    wgnx::net_probe::logger::Log("StartServers: creating control thread");
    const ::ams::Result control_thread_rc = ::ams::os::CreateThread(
        std::addressof(g_control_server_thread),
        ControlServerThreadMain,
        nullptr,
        g_control_server_thread_stack,
        sizeof(g_control_server_thread_stack),
        21);
    if (R_FAILED(control_thread_rc)) {
        wgnx::net_probe::logger::Log("CreateThread(wgnx-ctl) failed rc=0x%08X", control_thread_rc.GetValue());
        std::destroy_at(g_control_server_manager);
        g_control_server_manager = nullptr;
        return false;
    }

    ::ams::os::SetThreadNamePointer(std::addressof(g_control_server_thread), "wgnx-ctl");
    ::ams::os::StartThread(std::addressof(g_control_server_thread));
    g_control_thread_started = true;
    if (!WaitForThreadReady("wgnx-ctl", std::addressof(g_control_thread_ready_event), ThreadReadyTimeout)) {
        StopAndDestroyControlServerManager();
        return false;
    }

    wgnx::net_probe::logger::Log("StartServers: constructing MITM server manager");
    g_mitm_server_manager = ::ams::util::ConstructAt(g_mitm_server_manager_storage);

    u32 registered_server_count = 0;
    for (size_t i = 0; i < std::size(g_mitm_target_configs); ++i) {
        const auto &target = g_mitm_target_configs[i];
        if (!target.enabled) {
            wgnx::net_probe::logger::Log(
                "Skipping MITM server for %s: disabled due to known transparency issues",
                target.service_name);
            continue;
        }
        wgnx::net_probe::logger::Log("Registering MITM server for %s", target.service_name);
        const ::ams::Result rc = g_mitm_server_manager->RegisterMitmServer(
            static_cast<int>(i),
            ::ams::sm::ServiceName::Encode(target.service_name),
            target.should_mitm);
        if (R_SUCCEEDED(rc)) {
            ++registered_server_count;
            wgnx::net_probe::logger::Log("Registered MITM server for %s", target.service_name);
        } else {
            wgnx::net_probe::logger::Log("RegisterMitmServer(%s) failed rc=0x%08X", target.service_name, rc.GetValue());
        }
    }

    wgnx::net_probe::logger::Log(
        "Registered MITM servers: requested=%zu active=%u",
        std::size(g_mitm_target_configs),
        registered_server_count);

    if (registered_server_count == 0) {
        wgnx::net_probe::logger::Log("StartServers: no MITM servers active; skipping server thread");
        g_servers_started = true;
        return true;
    }

    wgnx::net_probe::logger::Log("StartServers: creating MITM thread");
    const ::ams::Result thread_rc = ::ams::os::CreateThread(
        std::addressof(g_mitm_server_thread),
        MitmServerThreadMain,
        nullptr,
        g_mitm_server_thread_stack,
        sizeof(g_mitm_server_thread_stack),
        21);
    if (R_FAILED(thread_rc)) {
        wgnx::net_probe::logger::Log("CreateThread(wgnx-mitm) failed rc=0x%08X", thread_rc.GetValue());
        StopAndDestroyMitmServerManager(false);
        StopAndDestroyControlServerManager();
        return false;
    }

    ::ams::os::SetThreadNamePointer(std::addressof(g_mitm_server_thread), "wgnx-mitm");
    ::ams::os::StartThread(std::addressof(g_mitm_server_thread));
    g_mitm_thread_started = true;
    if (!WaitForThreadReady("wgnx-mitm", std::addressof(g_mitm_thread_ready_event), ThreadReadyTimeout)) {
        StopAndDestroyMitmServerManager(false);
        StopAndDestroyControlServerManager();
        return false;
    }

    g_servers_started = true;
    wgnx::net_probe::logger::Log("StartServers: complete");
    return true;
}

void WaitForShutdownRequest() {
    EnsureShutdownEventInitialized();
    ::ams::os::WaitEvent(std::addressof(g_shutdown_requested_event));
}

void RequestShutdown() {
    std::scoped_lock lk(g_mitm_start_lock);
    EnsureShutdownEventInitialized();

    if (g_shutdown_requested) {
        wgnx::net_probe::logger::Log("RequestShutdown: already requested");
        return;
    }

    g_shutdown_requested = true;
    wgnx::net_probe::logger::Log("RequestShutdown: signaling main thread");
    ::ams::os::SignalEvent(std::addressof(g_shutdown_requested_event));
}

void StopServers() {
    std::scoped_lock lk(g_mitm_start_lock);
    if (!g_servers_started && g_control_server_manager == nullptr && g_mitm_server_manager == nullptr) {
        wgnx::net_probe::logger::Log("StopServers: nothing to stop");
        return;
    }

    StopAndDestroyControlServerManager();
    StopAndDestroyMitmServerManager(true);

    g_servers_started = false;
}

} // namespace wgnx::net_probe::mitm
