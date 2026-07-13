#include "mitm_service.hpp"

#include <atomic>
#include <mutex>

#include <stratosphere.hpp>
#include <stratosphere/sf/hipc/sf_hipc_mitm_monitor.hpp>

#include "build_config.hpp"
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
constexpr ::ams::ncm::ProgramId BsdSocketsProgramId = ::ams::ncm::SystemProgramId::BsdSockets;
constexpr ::ams::ncm::ProgramId NpnsProgramId = ::ams::ncm::SystemProgramId::Npns;
constexpr ::ams::ncm::ProgramId EupldProgramId = ::ams::ncm::SystemProgramId::Eupld;
constexpr ::ams::ncm::ProgramId OlscProgramId = ::ams::ncm::SystemProgramId::Olsc;
constexpr ::ams::ncm::ProgramId SslProgramId = ::ams::ncm::SystemProgramId::Ssl;
constexpr ::ams::ncm::ProgramId NimProgramId = ::ams::ncm::SystemProgramId::Nim;
constexpr ::ams::ncm::ProgramId QlaunchProgramId{0x0100000000001000ul};
constexpr ::ams::ncm::ProgramId SphairaWrapperProgramId{0x05446530ACA7E000ul};
constexpr ::ams::ncm::ProgramId RequesterForwarderProgramId{0x05720820ABC97000ul};

namespace {

namespace cfg = ::wgnx::net_probe::build_config;

const char *GetProgramDebugName(::ams::ncm::ProgramId program_id) {
    if (program_id == WireguardProgramId) {
        return "wireguard-nx";
    }
    if (program_id == NetProbeProgramId) {
        return "net-probe";
    }
    if (program_id == BsdSocketsProgramId) {
        return "bsdsockets";
    }
    if (program_id == NpnsProgramId) {
        return "npns";
    }
    if (program_id == EupldProgramId) {
        return "eupld";
    }
    if (program_id == OlscProgramId) {
        return "olsc";
    }
    if (program_id == SslProgramId) {
        return "ssl";
    }
    if (program_id == NimProgramId) {
        return "nim";
    }
    if (program_id == QlaunchProgramId) {
        return "qlaunch";
    }
    if (program_id == SphairaWrapperProgramId) {
        return "sphaira-wrapper";
    }
    if (program_id == RequesterForwarderProgramId) {
        return "requester-forwarder";
    }

    switch (program_id.value) {
        case ::ams::ncm::SystemProgramId::Am.value:
            return "am";
        case ::ams::ncm::SystemProgramId::Ssl.value:
            return "ssl";
        case ::ams::ncm::SystemProgramId::Nim.value:
            return "nim";
        case ::ams::ncm::SystemProgramId::Pctl.value:
            return "pctl";
        default:
            return "unknown";
    }
}

bool IsDiagnosticOverrideEnabled(const ::ams::sm::MitmProcessInfo &client_info, const char *service_name) {
    if (std::strcmp(service_name, "bsd:s") == 0) {
        if (client_info.program_id == NpnsProgramId) {
            return cfg::AllowBsdSystemNpns;
        }
        if (client_info.program_id == EupldProgramId) {
            return cfg::AllowBsdSystemEupld;
        }
        if (client_info.program_id == OlscProgramId) {
            return cfg::AllowBsdSystemOlsc;
        }
        if (client_info.program_id == BsdSocketsProgramId) {
            return cfg::AllowBsdSystemBsdSockets;
        }
        if (client_info.program_id == SslProgramId) {
            return cfg::AllowBsdSystemSsl;
        }
        if (client_info.program_id == NimProgramId) {
            return cfg::AllowBsdSystemNim;
        }
        if (client_info.program_id == SphairaWrapperProgramId) {
            return cfg::AllowBsdSystemSphairaWrapper;
        }
        if (client_info.program_id == RequesterForwarderProgramId) {
            return cfg::AllowBsdSystemRequesterForwarder;
        }
    }

    if (std::strcmp(service_name, "bsd:a") == 0) {
        if (client_info.program_id == QlaunchProgramId) {
            return cfg::AllowBsdAdminQlaunch;
        }
    }

    if (std::strcmp(service_name, "ssl") == 0 || std::strcmp(service_name, "ssl:s") == 0) {
        if (client_info.program_id == NpnsProgramId) {
            return cfg::AllowSslNpns;
        }
        if (client_info.program_id == EupldProgramId) {
            return cfg::AllowSslEupld;
        }
        if (client_info.program_id == OlscProgramId) {
            return cfg::AllowSslOlsc;
        }
    }

    return false;
}

const char *GetRequesterForwarderBsdMitmModeName();

void LogBuildPolicy() {
    wgnx::net_probe::logger::Log(
        "MITM targets: nifm:u=%u nifm:s=%u bsd:u=%u bsd:s=%u bsd:a=%u ssl=%u ssl:s=%u",
        static_cast<unsigned>(cfg::EnableMitmNifmUser),
        static_cast<unsigned>(cfg::EnableMitmNifmSystem),
        static_cast<unsigned>(cfg::EnableMitmBsdUser),
        static_cast<unsigned>(cfg::EnableMitmBsdSystem),
        static_cast<unsigned>(cfg::EnableMitmBsdAdmin),
        static_cast<unsigned>(cfg::EnableMitmSslUser),
        static_cast<unsigned>(cfg::EnableMitmSslSystem));
    wgnx::net_probe::logger::Log(
        "Diagnostic overrides: bsd:s[npns=%u eupld=%u olsc=%u bsdsockets=%u ssl=%u nim=%u sphaira-wrapper=%u requester-forwarder=%u] bsd:a[qlaunch=%u] ssl[npns=%u eupld=%u olsc=%u]",
        static_cast<unsigned>(cfg::AllowBsdSystemNpns),
        static_cast<unsigned>(cfg::AllowBsdSystemEupld),
        static_cast<unsigned>(cfg::AllowBsdSystemOlsc),
        static_cast<unsigned>(cfg::AllowBsdSystemBsdSockets),
        static_cast<unsigned>(cfg::AllowBsdSystemSsl),
        static_cast<unsigned>(cfg::AllowBsdSystemNim),
        static_cast<unsigned>(cfg::AllowBsdSystemSphairaWrapper),
        static_cast<unsigned>(cfg::AllowBsdSystemRequesterForwarder),
        static_cast<unsigned>(cfg::AllowBsdAdminQlaunch),
        static_cast<unsigned>(cfg::AllowSslNpns),
        static_cast<unsigned>(cfg::AllowSslEupld),
        static_cast<unsigned>(cfg::AllowSslOlsc));
    wgnx::net_probe::logger::Log(
        "Requester forwarder bsd:s MITM split: mode=%s (%u)",
        GetRequesterForwarderBsdMitmModeName(),
        static_cast<unsigned>(cfg::RequesterForwarderBsdMitm));
    wgnx::net_probe::logger::Log(
        "MITM HIPC diagnostics: advertise_zero_pointer_buffer_for_requester_bsd=%u",
        static_cast<unsigned>(cfg::AdvertiseZeroPointerBufferForRequesterBsd));
}

alignas(::ams::os::MemoryPageSize) constinit u8 g_mitm_object_heap[32 * 1024];
constinit ::ams::lmem::HeapHandle g_mitm_object_heap_handle = nullptr;
constinit ::ams::sf::ExpHeapMemoryResource g_mitm_object_memory_resource;
constinit ::ams::os::SdkMutex g_mitm_object_allocator_lock;
constinit ::ams::os::SdkMutex g_mitm_start_lock;
constinit bool g_mitm_object_allocator_ready = false;
constinit ::ams::os::EventType g_shutdown_requested_event = {};
constinit ::ams::os::EventType g_control_thread_ready_event = {};
constinit ::ams::os::EventType g_mitm_thread_ready_event = {};
constinit ::ams::os::EventType g_boot_watch_thread_ready_event = {};
constinit ::ams::os::EventType g_watchdog_thread_ready_event = {};
constinit bool g_shutdown_event_initialized = false;
constinit bool g_thread_ready_events_initialized = false;
constinit bool g_shutdown_requested = false;
constinit std::atomic<u32> g_bsd_system_pending_mitm_sessions = 0;
constinit std::atomic<u32> g_bsd_system_active_mitm_sessions = 0;
constinit std::atomic<u64> g_bsd_system_total_mitm_denials = 0;
constinit ::ams::os::SdkMutex g_requester_forwarder_bsd_ordinal_lock;
constinit u64 g_requester_forwarder_bsd_ordinal_pid = 0;
constinit u32 g_requester_forwarder_bsd_ordinal_count = 0;

enum class ShouldMitmPolicyReason : u32 {
    DefaultAllow = 1,
    SelfProgram = 2,
    DenylistedProgram = 3,
    BsdSystemSessionOutstanding = 4,
    DiagnosticOverride = 5,
    RequesterForwarderOrdinalDeny = 6,
};

bool IsBsdSystemServiceName(const ::ams::sm::ServiceName &service_name) {
    constexpr char Expected[] = "bsd:s";
    for (size_t i = 0; i < ::ams::sm::ServiceName::MaxLength; ++i) {
        const char expected = i < sizeof(Expected) ? Expected[i] : '\0';
        if (service_name.name[i] != expected) {
            return false;
        }
        if (expected == '\0') {
            return true;
        }
    }
    return sizeof(Expected) - 1 == ::ams::sm::ServiceName::MaxLength;
}

bool IsBsdSystemServiceNameText(const char *service_name) {
    return service_name != nullptr && std::strcmp(service_name, "bsd:s") == 0;
}

u32 DecrementAtomicIfPositive(std::atomic<u32> &value) {
    u32 current = value.load(std::memory_order_relaxed);
    while (current != 0) {
        if (value.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
            return current - 1;
        }
    }
    return 0;
}

void RecordShouldMitmPolicyDecision(
    const char *service_name,
    const ::ams::sm::MitmProcessInfo &client_info,
    bool result,
    ShouldMitmPolicyReason reason,
    u32 pending_count,
    u32 active_count,
    u32 holder_tag = 0,
    u32 detail_value3 = 0) {
    ::ams::sf::hipc::mitm_monitor::RecordQueryTraceDetail(
        ::ams::sf::hipc::mitm_monitor::QueryTraceEventType::ShouldMitmPolicyDecision,
        ::ams::sm::ServiceName::Encode(service_name),
        std::addressof(client_info),
        true,
        result,
        ::ams::os::InvalidNativeHandle,
        holder_tag,
        static_cast<u32>(reason),
        pending_count,
        active_count,
        static_cast<u32>(g_bsd_system_total_mitm_denials.load(std::memory_order_relaxed)),
        detail_value3,
        false,
        ::ams::ResultSuccess());
}

const char *GetRequesterForwarderBsdMitmModeName() {
    switch (cfg::RequesterForwarderBsdMitm) {
        case cfg::RequesterForwarderBsdMitmMode::None:
            return "none";
        case cfg::RequesterForwarderBsdMitmMode::FirstOnly:
            return "first-only";
        case cfg::RequesterForwarderBsdMitmMode::SecondOnly:
            return "second-only";
        case cfg::RequesterForwarderBsdMitmMode::Both:
            return "both";
        default:
            return "unknown";
    }
}

bool ShouldMitmRequesterForwarderBsdOrdinal(
    const ::ams::sm::MitmProcessInfo &client_info,
    u32 *out_ordinal) {
    const u64 pid = client_info.process_id.value;
    u32 ordinal = 0;

    {
        std::scoped_lock lk(g_requester_forwarder_bsd_ordinal_lock);
        if (g_requester_forwarder_bsd_ordinal_pid != pid) {
            g_requester_forwarder_bsd_ordinal_pid = pid;
            g_requester_forwarder_bsd_ordinal_count = 0;
        }
        ordinal = ++g_requester_forwarder_bsd_ordinal_count;
    }

    if (out_ordinal != nullptr) {
        *out_ordinal = ordinal;
    }

    switch (cfg::RequesterForwarderBsdMitm) {
        case cfg::RequesterForwarderBsdMitmMode::None:
            return false;
        case cfg::RequesterForwarderBsdMitmMode::FirstOnly:
            return ordinal == 1;
        case cfg::RequesterForwarderBsdMitmMode::SecondOnly:
            return ordinal == 2;
        case cfg::RequesterForwarderBsdMitmMode::Both:
            return true;
        default:
            return false;
    }
}

void NoteBsdSystemMitmPending() {
    g_bsd_system_pending_mitm_sessions.fetch_add(1, std::memory_order_relaxed);
}

void PromoteBsdSystemMitmPendingToActive(const char *phase) {
    const u32 pending_after = DecrementAtomicIfPositive(g_bsd_system_pending_mitm_sessions);
    const u32 active_after = g_bsd_system_active_mitm_sessions.fetch_add(1, std::memory_order_relaxed) + 1;
    wgnx::net_probe::logger::Log(
        "bsd:s MITM liveness: phase=%s pending=%u active=%u",
        phase,
        pending_after,
        active_after);
}

void DropBsdSystemMitmPending(const char *phase) {
    const u32 pending_after = DecrementAtomicIfPositive(g_bsd_system_pending_mitm_sessions);
    const u32 active = g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed);
    wgnx::net_probe::logger::Log(
        "bsd:s MITM liveness: phase=%s pending=%u active=%u",
        phase,
        pending_after,
        active);
}

void DropBsdSystemMitmActive(const char *phase) {
    const u32 active_after = DecrementAtomicIfPositive(g_bsd_system_active_mitm_sessions);
    const u32 pending = g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed);
    wgnx::net_probe::logger::Log(
        "bsd:s MITM liveness: phase=%s pending=%u active=%u",
        phase,
        pending,
        active_after);
}

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
    private:
        ::ams::sm::ServiceName m_service_name{};
        u64 m_session_id{0};

    public:
        PassiveMitmService(std::shared_ptr<::Service> &&forward_service, const ::ams::sm::MitmProcessInfo &client_info)
            : ::ams::sf::MitmServiceImplBase(std::move(forward_service), client_info) {
        }

        PassiveMitmService(
            std::shared_ptr<::Service> &&forward_service,
            const ::ams::sm::MitmProcessInfo &client_info,
            const ::ams::sm::ServiceName &service_name,
            u64 session_id)
            : ::ams::sf::MitmServiceImplBase(std::move(forward_service), client_info),
              m_service_name(service_name),
              m_session_id(session_id) {
        }

        ~PassiveMitmService() {
            wgnx::net_probe::logger::Log(
                "PassiveMitmService destructor begin: service=%.*s session_id=%llu pid=0x%016llx program_id=0x%016llx",
                static_cast<int>(::ams::sm::ServiceName::MaxLength),
                m_service_name.name,
                static_cast<unsigned long long>(m_session_id),
                static_cast<unsigned long long>(this->m_client_info.process_id.value),
                static_cast<unsigned long long>(this->m_client_info.program_id.value));
            if (this->m_forward_service != nullptr) {
                mitm_trace::LogForwardServiceState(
                    m_service_name,
                    this->m_client_info,
                    m_session_id,
                    "service_destructor",
                    this->m_forward_service->session,
                    this->m_forward_service->own_handle,
                    this->m_forward_service->object_id,
                    this->m_forward_service->pointer_buffer_size);
            }

            mitm_trace::LogDomainSnapshotForSession(m_session_id, "service_destructor");

            if (IsBsdSystemServiceName(m_service_name)) {
                DropBsdSystemMitmActive("service_destructor");
            }
            wgnx::net_probe::logger::Log(
                "PassiveMitmService destructor end: service=%.*s session_id=%llu",
                static_cast<int>(::ams::sm::ServiceName::MaxLength),
                m_service_name.name,
                static_cast<unsigned long long>(m_session_id));
        }

        static bool IsSelfProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == WireguardProgramId || client_info.program_id == NetProbeProgramId;
        }

        static bool IsBsdSystemDenylistedProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == NpnsProgramId
                || client_info.program_id == EupldProgramId
                || client_info.program_id == OlscProgramId
                || client_info.program_id == BsdSocketsProgramId
                || client_info.program_id == SslProgramId
                || client_info.program_id == NimProgramId
                || client_info.program_id == SphairaWrapperProgramId
                || client_info.program_id == RequesterForwarderProgramId;
        }

        static bool IsBsdAdminDenylistedProgram(const ::ams::sm::MitmProcessInfo &client_info) {
            return client_info.program_id == QlaunchProgramId;
        }

        static void LogShouldMitmDecision(const char *format, ...) {
            /*
             * File-backed logging from ShouldMitm* runs on SM's hidden MITM query
             * path. Keep it disabled while isolating the second-launch hang.
             */
            AMS_UNUSED(format);
        }

        static bool ShouldMitmForService(const char *service_name, const ::ams::sm::MitmProcessInfo &client_info) {
            const bool should_mitm = !IsSelfProgram(client_info);
            RecordShouldMitmPolicyDecision(
                service_name,
                client_info,
                should_mitm,
                should_mitm ? ShouldMitmPolicyReason::DefaultAllow : ShouldMitmPolicyReason::SelfProgram,
                IsBsdSystemServiceNameText(service_name) ? g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed) : 0,
                IsBsdSystemServiceNameText(service_name) ? g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed) : 0);
            LogShouldMitmDecision(
                "ShouldMitm(%s): pid=0x%016llx program_id=0x%016llx (%s) result=%u",
                service_name,
                static_cast<unsigned long long>(client_info.process_id.value),
                static_cast<unsigned long long>(client_info.program_id.value),
                GetProgramDebugName(client_info.program_id),
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
                RecordShouldMitmPolicyDecision(
                    "bsd:s",
                    client_info,
                    false,
                    ShouldMitmPolicyReason::SelfProgram,
                    g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed),
                    g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed));
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx (%s) skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (client_info.program_id == RequesterForwarderProgramId && cfg::AllowBsdSystemRequesterForwarder) {
                u32 ordinal = 0;
                const bool allow_for_ordinal = ShouldMitmRequesterForwarderBsdOrdinal(client_info, std::addressof(ordinal));
                const u32 pending_count = g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed);
                const u32 active_count = g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed);
                RecordShouldMitmPolicyDecision(
                    "bsd:s",
                    client_info,
                    allow_for_ordinal,
                    allow_for_ordinal ? ShouldMitmPolicyReason::DiagnosticOverride : ShouldMitmPolicyReason::RequesterForwarderOrdinalDeny,
                    pending_count,
                    active_count,
                    ordinal,
                    static_cast<u32>(cfg::RequesterForwarderBsdMitm));
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx (%s) requester_forwarder_ordinal=%u mode=%s result=%u",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id),
                    ordinal,
                    GetRequesterForwarderBsdMitmModeName(),
                    allow_for_ordinal ? 1u : 0u);
                if (!allow_for_ordinal) {
                    return false;
                }

                const bool should_mitm = ShouldMitmForService("bsd:s", client_info);
                if (should_mitm) {
                    NoteBsdSystemMitmPending();
                }
                return should_mitm;
            }

            if (IsBsdSystemDenylistedProgram(client_info) && !IsDiagnosticOverrideEnabled(client_info, "bsd:s")) {
                RecordShouldMitmPolicyDecision(
                    "bsd:s",
                    client_info,
                    false,
                    ShouldMitmPolicyReason::DenylistedProgram,
                    g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed),
                    g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed));
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx (%s) skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            const u32 pending_count = g_bsd_system_pending_mitm_sessions.load(std::memory_order_relaxed);
            const u32 active_count = g_bsd_system_active_mitm_sessions.load(std::memory_order_relaxed);

            if (IsDiagnosticOverrideEnabled(client_info, "bsd:s")) {
                RecordShouldMitmPolicyDecision(
                    "bsd:s",
                    client_info,
                    true,
                    ShouldMitmPolicyReason::DiagnosticOverride,
                    pending_count,
                    active_count);
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:s): pid=0x%016llx program_id=0x%016llx (%s) override=diagnostic",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
            }

            const bool should_mitm = ShouldMitmForService("bsd:s", client_info);
            if (should_mitm) {
                NoteBsdSystemMitmPending();
            }
            return should_mitm;
        }

        static bool ShouldMitmBsdAdmin(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:a): pid=0x%016llx program_id=0x%016llx (%s) skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsBsdAdminDenylistedProgram(client_info) && !IsDiagnosticOverrideEnabled(client_info, "bsd:a")) {
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:a): pid=0x%016llx program_id=0x%016llx (%s) skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsDiagnosticOverrideEnabled(client_info, "bsd:a")) {
                LogShouldMitmDecision(
                    "ShouldMitm(bsd:a): pid=0x%016llx program_id=0x%016llx (%s) override=diagnostic",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
            }

            return ShouldMitmForService("bsd:a", client_info);
        }

        static bool ShouldMitmSslUser(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl): pid=0x%016llx program_id=0x%016llx (%s) skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsBsdSystemDenylistedProgram(client_info) && !IsDiagnosticOverrideEnabled(client_info, "ssl")) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl): pid=0x%016llx program_id=0x%016llx (%s) skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsDiagnosticOverrideEnabled(client_info, "ssl")) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl): pid=0x%016llx program_id=0x%016llx (%s) override=diagnostic",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
            }

            return ShouldMitmForService("ssl", client_info);
        }

        static bool ShouldMitmSslSystem(const ::ams::sm::MitmProcessInfo &client_info) {
            if (IsSelfProgram(client_info)) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl:s): pid=0x%016llx program_id=0x%016llx (%s) skipped=self",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsBsdSystemDenylistedProgram(client_info) && !IsDiagnosticOverrideEnabled(client_info, "ssl:s")) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl:s): pid=0x%016llx program_id=0x%016llx (%s) skipped=denylist",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
                return false;
            }

            if (IsDiagnosticOverrideEnabled(client_info, "ssl:s")) {
                LogShouldMitmDecision(
                    "ShouldMitm(ssl:s): pid=0x%016llx program_id=0x%016llx (%s) override=diagnostic",
                    static_cast<unsigned long long>(client_info.process_id.value),
                    static_cast<unsigned long long>(client_info.program_id.value),
                    GetProgramDebugName(client_info.program_id));
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
    { "nifm:u", &PassiveMitmService::ShouldMitmNifmUser, cfg::EnableMitmNifmUser },
    { "nifm:s", &PassiveMitmService::ShouldMitmNifmSystem, cfg::EnableMitmNifmSystem },
    { "bsd:u",  &PassiveMitmService::ShouldMitmBsdUser, cfg::EnableMitmBsdUser },
    { "bsd:s",  &PassiveMitmService::ShouldMitmBsdSystem, cfg::EnableMitmBsdSystem },
    { "bsd:a",  &PassiveMitmService::ShouldMitmBsdAdmin, cfg::EnableMitmBsdAdmin },
    { "ssl",    &PassiveMitmService::ShouldMitmSslUser, cfg::EnableMitmSslUser },
    { "ssl:s",  &PassiveMitmService::ShouldMitmSslSystem, cfg::EnableMitmSslSystem },
};

constexpr size_t NumSessions = 48;
constexpr size_t ControlMaxSessions = 4;

void LogMitmServerManagerState(const char *phase);
void LogMitmServerManagerResourceState(const char *phase);

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
            const ::ams::Result acknowledge_rc = server->AcknowledgeMitmSession(std::addressof(forward_service), std::addressof(client_info));
            const auto service_name = GetServiceNameForPortIndex(port_index);
            if (R_FAILED(acknowledge_rc)) {
                if (IsBsdSystemServiceName(service_name)) {
                    DropBsdSystemMitmPending("acknowledge_failed");
                }
                wgnx::net_probe::logger::Log(
                    "AcceptMitmConnection(%s): AcknowledgeMitmSession failed rc=0x%08X",
                    service_name_text,
                    acknowledge_rc.GetValue());
                R_THROW(acknowledge_rc);
            }
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
                client_info,
                service_name,
                session_id);
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
                if (IsBsdSystemServiceName(service_name)) {
                    DropBsdSystemMitmPending("accept_mitm_impl_failed");
                }
                wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): AcceptMitmImpl failed rc=0x%08X", service_name_text, rc.GetValue());
                mitm_trace::LogSessionAcceptFailure(service_name, client_info, rc);
                R_THROW(rc);
            }
            if (IsBsdSystemServiceName(service_name)) {
                PromoteBsdSystemMitmPendingToActive("accept_mitm_impl_success");
            }

            wgnx::net_probe::logger::Log("AcceptMitmConnection(%s): AcceptMitmImpl succeeded session_id=%llu", service_name_text, static_cast<unsigned long long>(session_id));
            mitm_trace::LogSessionConnected(service_name, session_id, client_info);
            LogMitmServerManagerState("accept_success");
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

constexpr size_t MitmServerThreadStackSize = 64 * 1024;
alignas(::ams::os::ThreadStackAlignment) constinit u8 g_mitm_server_thread_stack[MitmServerThreadStackSize];
constinit ::ams::os::ThreadType g_mitm_server_thread;
alignas(::ams::os::ThreadStackAlignment) constinit u8 g_control_server_thread_stack[8 * 1024];
constinit ::ams::os::ThreadType g_control_server_thread;
alignas(::ams::os::ThreadStackAlignment) constinit u8 g_boot_watch_thread_stack[8 * 1024];
constinit ::ams::os::ThreadType g_boot_watch_thread;
alignas(::ams::os::ThreadStackAlignment) constinit u8 g_watchdog_thread_stack[64 * 1024];
constinit ::ams::os::ThreadType g_watchdog_thread;
constinit bool g_mitm_thread_started = false;
constinit bool g_control_thread_started = false;
constinit bool g_boot_watch_thread_started = false;
constinit bool g_watchdog_thread_started = false;
constinit bool g_servers_started = false;

constexpr const char *g_boot_watch_services[] = {
    "sfdnsres",
    "dns:priv",
    "nsd:u",
};
constexpr auto BootWatchSampleInterval = ::ams::TimeSpan::FromSeconds(10);
constexpr size_t BootWatchSampleCount = 6;
constexpr auto WatchdogSampleInterval = ::ams::TimeSpan::FromSeconds(2);
constexpr size_t WatchdogPostTeardownSampleCount = 6;
constexpr size_t WatchdogIdleQuerySummaryIntervalCount = 5;
constexpr size_t QueryTraceDrainBatchSize = 64;

constinit u64 g_query_trace_cursor = 0;
constinit u64 g_query_trace_dropped_total = 0;

void EncodeServiceName(char *out, size_t out_size, const ::ams::sm::ServiceName &service_name) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    size_t length = 0;
    while (length < ::ams::sm::ServiceName::MaxLength && service_name.name[length] != '\0') {
        ++length;
    }

    if (length == 0) {
        std::snprintf(out, out_size, "<invalid>");
        return;
    }

    std::snprintf(out, out_size, "%.*s", static_cast<int>(length), service_name.name);
}

const char *GetQueryTraceEventName(::ams::sf::hipc::mitm_monitor::QueryTraceEventType event_type) {
    using EventType = ::ams::sf::hipc::mitm_monitor::QueryTraceEventType;
    switch (event_type) {
        case EventType::RegisterHandle:
            return "register_handle";
        case EventType::RegisterSessionBegin:
            return "register_session_begin";
        case EventType::RegisterSessionEnd:
            return "register_session_end";
        case EventType::ServerThreadCreateBegin:
            return "server_thread_create_begin";
        case EventType::ServerThreadCreateEnd:
            return "server_thread_create_end";
        case EventType::ServerThreadStart:
            return "server_thread_start";
        case EventType::ServerLoopBegin:
            return "server_loop_begin";
        case EventType::ServerLoopEnd:
            return "server_loop_end";
        case EventType::WaitLoopBegin:
            return "wait_loop_begin";
        case EventType::WaitLinkDeferredBegin:
            return "wait_link_deferred_begin";
        case EventType::WaitLinkDeferredEnd:
            return "wait_link_deferred_end";
        case EventType::WaitAnyBegin:
            return "wait_any_begin";
        case EventType::WaitAnyEnd:
            return "wait_any_end";
        case EventType::WaitSelectedStop:
            return "wait_selected_stop";
        case EventType::WaitSelectedNotify:
            return "wait_selected_notify";
        case EventType::WaitSelectedSession:
            return "wait_selected_session";
        case EventType::WaitSelectedOther:
            return "wait_selected_other";
        case EventType::NotifyCleared:
            return "notify_cleared";
        case EventType::HolderUnlinked:
            return "holder_unlinked";
        case EventType::WaitAndProcessBegin:
            return "wait_and_process_begin";
        case EventType::WaitAndProcessEnd:
            return "wait_and_process_end";
        case EventType::ProcessBegin:
            return "process_begin";
        case EventType::ProcessEnd:
            return "process_end";
        case EventType::ProcessSessionBegin:
            return "process_session_begin";
        case EventType::ProcessSessionReceiveBegin:
            return "process_session_receive_begin";
        case EventType::ProcessSessionReceiveEnd:
            return "process_session_receive_end";
        case EventType::ProcessSessionDispatchBegin:
            return "process_session_dispatch_begin";
        case EventType::ProcessSessionDispatchEnd:
            return "process_session_dispatch_end";
        case EventType::ProcessSessionEnd:
            return "process_session_end";
        case EventType::ShouldMitmBegin:
            return "should_mitm_begin";
        case EventType::ShouldMitmEnd:
            return "should_mitm_end";
        case EventType::ShouldMitmPolicyDecision:
            return "should_mitm_policy_decision";
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

void LogMitmServerManagerState(const char *phase) {
    if (g_mitm_server_manager == nullptr) {
        wgnx::net_probe::logger::Log("MITM manager state: phase=%s unavailable=1", phase);
        return;
    }

    const auto resources = g_mitm_server_manager->GetDebugResourceState();
    const auto wait_state = g_mitm_server_manager->GetDebugMultiWaitState();
    wgnx::net_probe::logger::Log(
        "MITM manager state: phase=%s resources[sessions=%zu/%zu servers=%zu/%zu domains=%zu/%zu] active_wait[total=%zu server=%zu session=%zu mitm=%zu other=%zu] deferred_wait[total=%zu server=%zu session=%zu mitm=%zu other=%zu]",
        phase,
        resources.allocated_sessions,
        resources.max_sessions,
        resources.allocated_servers,
        resources.max_servers,
        resources.allocated_domains,
        resources.max_domains,
        wait_state.active.total,
        wait_state.active.server,
        wait_state.active.session,
        wait_state.active.mitm_server,
        wait_state.active.other,
        wait_state.deferred.total,
        wait_state.deferred.server,
        wait_state.deferred.session,
        wait_state.deferred.mitm_server,
        wait_state.deferred.other);
}

void LogMitmServerManagerResourceState(const char *phase) {
    if (g_mitm_server_manager == nullptr) {
        wgnx::net_probe::logger::Log("MITM manager resources: phase=%s unavailable=1", phase);
        return;
    }

    const auto resources = g_mitm_server_manager->GetDebugResourceState();
    wgnx::net_probe::logger::Log(
        "MITM manager resources: phase=%s sessions=%zu/%zu servers=%zu/%zu domains=%zu/%zu",
        phase,
        resources.allocated_sessions,
        resources.max_sessions,
        resources.allocated_servers,
        resources.max_servers,
        resources.allocated_domains,
        resources.max_domains);
}

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

void LogEnabledTargetHasMitmStates(const char *phase) {
    for (const auto &target : g_mitm_target_configs) {
        if (!target.enabled) {
            continue;
        }
        LogHasMitmState(target.service_name, phase);
    }
}

void LogResolverHasMitmStates(const char *phase) {
    for (const char *service_name : g_boot_watch_services) {
        LogHasMitmState(service_name, phase);
    }
}

void LogMitmQueryTelemetry(const char *phase, bool force_summary) {
    namespace monitor = ::ams::sf::hipc::mitm_monitor;

    size_t drained_total = 0;
    while (true) {
        monitor::QueryTraceRecord records[QueryTraceDrainBatchSize] = {};
        u64 dropped = 0;
        const size_t drained = monitor::DrainQueryTrace(
            records,
            std::size(records),
            std::addressof(g_query_trace_cursor),
            std::addressof(dropped));

        if (dropped != 0) {
            g_query_trace_dropped_total += dropped;
            wgnx::net_probe::logger::Log(
                "MITM query trace dropped: phase=%s dropped=%llu total_dropped=%llu cursor=%llu",
                phase,
                static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(g_query_trace_dropped_total),
                static_cast<unsigned long long>(g_query_trace_cursor));
        }

        for (size_t i = 0; i < drained; ++i) {
            const auto &record = records[i];
            char service_name[::ams::sm::ServiceName::MaxLength + 8] = {};
            EncodeServiceName(service_name, sizeof(service_name), record.service_name);
            wgnx::net_probe::logger::Log(
                "MITM query trace: phase=%s seq=%llu tick=%llu event=%s service=%s has_client=%u pid=0x%016llx program_id=0x%016llx has_result=%u result=%u query_handle=%d holder_tag=%u detail_code=%u detail0=%u detail1=%u detail2=%u detail3=%u has_detail_result=%u detail_result=0x%08X",
                phase,
                static_cast<unsigned long long>(record.sequence),
                static_cast<unsigned long long>(record.tick),
                GetQueryTraceEventName(record.event_type),
                service_name,
                record.has_client_info ? 1u : 0u,
                static_cast<unsigned long long>(record.client_info.process_id.value),
                static_cast<unsigned long long>(record.client_info.program_id.value),
                record.has_result ? 1u : 0u,
                record.result ? 1u : 0u,
                record.query_handle,
                record.holder_tag,
                record.detail_code,
                record.detail_value0,
                record.detail_value1,
                record.detail_value2,
                record.detail_value3,
                record.has_detail_result ? 1u : 0u,
                record.has_detail_result ? record.detail_result_value : 0u);
        }

        drained_total += drained;
        if (drained < std::size(records)) {
            break;
        }
    }

    const auto summary = monitor::GetQueryTraceSummary();
    if (force_summary || drained_total != 0) {
        wgnx::net_probe::logger::Log(
            "MITM query summary: phase=%s written=%llu cursor=%llu dropped_total=%llu register=%llu reg_session=%llu/%llu thread_create=%llu/%llu thread_start=%llu loop=%llu/%llu wait_loop=%llu wait_any=%llu/%llu selected[stop=%llu notify=%llu session=%llu other=%llu] notify_cleared=%llu unlinked=%llu wait_process=%llu/%llu process=%llu/%llu session=%llu/%llu recv=%llu/%llu dispatch=%llu/%llu should=%llu/%llu pending_should=%lld pending_dispatch=%lld",
            phase,
            static_cast<unsigned long long>(summary.written),
            static_cast<unsigned long long>(g_query_trace_cursor),
            static_cast<unsigned long long>(g_query_trace_dropped_total),
            static_cast<unsigned long long>(summary.register_handle_count),
            static_cast<unsigned long long>(summary.register_session_begin_count),
            static_cast<unsigned long long>(summary.register_session_end_count),
            static_cast<unsigned long long>(summary.server_thread_create_begin_count),
            static_cast<unsigned long long>(summary.server_thread_create_end_count),
            static_cast<unsigned long long>(summary.server_thread_start_count),
            static_cast<unsigned long long>(summary.server_loop_begin_count),
            static_cast<unsigned long long>(summary.server_loop_end_count),
            static_cast<unsigned long long>(summary.wait_loop_begin_count),
            static_cast<unsigned long long>(summary.wait_any_begin_count),
            static_cast<unsigned long long>(summary.wait_any_end_count),
            static_cast<unsigned long long>(summary.wait_selected_stop_count),
            static_cast<unsigned long long>(summary.wait_selected_notify_count),
            static_cast<unsigned long long>(summary.wait_selected_session_count),
            static_cast<unsigned long long>(summary.wait_selected_other_count),
            static_cast<unsigned long long>(summary.notify_cleared_count),
            static_cast<unsigned long long>(summary.holder_unlinked_count),
            static_cast<unsigned long long>(summary.wait_and_process_begin_count),
            static_cast<unsigned long long>(summary.wait_and_process_end_count),
            static_cast<unsigned long long>(summary.process_begin_count),
            static_cast<unsigned long long>(summary.process_end_count),
            static_cast<unsigned long long>(summary.process_session_begin_count),
            static_cast<unsigned long long>(summary.process_session_end_count),
            static_cast<unsigned long long>(summary.process_session_receive_begin_count),
            static_cast<unsigned long long>(summary.process_session_receive_end_count),
            static_cast<unsigned long long>(summary.process_session_dispatch_begin_count),
            static_cast<unsigned long long>(summary.process_session_dispatch_end_count),
            static_cast<unsigned long long>(summary.should_mitm_begin_count),
            static_cast<unsigned long long>(summary.should_mitm_end_count),
            static_cast<long long>(summary.should_mitm_begin_count) - static_cast<long long>(summary.should_mitm_end_count),
            static_cast<long long>(summary.process_session_dispatch_begin_count) - static_cast<long long>(summary.process_session_dispatch_end_count));
    }
}

void BootWatchThreadMain(void *) {
    ::ams::os::SignalEvent(std::addressof(g_boot_watch_thread_ready_event));
    wgnx::net_probe::logger::Log(
        "BootWatch thread started: samples=%zu interval_ms=%lld",
        BootWatchSampleCount,
        static_cast<long long>(BootWatchSampleInterval.GetMilliSeconds()));

    for (size_t sample_index = 0; sample_index < BootWatchSampleCount; ++sample_index) {
        char phase[64];
        std::snprintf(
            phase,
            sizeof(phase),
            "boot_watch_%zu_t+%us",
            sample_index,
            static_cast<unsigned>(sample_index * BootWatchSampleInterval.GetSeconds()));
        LogResolverHasMitmStates(phase);

        if (sample_index + 1 == BootWatchSampleCount) {
            break;
        }

        if (::ams::os::TimedWaitEvent(std::addressof(g_shutdown_requested_event), BootWatchSampleInterval)) {
            wgnx::net_probe::logger::Log("BootWatch thread exiting early due to shutdown request");
            return;
        }
    }

    wgnx::net_probe::logger::Log("BootWatch thread completed");
}

void WatchdogThreadMain(void *) {
    ::ams::os::SignalEvent(std::addressof(g_watchdog_thread_ready_event));
    wgnx::net_probe::logger::Log(
        "Watchdog thread started: interval_ms=%lld",
        static_cast<long long>(WatchdogSampleInterval.GetMilliSeconds()));

    size_t sample_index = 0;
    size_t previous_tracked_session_count = 0;
    size_t post_teardown_samples_remaining = 0;
    size_t idle_query_summary_samples = 0;
    while (true) {
        if (::ams::os::TimedWaitEvent(std::addressof(g_shutdown_requested_event), WatchdogSampleInterval)) {
            wgnx::net_probe::logger::Log("Watchdog thread exiting due to shutdown request");
            return;
        }

        const size_t tracked_session_count = mitm_trace::GetTrackedSessionCount();
        if (previous_tracked_session_count != 0 && tracked_session_count == 0) {
            post_teardown_samples_remaining = WatchdogPostTeardownSampleCount;
        }

        const auto query_summary = ::ams::sf::hipc::mitm_monitor::GetQueryTraceSummary();
        const bool has_pending_query_trace = query_summary.written != g_query_trace_cursor;
        const bool should_log_idle_query_summary =
            tracked_session_count == 0 &&
            post_teardown_samples_remaining == 0 &&
            ++idle_query_summary_samples >= WatchdogIdleQuerySummaryIntervalCount;
        const bool should_log =
            tracked_session_count != 0 ||
            post_teardown_samples_remaining != 0 ||
            has_pending_query_trace ||
            should_log_idle_query_summary;
        previous_tracked_session_count = tracked_session_count;
        if (!should_log) {
            continue;
        }
        idle_query_summary_samples = 0;

        char phase[64];
        std::snprintf(phase, sizeof(phase), "watchdog_%zu", sample_index++);
        wgnx::net_probe::logger::Log(
            "Watchdog snapshot: phase=%s tracked_sessions=%zu",
            phase,
            tracked_session_count);
        LogMitmQueryTelemetry(phase, should_log_idle_query_summary);
        LogMitmServerManagerResourceState(phase);
        LogEnabledTargetHasMitmStates(phase);
        mitm_trace::LogSessionSnapshot(phase);
        mitm_trace::LogDomainSnapshot(phase);

        if (tracked_session_count == 0 && post_teardown_samples_remaining != 0) {
            --post_teardown_samples_remaining;
        }
    }
}

void ClearResidualMitmState(const char *service_name, const char *phase) {
    const auto encoded_service_name = ::ams::sm::ServiceName::Encode(service_name);

    bool has_mitm = false;
    ::ams::Result has_mitm_rc = ::ams::sm::mitm::HasMitm(std::addressof(has_mitm), encoded_service_name);
    if (R_SUCCEEDED(has_mitm_rc)) {
        wgnx::net_probe::logger::Log(
            "ClearResidualMitmState(%s) %s: has_mitm=%u",
            service_name,
            phase,
            has_mitm ? 1u : 0u);
    } else {
        wgnx::net_probe::logger::Log(
            "ClearResidualMitmState(%s) %s: HasMitm failed rc=0x%08X",
            service_name,
            phase,
            has_mitm_rc.GetValue());
    }

    if (R_SUCCEEDED(has_mitm_rc) && has_mitm) {
        const ::ams::Result uninstall_rc = ::ams::sm::mitm::UninstallMitm(encoded_service_name);
        wgnx::net_probe::logger::Log(
            "ClearResidualMitmState(%s) %s: UninstallMitm rc=0x%08X",
            service_name,
            phase,
            uninstall_rc.GetValue());
    }

    const ::ams::Result clear_future_rc = ::ams::sm::mitm::ClearFutureMitm(encoded_service_name);
    wgnx::net_probe::logger::Log(
        "ClearResidualMitmState(%s) %s: ClearFutureMitm rc=0x%08X",
        service_name,
        phase,
        clear_future_rc.GetValue());

    has_mitm = false;
    has_mitm_rc = ::ams::sm::mitm::HasMitm(std::addressof(has_mitm), encoded_service_name);
    if (R_SUCCEEDED(has_mitm_rc)) {
        wgnx::net_probe::logger::Log(
            "ClearResidualMitmState(%s) %s: final_has_mitm=%u",
            service_name,
            phase,
            has_mitm ? 1u : 0u);
    } else {
        wgnx::net_probe::logger::Log(
            "ClearResidualMitmState(%s) %s: final HasMitm failed rc=0x%08X",
            service_name,
            phase,
            has_mitm_rc.GetValue());
    }
}

void ClearAllResidualMitmStates(const char *phase) {
    for (const auto &target : g_mitm_target_configs) {
        if (!target.enabled) {
            continue;
        }
        ClearResidualMitmState(target.service_name, phase);
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
    ::ams::os::InitializeEvent(std::addressof(g_boot_watch_thread_ready_event), false, ::ams::os::EventClearMode_ManualClear);
    ::ams::os::InitializeEvent(std::addressof(g_watchdog_thread_ready_event), false, ::ams::os::EventClearMode_ManualClear);
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
        wgnx::net_probe::logger::Log("StopAndDestroyControlServerManager: waiting for control thread");
        ::ams::os::WaitThread(std::addressof(g_control_server_thread));
        ::ams::os::DestroyThread(std::addressof(g_control_server_thread));
        g_control_thread_started = false;
        wgnx::net_probe::logger::Log("StopAndDestroyControlServerManager: control thread stopped");
    }
    wgnx::net_probe::logger::Log("StopAndDestroyControlServerManager: destroying control server manager");
    std::destroy_at(g_control_server_manager);
    g_control_server_manager = nullptr;
    wgnx::net_probe::logger::Log("StopAndDestroyControlServerManager: complete");
}

void StopBootWatchThread() {
    if (!g_boot_watch_thread_started) {
        return;
    }

    wgnx::net_probe::logger::Log("StopBootWatchThread: waiting for boot watch thread");
    ::ams::os::WaitThread(std::addressof(g_boot_watch_thread));
    ::ams::os::DestroyThread(std::addressof(g_boot_watch_thread));
    g_boot_watch_thread_started = false;
    wgnx::net_probe::logger::Log("StopBootWatchThread: boot watch thread stopped");
}

void StopWatchdogThread() {
    if (!g_watchdog_thread_started) {
        return;
    }

    wgnx::net_probe::logger::Log("StopWatchdogThread: waiting for watchdog thread");
    ::ams::os::WaitThread(std::addressof(g_watchdog_thread));
    ::ams::os::DestroyThread(std::addressof(g_watchdog_thread));
    g_watchdog_thread_started = false;
    wgnx::net_probe::logger::Log("StopWatchdogThread: watchdog thread stopped");
}

void StopAndDestroyMitmServerManager(bool log_snapshots, bool clear_residual_state = true) {
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
        wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: waiting for MITM thread");
        ::ams::os::WaitThread(std::addressof(g_mitm_server_thread));
        ::ams::os::DestroyThread(std::addressof(g_mitm_server_thread));
        g_mitm_thread_started = false;
        wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: MITM thread stopped");
    }

    if (log_snapshots) {
        mitm_trace::LogSessionSnapshot("after_stop_wait");
    }

    wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: destroying MITM server manager");
    std::destroy_at(g_mitm_server_manager);
    g_mitm_server_manager = nullptr;
    wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: destroyed MITM server manager");

    if (log_snapshots) {
        LogAllHasMitmStates("after_stop");
        mitm_trace::LogSessionSnapshot("after_stop");
    }

    if (clear_residual_state) {
        ClearAllResidualMitmStates("after_destroy");
    } else {
        wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: residual MITM clear skipped");
    }
    wgnx::net_probe::logger::Log("StopAndDestroyMitmServerManager: complete");
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
    ::ams::os::ClearEvent(std::addressof(g_boot_watch_thread_ready_event));
    ::ams::os::ClearEvent(std::addressof(g_watchdog_thread_ready_event));
    g_shutdown_requested = false;

    mitm_trace::Initialize();
    mitm_trace::RegisterMonitorHooks();
    LogBuildPolicy();
    LogResolverHasMitmStates("pre_register");

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

    u32 requested_server_count = 0;
    u32 registered_server_count = 0;
    u32 failed_server_count = 0;
    for (size_t i = 0; i < std::size(g_mitm_target_configs); ++i) {
        const auto &target = g_mitm_target_configs[i];
        if (!target.enabled) {
            wgnx::net_probe::logger::Log(
                "Skipping MITM server for %s: disabled by build config",
                target.service_name);
            continue;
        }
        ++requested_server_count;
        wgnx::net_probe::logger::Log("Registering MITM server for %s", target.service_name);
        LogHasMitmState(target.service_name, "before_register_target");
        const ::ams::Result rc = g_mitm_server_manager->RegisterMitmServer(
            static_cast<int>(i),
            ::ams::sm::ServiceName::Encode(target.service_name),
            target.should_mitm);
        if (R_SUCCEEDED(rc)) {
            ++registered_server_count;
            wgnx::net_probe::logger::Log("Registered MITM server for %s", target.service_name);
            LogHasMitmState(target.service_name, "after_register_target_success");
        } else {
            ++failed_server_count;
            wgnx::net_probe::logger::Log("RegisterMitmServer(%s) failed rc=0x%08X", target.service_name, rc.GetValue());
            LogHasMitmState(target.service_name, "after_register_target_failure");
        }
    }

    wgnx::net_probe::logger::Log(
        "Registered MITM servers: requested=%u active=%u failed=%u",
        requested_server_count,
        registered_server_count,
        failed_server_count);

    if (failed_server_count != 0) {
        wgnx::net_probe::logger::Log("StartServers: MITM registration failed; tearing down partial MITM state");
        LogEnabledTargetHasMitmStates("registration_failure_before_teardown");
        StopAndDestroyMitmServerManager(false, false);
        StopAndDestroyControlServerManager();
        LogEnabledTargetHasMitmStates("registration_failure_after_teardown");
        return false;
    }

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
    wgnx::net_probe::logger::Log("StartServers: creating boot watch thread");
    const ::ams::Result boot_watch_thread_rc = ::ams::os::CreateThread(
        std::addressof(g_boot_watch_thread),
        BootWatchThreadMain,
        nullptr,
        g_boot_watch_thread_stack,
        sizeof(g_boot_watch_thread_stack),
        20);
    if (R_FAILED(boot_watch_thread_rc)) {
        wgnx::net_probe::logger::Log("CreateThread(wgnx-boot-watch) failed rc=0x%08X", boot_watch_thread_rc.GetValue());
    } else {
        ::ams::os::SetThreadNamePointer(std::addressof(g_boot_watch_thread), "wgnx-boot-watch");
        ::ams::os::StartThread(std::addressof(g_boot_watch_thread));
        g_boot_watch_thread_started = true;
        static_cast<void>(WaitForThreadReady("wgnx-boot-watch", std::addressof(g_boot_watch_thread_ready_event), ThreadReadyTimeout));
    }

    wgnx::net_probe::logger::Log("StartServers: creating watchdog thread");
    const ::ams::Result watchdog_thread_rc = ::ams::os::CreateThread(
        std::addressof(g_watchdog_thread),
        WatchdogThreadMain,
        nullptr,
        g_watchdog_thread_stack,
        sizeof(g_watchdog_thread_stack),
        19);
    if (R_FAILED(watchdog_thread_rc)) {
        wgnx::net_probe::logger::Log("CreateThread(wgnx-watchdog) failed rc=0x%08X", watchdog_thread_rc.GetValue());
    } else {
        ::ams::os::SetThreadNamePointer(std::addressof(g_watchdog_thread), "wgnx-watchdog");
        ::ams::os::StartThread(std::addressof(g_watchdog_thread));
        g_watchdog_thread_started = true;
        static_cast<void>(WaitForThreadReady("wgnx-watchdog", std::addressof(g_watchdog_thread_ready_event), ThreadReadyTimeout));
    }

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

    wgnx::net_probe::logger::Log("StopServers: begin");
    StopBootWatchThread();
    StopWatchdogThread();
    StopAndDestroyMitmServerManager(true);
    StopAndDestroyControlServerManager();

    ::ams::os::ClearEvent(std::addressof(g_shutdown_requested_event));
    ::ams::os::ClearEvent(std::addressof(g_control_thread_ready_event));
    ::ams::os::ClearEvent(std::addressof(g_mitm_thread_ready_event));
    ::ams::os::ClearEvent(std::addressof(g_boot_watch_thread_ready_event));
    ::ams::os::ClearEvent(std::addressof(g_watchdog_thread_ready_event));
    g_shutdown_requested = false;

    g_servers_started = false;
    wgnx::net_probe::logger::Log("StopServers: complete");
}

} // namespace wgnx::net_probe::mitm
