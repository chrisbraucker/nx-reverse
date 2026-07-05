#include "mitm_trace.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "fs_runtime.hpp"
#include "logger.hpp"

namespace wgnx::net_probe::mitm_trace {

namespace {

constexpr const char *TraceDirectory = "/wgnx";
constexpr const char *NifmTracePath = "sdmc:/wgnx/probe-mitm-nifm.jsonl";
constexpr const char *NifmMetaPath = "sdmc:/wgnx/probe-mitm-nifm-meta.json";
constexpr const char *NifmBinaryPath = "sdmc:/wgnx/probe-mitm-nifm.bin";
constexpr const char *BsdTracePath = "sdmc:/wgnx/probe-mitm-bsd.jsonl";
constexpr const char *BsdMetaPath = "sdmc:/wgnx/probe-mitm-bsd-meta.json";
constexpr const char *BsdBinaryPath = "sdmc:/wgnx/probe-mitm-bsd.bin";
constexpr const char *SslTracePath = "sdmc:/wgnx/probe-mitm-ssl.jsonl";
constexpr const char *SslMetaPath = "sdmc:/wgnx/probe-mitm-ssl-meta.json";
constexpr const char *SslBinaryPath = "sdmc:/wgnx/probe-mitm-ssl.bin";
constexpr u32 InHeaderMagic = ::ams::util::FourCC<'S', 'F', 'C', 'I'>::Code;
constexpr u32 OutHeaderMagic = ::ams::util::FourCC<'S', 'F', 'C', 'O'>::Code;
constexpr u32 BinaryTraceMagic = ::ams::util::FourCC<'W', 'G', 'T', 'B'>::Code;
constexpr size_t HexPreviewBytes = 64;
constexpr size_t SemanticPayloadBytes = 64;
constexpr size_t MaxTrackedDomainPaths = 256;
constexpr size_t MaxDomainPathLength = 96;
constexpr size_t MaxTrackedSessions = 64;
constexpr size_t BinaryPayloadChunkBytes = 1536;
constexpr u32 BsdCommandPoll = 6;
constexpr u16 BsdAddressFamilyInet = 2;
constexpr u16 BsdAddressFamilyInet6 = 28;
constexpr u16 BsdSocketLevel = 0xFFFF;
constexpr s32 BsdFcntlGetFl = 3;
constexpr s32 BsdFcntlSetFl = 4;

std::atomic<u64> g_next_session_id{1};
std::atomic<u64> g_next_request_id{1};
bool g_initialized = false;
u64 g_run_tick = 0;
ams::os::SdkMutex g_trace_lock;
ams::os::SdkMutex g_state_lock;

struct DomainPathEntry {
    bool used{false};
    u64 session_id{0};
    u32 object_id{0};
    char relative_path[MaxDomainPathLength]{};
};

struct SessionEntry {
    bool used{false};
    ams::sm::ServiceName service_name{};
    ams::sm::MitmProcessInfo client_info{};
    u64 session_id{0};
};

struct ResponseDecodeDetails;
bool StartsWith(const char *value, const char *prefix);
void GetNifmObjectKindAndCommandName(
    const char *object_path,
    u32 command_id,
    char *object_kind,
    size_t object_kind_size,
    char *command_name,
    size_t command_name_size);

enum class TraceFamily : u8 {
    Broadcast = 0,
    Nifm,
    Bsd,
    Ssl,
};

TraceFamily GetTraceFamilyForServiceName(const char *service_name);

struct TraceSinkConfig {
    TraceFamily family;
    const char *family_name;
    const char *trace_path;
    const char *meta_path;
    const char *binary_path;
};

constexpr TraceSinkConfig g_trace_sinks[] = {
    { TraceFamily::Nifm, "nifm", NifmTracePath, NifmMetaPath, NifmBinaryPath },
    { TraceFamily::Bsd,  "bsd",  BsdTracePath,  BsdMetaPath,  BsdBinaryPath },
    { TraceFamily::Ssl,  "ssl",  SslTracePath,  SslMetaPath,  SslBinaryPath },
};

enum class TraceServiceCode : u16 {
    Unknown = 0,
    NifmU = 0x101,
    NifmS = 0x102,
    BsdU = 0x201,
    BsdS = 0x202,
    BsdA = 0x203,
    Ssl = 0x301,
    SslS = 0x302,
};

enum class BinaryCapturePhase : u8 {
    Request = 1,
    Response = 2,
};

enum class BinaryBufferDirection : u8 {
    Send = 1,
    Recv = 2,
    Exchange = 3,
};

enum BinaryTraceFlags : u8 {
    BinaryTraceFlag_FirstChunk = 1 << 0,
    BinaryTraceFlag_LastChunk = 1 << 1,
    BinaryTraceFlag_PointerStatic = 1 << 2,
};

struct BinaryTraceRecordHeader {
    u32 magic;
    u16 version;
    u16 header_size;
    u64 run_tick;
    u64 timestamp_ns;
    u64 request_id;
    u64 session_id;
    u64 program_id;
    u64 process_id;
    u32 object_id;
    u16 service_code;
    u16 command_id;
    u8 phase;
    u8 buffer_direction;
    u8 buffer_index;
    u8 flags;
    u32 payload_offset;
    u32 payload_size;
    u32 total_buffer_size;
} __attribute__((packed));
static_assert(sizeof(BinaryTraceRecordHeader) == 80);

std::array<DomainPathEntry, MaxTrackedDomainPaths> g_domain_paths = {};
std::array<SessionEntry, MaxTrackedSessions> g_sessions = {};

int ClampLength(int written, size_t capacity) {
    if (written <= 0) {
        return 0;
    }
    if (static_cast<size_t>(written) >= capacity) {
        return static_cast<int>(capacity - 1);
    }
    return written;
}

const TraceSinkConfig *GetTraceSinkConfig(TraceFamily family) {
    for (const auto &sink : g_trace_sinks) {
        if (sink.family == family) {
            return std::addressof(sink);
        }
    }
    return nullptr;
}

const TraceSinkConfig *GetTraceSinkConfigForServiceName(const char *service_name) {
    return GetTraceSinkConfig(GetTraceFamilyForServiceName(service_name));
}

TraceServiceCode GetTraceServiceCode(const char *service_name) {
    if (service_name == nullptr) {
        return TraceServiceCode::Unknown;
    }
    if (std::strcmp(service_name, "nifm:u") == 0) {
        return TraceServiceCode::NifmU;
    }
    if (std::strcmp(service_name, "nifm:s") == 0) {
        return TraceServiceCode::NifmS;
    }
    if (std::strcmp(service_name, "bsd:u") == 0) {
        return TraceServiceCode::BsdU;
    }
    if (std::strcmp(service_name, "bsd:s") == 0) {
        return TraceServiceCode::BsdS;
    }
    if (std::strcmp(service_name, "bsd:a") == 0) {
        return TraceServiceCode::BsdA;
    }
    if (std::strcmp(service_name, "ssl") == 0) {
        return TraceServiceCode::Ssl;
    }
    if (std::strcmp(service_name, "ssl:s") == 0) {
        return TraceServiceCode::SslS;
    }
    return TraceServiceCode::Unknown;
}

TraceFamily GetTraceFamilyForServiceName(const char *service_name) {
    if (StartsWith(service_name, "nifm:")) {
        return TraceFamily::Nifm;
    }
    if (StartsWith(service_name, "bsd:")) {
        return TraceFamily::Bsd;
    }
    if (StartsWith(service_name, "ssl")) {
        return TraceFamily::Ssl;
    }
    return TraceFamily::Broadcast;
}

void AppendLineToPath(const char *path, const char *line) {
    if (line == nullptr || line[0] == '\0') {
        return;
    }

    wgnx::net_probe::logger::AppendLine(path, line);
}

void AppendLine(TraceFamily family, const char *line) {
    if (family == TraceFamily::Broadcast) {
        for (const auto &sink : g_trace_sinks) {
            AppendLineToPath(sink.trace_path, line);
        }
        return;
    }

    if (const auto *sink = GetTraceSinkConfig(family); sink != nullptr) {
        AppendLineToPath(sink->trace_path, line);
    }
}

void AppendBinaryRecord(const char *path, const void *data, size_t size) {
    if (path == nullptr || data == nullptr || size == 0) {
        return;
    }

    wgnx::net_probe::logger::AppendBytes(path, data, size);
}

void WriteMetaFile(const TraceSinkConfig &sink) {
    char meta[512];
    const int written = std::snprintf(
        meta,
        sizeof(meta),
        "{\n"
        "  \"schema_version\":1,\n"
        "  \"run_id\":\"tick-%llu\",\n"
        "  \"trace_family\":\"%s\",\n"
        "  \"firmware\":\"unknown\",\n"
        "  \"scenario\":\"unknown\",\n"
        "  \"build\":{\"module\":\"net-probe-mitm\",\"git_rev\":\"%s\"}\n"
        "}\n",
        static_cast<unsigned long long>(g_run_tick),
        sink.family_name,
        BUILD_ID);
    if (written <= 0) {
        return;
    }

    static_cast<void>(wgnx::net_probe::fs_runtime::WriteTextFile(
        sink.meta_path,
        std::string_view(meta, static_cast<size_t>(ClampLength(written, sizeof(meta))))));
}

void EncodeServiceName(char *out, size_t out_size, const ams::sm::ServiceName &service_name) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    size_t length = 0;
    while (length < ams::sm::ServiceName::MaxLength && service_name.name[length] != '\0') {
        ++length;
    }

    if (length == 0) {
        std::snprintf(out, out_size, "<invalid>");
        return;
    }

    std::snprintf(out, out_size, "%.*s", static_cast<int>(length), service_name.name);
}

void FormatProgramId(char *out, size_t out_size, ams::ncm::ProgramId program_id) {
    std::snprintf(out, out_size, "0x%016llX", static_cast<unsigned long long>(program_id.value));
}

const char *GetSmLifecycleOperation(ams::sf::hipc::mitm_monitor::SmLifecycleEventType event_type) {
    using EventType = ams::sf::hipc::mitm_monitor::SmLifecycleEventType;
    switch (event_type) {
        case EventType::InstallBegin:
        case EventType::InstallEnd:
            return "install";
        case EventType::UninstallBegin:
        case EventType::UninstallEnd:
            return "uninstall";
        case EventType::DeclareFutureBegin:
        case EventType::DeclareFutureEnd:
            return "declare_future";
        case EventType::ClearFutureBegin:
        case EventType::ClearFutureEnd:
            return "clear_future";
        case EventType::AcknowledgeBegin:
        case EventType::AcknowledgeEnd:
            return "acknowledge";
        case EventType::HasBegin:
        case EventType::HasEnd:
            return "has_mitm";
        case EventType::WaitBegin:
        case EventType::WaitEnd:
            return "wait_mitm";
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

const char *GetSmLifecyclePhase(ams::sf::hipc::mitm_monitor::SmLifecycleEventType event_type) {
    using EventType = ams::sf::hipc::mitm_monitor::SmLifecycleEventType;
    switch (event_type) {
        case EventType::InstallBegin:
        case EventType::UninstallBegin:
        case EventType::DeclareFutureBegin:
        case EventType::ClearFutureBegin:
        case EventType::AcknowledgeBegin:
        case EventType::HasBegin:
        case EventType::WaitBegin:
            return "begin";
        case EventType::InstallEnd:
        case EventType::UninstallEnd:
        case EventType::DeclareFutureEnd:
        case EventType::ClearFutureEnd:
        case EventType::AcknowledgeEnd:
        case EventType::HasEnd:
        case EventType::WaitEnd:
            return "end";
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

const char *GetDispatchEventName(ams::sf::hipc::mitm_monitor::DispatchTraceEventType event_type) {
    using EventType = ams::sf::hipc::mitm_monitor::DispatchTraceEventType;
    switch (event_type) {
        case EventType::RootUnknownCommandForwardBegin:
            return "root_unknown_forward_begin";
        case EventType::RootUnknownCommandForwardEnd:
            return "root_unknown_forward_end";
        case EventType::RootShouldForwardBegin:
            return "root_should_forward_begin";
        case EventType::RootShouldForwardEnd:
            return "root_should_forward_end";
        case EventType::RootLocalResult:
            return "root_local_result";
        case EventType::DomainMissingObjectForwardBegin:
            return "domain_missing_object_forward_begin";
        case EventType::DomainMissingObjectForwardEnd:
            return "domain_missing_object_forward_end";
        case EventType::DomainLocalResult:
            return "domain_local_result";
        case EventType::ManagerRequestBegin:
            return "manager_request_begin";
        case EventType::ManagerRequestEnd:
            return "manager_request_end";
        case EventType::ManagerRequestDomainOverride:
            return "manager_request_domain_override";
        case EventType::ManagerRequestBaseFallback:
            return "manager_request_base_fallback";
        case EventType::ManagerRequestCmifParseFail:
            return "manager_request_cmif_parse_fail";
        case EventType::ManagerRequestCmifHandlerLookup:
            return "manager_request_cmif_handler_lookup";
        case EventType::ManagerRequestCmifHandlerResult:
            return "manager_request_cmif_handler_result";
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

u64 GetMonotonicNs() {
    return static_cast<u64>(ams::os::ConvertToTimeSpan(ams::os::GetSystemTick()).GetNanoSeconds());
}

u64 GetDurationNs(ams::os::Tick start_tick, ams::os::Tick end_tick) {
    if (end_tick < start_tick) {
        return 0;
    }
    return static_cast<u64>(ams::os::ConvertToTimeSpan(end_tick - start_tick).GetNanoSeconds());
}

void HexEncode(const u8 *src, size_t src_size, char *out, size_t out_size) {
    static constexpr char Hex[] = "0123456789abcdef";

    if (out == nullptr || out_size == 0) {
        return;
    }

    size_t cursor = 0;
    for (size_t i = 0; i < src_size && cursor + 2 < out_size; ++i) {
        out[cursor++] = Hex[(src[i] >> 4) & 0xF];
        out[cursor++] = Hex[src[i] & 0xF];
    }
    out[cursor] = '\0';
}

void ResetDomainPathState() {
    std::scoped_lock lk(g_state_lock);
    for (auto &entry : g_domain_paths) {
        entry.used = false;
        entry.session_id = 0;
        entry.object_id = 0;
        entry.relative_path[0] = '\0';
    }
}

void ResetSessionState() {
    std::scoped_lock lk(g_state_lock);
    for (auto &entry : g_sessions) {
        entry.used = false;
        entry.service_name = {};
        entry.client_info = {};
        entry.session_id = 0;
    }
}

void ForgetDomainPathsForSessionLocked(u64 session_id) {
    for (auto &entry : g_domain_paths) {
        if (entry.used && entry.session_id == session_id) {
            entry.used = false;
            entry.session_id = 0;
            entry.object_id = 0;
            entry.relative_path[0] = '\0';
        }
    }
}

size_t CountTrackedSessionsLocked() {
    size_t count = 0;
    for (const auto &entry : g_sessions) {
        if (entry.used) {
            ++count;
        }
    }
    return count;
}

size_t RememberSessionLocked(const ams::sm::ServiceName &service_name, u64 session_id, const ams::sm::MitmProcessInfo &client_info) {
    SessionEntry *free_entry = nullptr;
    for (auto &entry : g_sessions) {
        if (entry.used && entry.session_id == session_id) {
            entry.service_name = service_name;
            entry.client_info = client_info;
            return CountTrackedSessionsLocked();
        }
        if (!entry.used && free_entry == nullptr) {
            free_entry = std::addressof(entry);
        }
    }

    if (free_entry == nullptr) {
        free_entry = std::addressof(g_sessions[0]);
        ForgetDomainPathsForSessionLocked(free_entry->session_id);
    }

    free_entry->used = true;
    free_entry->service_name = service_name;
    free_entry->client_info = client_info;
    free_entry->session_id = session_id;
    return CountTrackedSessionsLocked();
}

bool ForgetSessionLocked(u64 session_id, SessionEntry *out_entry, size_t *out_remaining_count) {
    for (auto &entry : g_sessions) {
        if (entry.used && entry.session_id == session_id) {
            if (out_entry != nullptr) {
                *out_entry = entry;
            }
            entry.used = false;
            entry.service_name = {};
            entry.client_info = {};
            entry.session_id = 0;
            ForgetDomainPathsForSessionLocked(session_id);
            if (out_remaining_count != nullptr) {
                *out_remaining_count = CountTrackedSessionsLocked();
            }
            return true;
        }
    }

    ForgetDomainPathsForSessionLocked(session_id);
    if (out_remaining_count != nullptr) {
        *out_remaining_count = CountTrackedSessionsLocked();
    }
    return false;
}

size_t CopyTrackedSessions(SessionEntry *out_entries, size_t max_entries) {
    std::scoped_lock lk(g_state_lock);
    size_t cursor = 0;
    for (const auto &entry : g_sessions) {
        if (!entry.used) {
            continue;
        }
        if (out_entries != nullptr && cursor < max_entries) {
            out_entries[cursor] = entry;
        }
        ++cursor;
    }
    return cursor;
}

void RememberDomainPath(u64 session_id, u32 object_id, const char *relative_path) {
    if (object_id == 0 || relative_path == nullptr || relative_path[0] == '\0') {
        return;
    }

    std::scoped_lock lk(g_state_lock);
    DomainPathEntry *free_entry = nullptr;
    for (auto &entry : g_domain_paths) {
        if (entry.used && entry.session_id == session_id && entry.object_id == object_id) {
            std::snprintf(entry.relative_path, sizeof(entry.relative_path), "%s", relative_path);
            return;
        }
        if (!entry.used && free_entry == nullptr) {
            free_entry = std::addressof(entry);
        }
    }

    if (free_entry == nullptr) {
        free_entry = std::addressof(g_domain_paths[0]);
    }

    free_entry->used = true;
    free_entry->session_id = session_id;
    free_entry->object_id = object_id;
    std::snprintf(free_entry->relative_path, sizeof(free_entry->relative_path), "%s", relative_path);
}

void GetRelativeDomainPath(char *out, size_t out_size, u64 session_id, u32 object_id) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (object_id == 0) {
        std::snprintf(out, out_size, "root");
        return;
    }

    std::scoped_lock lk(g_state_lock);
    for (const auto &entry : g_domain_paths) {
        if (entry.used && entry.session_id == session_id && entry.object_id == object_id) {
            std::snprintf(out, out_size, "%s", entry.relative_path);
            return;
        }
    }

    std::snprintf(out, out_size, "obj%u", object_id);
}

void FormatObjectPath(char *out, size_t out_size, const char *service_name, u64 session_id, u32 object_id) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    char relative_path[MaxDomainPathLength] = {};
    GetRelativeDomainPath(relative_path, sizeof(relative_path), session_id, object_id);
    std::snprintf(out, out_size, "%s.%s", service_name, relative_path);
}

struct ParsedRequestInfo {
    bool valid{false};
    bool is_domain{false};
    bool is_close{false};
    u32 command_type{0};
    u32 command_id{0};
    u32 object_id{0};
    size_t raw_size{0};
};

struct RequestDecodeDetails {
    bool have_raw{false};
    bool root_cmif_valid{false};
    bool domain_cmif_valid{false};
    bool root_close{false};
    bool domain_close{false};
    u32 command_type{0};
    size_t raw_size{0};
    u32 raw_words[4]{};
    ::CmifInHeader root_header{};
    ::CmifDomainInHeader domain_header{};
    ::CmifInHeader domain_cmif_header{};
    size_t payload_size{0};
    u32 payload_words[4]{};
    u8 payload_bytes[SemanticPayloadBytes]{};
    char raw_preview_hex[(HexPreviewBytes * 2) + 1]{};
    char payload_preview_hex[(HexPreviewBytes * 2) + 1]{};
};

struct ParsedResponseInfo {
    bool valid{false};
    bool is_domain{false};
    u32 result_value{0};
    size_t raw_size{0};
    u32 out_object_count{0};
};

struct ResponseDecodeDetails {
    bool have_raw{false};
    bool root_cmif_valid{false};
    bool domain_cmif_valid{false};
    bool out_object_ids_valid{false};
    size_t raw_size{0};
    u32 raw_words[4]{};
    ::CmifOutHeader root_header{};
    ::CmifDomainOutHeader domain_header{};
    ::CmifOutHeader domain_cmif_header{};
    size_t payload_size{0};
    u32 payload_words[4]{};
    u8 payload_bytes[SemanticPayloadBytes]{};
    u32 out_object_id_count{0};
    u32 out_object_ids[8]{};
    char raw_preview_hex[(HexPreviewBytes * 2) + 1]{};
    char payload_preview_hex[(HexPreviewBytes * 2) + 1]{};
};

bool StartsWith(const char *value, const char *prefix) {
    if (value == nullptr || prefix == nullptr) {
        return false;
    }

    const size_t prefix_len = std::strlen(prefix);
    return std::strncmp(value, prefix, prefix_len) == 0;
}

bool EndsWith(const char *value, const char *suffix) {
    if (value == nullptr || suffix == nullptr) {
        return false;
    }

    const size_t value_len = std::strlen(value);
    const size_t suffix_len = std::strlen(suffix);
    if (suffix_len > value_len) {
        return false;
    }

    return std::strncmp(value + (value_len - suffix_len), suffix, suffix_len) == 0;
}

const char *GetHipcCommandTypeName(u32 command_type) {
    switch (command_type) {
        case 2:
            return "close";
        case 4:
            return "request";
        case 5:
            return "control";
        case 6:
            return "request_with_context";
        default:
            return "unknown";
    }
}

const char *GetSelectedKindText(const ParsedRequestInfo &request_info) {
    if (!request_info.valid) {
        return "invalid";
    }
    if (request_info.is_close) {
        return request_info.is_domain ? "domain_close" : "root_close";
    }
    return request_info.is_domain ? "domain" : "root";
}

int CountCmdSegments(const char *object_path) {
    if (object_path == nullptr) {
        return 0;
    }

    int count = 0;
    const char *cursor = object_path;
    while ((cursor = std::strstr(cursor, ".cmd")) != nullptr) {
        ++count;
        cursor += 4;
    }
    return count;
}

int GetLastCreatorCommandId(const char *object_path) {
    if (object_path == nullptr) {
        return -1;
    }

    const char *last = nullptr;
    const char *cursor = object_path;
    while ((cursor = std::strstr(cursor, ".cmd")) != nullptr) {
        last = cursor;
        cursor += 4;
    }

    if (last == nullptr) {
        return -1;
    }

    char *end = nullptr;
    const unsigned long value = std::strtoul(last + 4, &end, 10);
    if (end == (last + 4) || end == nullptr || *end != '[') {
        return -1;
    }

    return static_cast<int>(value);
}

void FormatIpv4(char *out, size_t out_size, const u8 *bytes, size_t size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    if (bytes == nullptr || size < 4) {
        std::snprintf(out, out_size, "unknown");
        return;
    }

    std::snprintf(out, out_size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
}

u16 ReadLe16(const u8 *bytes) {
    return static_cast<u16>(bytes[0] | (static_cast<u16>(bytes[1]) << 8));
}

u16 ReadBe16(const u8 *bytes) {
    return static_cast<u16>((static_cast<u16>(bytes[0]) << 8) | bytes[1]);
}

u32 ReadBe32(const u8 *bytes) {
    return (static_cast<u32>(bytes[0]) << 24) |
           (static_cast<u32>(bytes[1]) << 16) |
           (static_cast<u32>(bytes[2]) << 8) |
           static_cast<u32>(bytes[3]);
}

void FormatIpv6(char *out, size_t out_size, const u8 *bytes, size_t size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    if (bytes == nullptr || size < 16) {
        std::snprintf(out, out_size, "unknown");
        return;
    }

    std::snprintf(
        out,
        out_size,
        "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

const char *GetBsdAddressFamilyName(u16 family) {
    switch (family) {
        case BsdAddressFamilyInet: return "AF_INET";
        case BsdAddressFamilyInet6: return "AF_INET6";
        default: return nullptr;
    }
}

const char *GetBsdSocketTypeName(s32 type) {
    switch (type) {
        case 1: return "SOCK_STREAM";
        case 2: return "SOCK_DGRAM";
        case 3: return "SOCK_RAW";
        case 5: return "SOCK_SEQPACKET";
        default: return nullptr;
    }
}

const char *GetBsdProtocolName(s32 protocol) {
    switch (protocol) {
        case 6: return "TCP";
        case 17: return "UDP";
        default: return nullptr;
    }
}

const char *GetBsdSocketLevelName(s32 level) {
    switch (level) {
        case BsdSocketLevel: return "SOL_SOCKET";
        case 6: return "IPPROTO_TCP";
        case 17: return "IPPROTO_UDP";
        default: return nullptr;
    }
}

const char *GetBsdSocketOptionName(s32 level, s32 optname) {
    switch (level) {
        case BsdSocketLevel:
            switch (optname) {
                case 0x0004: return "SO_REUSEADDR";
                case 0x0008: return "SO_KEEPALIVE";
                case 0x0800: return "SO_NOSIGPIPE";
                case 0x1007: return "SO_ERROR";
                default: return nullptr;
            }
        case 6:
            switch (optname) {
                case 1: return "TCP_NODELAY";
                default: return nullptr;
            }
        default:
            return nullptr;
    }
}

const char *GetBsdFcntlCommandName(s32 cmd) {
    switch (cmd) {
        case BsdFcntlGetFl: return "F_GETFL";
        case BsdFcntlSetFl: return "F_SETFL";
        default: return nullptr;
    }
}

const char *GetBsdShutdownHowName(s32 how) {
    switch (how) {
        case 0: return "SHUT_RD";
        case 1: return "SHUT_WR";
        case 2: return "SHUT_RDWR";
        default: return nullptr;
    }
}

void AppendSummaryText(char *out, size_t out_size, const char *fmt, ...) {
    if (out == nullptr || out_size == 0 || fmt == nullptr) {
        return;
    }

    size_t cursor = std::strlen(out);
    if (cursor >= out_size - 1) {
        return;
    }

    if (cursor != 0) {
        out[cursor++] = ' ';
        out[cursor] = '\0';
    }

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out + cursor, out_size - cursor, fmt, args);
    va_end(args);
}

void AppendNamedValue(char *out, size_t out_size, const char *label, s32 value, const char *name) {
    if (name != nullptr) {
        AppendSummaryText(out, out_size, "%s=%s(%d)", label, name, value);
    } else {
        AppendSummaryText(out, out_size, "%s=%d", label, value);
    }
}

void AppendNamedHexValue(char *out, size_t out_size, const char *label, u32 value, const char *name) {
    if (name != nullptr) {
        AppendSummaryText(out, out_size, "%s=%s(0x%X)", label, name, value);
    } else {
        AppendSummaryText(out, out_size, "%s=0x%X", label, value);
    }
}

bool TryFormatSockaddr(char *out, size_t out_size, const u8 *bytes, size_t size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (bytes == nullptr || size < 2) {
        return false;
    }

    auto format_unknown = [&](u16 family, size_t preview_size) {
        char preview[(HexPreviewBytes * 2) + 1] = {};
        HexEncode(bytes, std::min(preview_size, size), preview, sizeof(preview));
        std::snprintf(out, out_size, "family=%u raw=%s", family, preview);
    };

    if (size >= 8 && bytes[1] == BsdAddressFamilyInet && bytes[0] >= 8) {
        char ip[32] = {};
        FormatIpv4(ip, sizeof(ip), bytes + 4, size - 4);
        std::snprintf(out, out_size, "family=AF_INET addr=%s port=%u", ip, ReadBe16(bytes + 2));
        return true;
    }

    if (size >= 8 && ReadLe16(bytes) == BsdAddressFamilyInet) {
        char ip[32] = {};
        FormatIpv4(ip, sizeof(ip), bytes + 4, size - 4);
        std::snprintf(out, out_size, "family=AF_INET addr=%s port=%u", ip, ReadBe16(bytes + 2));
        return true;
    }

    if (size >= 28 && bytes[1] == BsdAddressFamilyInet6 && bytes[0] >= 28) {
        char ip[64] = {};
        FormatIpv6(ip, sizeof(ip), bytes + 8, size - 8);
        std::snprintf(
            out,
            out_size,
            "family=AF_INET6 addr=%s port=%u flow=0x%08X scope=%u",
            ip,
            ReadBe16(bytes + 2),
            ReadBe32(bytes + 4),
            ReadBe32(bytes + 24));
        return true;
    }

    if (size >= 28 && ReadLe16(bytes) == BsdAddressFamilyInet6) {
        char ip[64] = {};
        FormatIpv6(ip, sizeof(ip), bytes + 8, size - 8);
        std::snprintf(
            out,
            out_size,
            "family=AF_INET6 addr=%s port=%u flow=0x%08X scope=%u",
            ip,
            ReadBe16(bytes + 2),
            ReadBe32(bytes + 4),
            ReadBe32(bytes + 24));
        return true;
    }

    if (size >= 2 && bytes[0] >= 2) {
        if (const char *family_name = GetBsdAddressFamilyName(bytes[1]); family_name != nullptr) {
            char preview[(HexPreviewBytes * 2) + 1] = {};
            HexEncode(bytes, std::min<size_t>(16, size), preview, sizeof(preview));
            std::snprintf(out, out_size, "family=%s raw=%s", family_name, preview);
            return true;
        }
    }

    const u16 family = ReadLe16(bytes);
    format_unknown(family, 16);
    return true;
}

bool GetBufferView(const ::HipcBufferDescriptor &desc, const u8 **out_bytes, size_t *out_size) {
    if (out_bytes == nullptr || out_size == nullptr) {
        return false;
    }

    const uintptr_t address = reinterpret_cast<uintptr_t>(hipcGetBufferAddress(std::addressof(desc)));
    const size_t size = hipcGetBufferSize(std::addressof(desc));
    if (address == 0 || size == 0) {
        return false;
    }

    *out_bytes = reinterpret_cast<const u8 *>(address);
    *out_size = size;
    return true;
}

bool GetStaticView(const ::HipcStaticDescriptor &desc, const u8 **out_bytes, size_t *out_size) {
    if (out_bytes == nullptr || out_size == nullptr) {
        return false;
    }

    const uintptr_t address = reinterpret_cast<uintptr_t>(hipcGetStaticAddress(std::addressof(desc)));
    const size_t size = hipcGetStaticSize(std::addressof(desc));
    if (address == 0 || size == 0) {
        return false;
    }

    *out_bytes = reinterpret_cast<const u8 *>(address);
    *out_size = size;
    return true;
}

void EmitBinaryPayloadRecords(
    const char *service_name,
    u64 timestamp_ns,
    const ams::sm::MitmProcessInfo &client_info,
    u64 session_id,
    u32 object_id,
    u64 request_id,
    u32 command_id,
    BinaryCapturePhase phase,
    BinaryBufferDirection direction,
    u8 buffer_index,
    bool is_pointer_static,
    const u8 *bytes,
    size_t size) {
    if (service_name == nullptr || bytes == nullptr || size == 0) {
        return;
    }

    const auto *sink = GetTraceSinkConfigForServiceName(service_name);
    if (sink == nullptr || sink->binary_path == nullptr) {
        return;
    }

    alignas(BinaryTraceRecordHeader) u8 record[sizeof(BinaryTraceRecordHeader) + BinaryPayloadChunkBytes] = {};
    for (size_t offset = 0; offset < size; offset += BinaryPayloadChunkBytes) {
        const size_t chunk_size = std::min(BinaryPayloadChunkBytes, size - offset);
        BinaryTraceRecordHeader header = {};
        header.magic = BinaryTraceMagic;
        header.version = 1;
        header.header_size = sizeof(BinaryTraceRecordHeader);
        header.run_tick = g_run_tick;
        header.timestamp_ns = timestamp_ns;
        header.request_id = request_id;
        header.session_id = session_id;
        header.program_id = client_info.program_id.value;
        header.process_id = client_info.process_id.value;
        header.object_id = object_id;
        header.service_code = static_cast<u16>(GetTraceServiceCode(service_name));
        header.command_id = static_cast<u16>(command_id);
        header.phase = static_cast<u8>(phase);
        header.buffer_direction = static_cast<u8>(direction);
        header.buffer_index = buffer_index;
        header.flags = 0;
        if (offset == 0) {
            header.flags |= BinaryTraceFlag_FirstChunk;
        }
        if (offset + chunk_size >= size) {
            header.flags |= BinaryTraceFlag_LastChunk;
        }
        if (is_pointer_static) {
            header.flags |= BinaryTraceFlag_PointerStatic;
        }
        header.payload_offset = static_cast<u32>(offset);
        header.payload_size = static_cast<u32>(chunk_size);
        header.total_buffer_size = static_cast<u32>(size);

        std::memcpy(record, std::addressof(header), sizeof(header));
        std::memcpy(record + sizeof(header), bytes + offset, chunk_size);
        AppendBinaryRecord(sink->binary_path, record, sizeof(header) + chunk_size);
    }
}

bool GetFirstRequestInputView(
    const ::HipcParsedRequest &request,
    const u8 **out_bytes,
    size_t *out_size,
    bool *out_is_pointer_static) {
    if (out_is_pointer_static != nullptr) {
        *out_is_pointer_static = false;
    }

    if (request.meta.num_send_buffers > 0 &&
        GetBufferView(request.data.send_buffers[0], out_bytes, out_size)) {
        return true;
    }

    if (request.meta.num_send_statics > 0 &&
        GetStaticView(request.data.send_statics[0], out_bytes, out_size)) {
        if (out_is_pointer_static != nullptr) {
            *out_is_pointer_static = true;
        }
        return true;
    }

    if (request.meta.num_exch_buffers > 0 &&
        GetBufferView(request.data.exch_buffers[0], out_bytes, out_size)) {
        return true;
    }

    return false;
}

bool GetFirstResponseOutputView(
    const ::HipcParsedRequest &request,
    const ::HipcResponse &response,
    bool have_response,
    const u8 **out_bytes,
    size_t *out_size,
    bool *out_is_pointer_static) {
    if (out_is_pointer_static != nullptr) {
        *out_is_pointer_static = false;
    }

    if (request.meta.num_recv_buffers > 0 &&
        GetBufferView(request.data.recv_buffers[0], out_bytes, out_size)) {
        return true;
    }

    if (request.meta.num_exch_buffers > 0 &&
        GetBufferView(request.data.exch_buffers[0], out_bytes, out_size)) {
        return true;
    }

    if (have_response && response.num_statics > 0 &&
        GetStaticView(response.statics[0], out_bytes, out_size)) {
        if (out_is_pointer_static != nullptr) {
            *out_is_pointer_static = true;
        }
        return true;
    }

    return false;
}

bool TryAppendBsdSockaddrRequestSummary(
    char *out,
    size_t out_size,
    u32 command_id,
    const ::HipcParsedRequest &request) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    if (command_id != 13 && command_id != 14) {
        return false;
    }

    const u8 *bytes = nullptr;
    size_t size = 0;
    if (!GetFirstRequestInputView(request, std::addressof(bytes), std::addressof(size), nullptr)) {
        return false;
    }

    char sockaddr_text[160] = {};
    if (!TryFormatSockaddr(sockaddr_text, sizeof(sockaddr_text), bytes, size)) {
        return false;
    }

    const char *label = (command_id == 13) ? "bind" : "peer";
    AppendSummaryText(out, out_size, "%s={%s}", label, sockaddr_text);
    return true;
}

bool TryAppendBsdSockOptRequestSummary(
    char *out,
    size_t out_size,
    u32 command_id,
    const ::HipcParsedRequest &request) {
    if (out == nullptr || out_size == 0 || (command_id != 17 && command_id != 21)) {
        return false;
    }

    const u8 *bytes = nullptr;
    size_t size = 0;
    if (!GetFirstRequestInputView(request, std::addressof(bytes), std::addressof(size), nullptr)) {
        return false;
    }

    if (size >= sizeof(u32)) {
        u32 value = 0;
        std::memcpy(std::addressof(value), bytes, sizeof(value));
        const char *label = (command_id == 21) ? "optval" : "optlen_buf";
        AppendSummaryText(out, out_size, "%s_u32=%u", label, value);
        return true;
    }

    return false;
}

bool TryAppendBsdSockaddrResponseSummary(
    char *out,
    size_t out_size,
    u32 command_id,
    const ::HipcParsedRequest &request,
    const ::HipcResponse &response,
    bool have_response,
    const ResponseDecodeDetails &response_decode) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    if ((command_id != 15 && command_id != 16) || response_decode.payload_size < 12) {
        return false;
    }

    u32 out_len = 0;
    std::memcpy(std::addressof(out_len), response_decode.payload_bytes + 8, sizeof(out_len));
    if (out_len == 0) {
        return false;
    }

    const u8 *bytes = nullptr;
    size_t size = 0;
    if (!GetFirstResponseOutputView(request, response, have_response, std::addressof(bytes), std::addressof(size), nullptr)) {
        return false;
    }

    char sockaddr_text[160] = {};
    if (!TryFormatSockaddr(sockaddr_text, sizeof(sockaddr_text), bytes, std::min<size_t>(size, out_len))) {
        return false;
    }

    const char *label = (command_id == 15) ? "peer" : "local";
    AppendSummaryText(out, out_size, "%s={%s}", label, sockaddr_text);
    return true;
}

bool TryAppendBsdSockOptResponseSummary(
    char *out,
    size_t out_size,
    u32 command_id,
    const ::HipcParsedRequest &request,
    const ::HipcResponse &response,
    bool have_response) {
    if (out == nullptr || out_size == 0 || command_id != 17) {
        return false;
    }

    const u8 *bytes = nullptr;
    size_t size = 0;
    if (!GetFirstResponseOutputView(request, response, have_response, std::addressof(bytes), std::addressof(size), nullptr)) {
        return false;
    }

    if (size >= sizeof(u32)) {
        u32 value = 0;
        std::memcpy(std::addressof(value), bytes, sizeof(value));
        AppendSummaryText(out, out_size, "optval_u32=%u", value);
        return true;
    }

    return false;
}

bool ShouldSuppressBsdTrace(const char *service_name, u32 command_id) {
    return StartsWith(service_name, "bsd:") && command_id == BsdCommandPoll;
}

template <typename T>
bool ReadPod(T *out, const u8 *bytes, size_t size, size_t offset = 0) {
    if (out == nullptr || bytes == nullptr || size < offset + sizeof(T)) {
        return false;
    }

    std::memcpy(out, bytes + offset, sizeof(T));
    return true;
}

bool IsKnownNifmObjectPath(const char *object_path) {
    if (object_path == nullptr) {
        return false;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);
    if (EndsWith(object_path, ".root")) {
        return true;
    }
    if (cmd_depth == 1 && (last_creator == 4 || last_creator == 5)) {
        return true;
    }
    if (cmd_depth == 2 && last_creator == 4) {
        return true;
    }

    return false;
}

void GetNifmObjectKindAndCommandName(
    const char *object_path,
    u32 command_id,
    char *object_kind,
    size_t object_kind_size,
    char *command_name,
    size_t command_name_size) {
    if (object_kind != nullptr && object_kind_size > 0) {
        std::snprintf(object_kind, object_kind_size, "Unknown");
    }
    if (command_name != nullptr && command_name_size > 0) {
        std::snprintf(command_name, command_name_size, "Unknown");
    }

    if (object_path == nullptr) {
        return;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);

    if (EndsWith(object_path, ".root")) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(object_kind, object_kind_size, "IStaticService");
        }
        if (command_name != nullptr && command_name_size > 0) {
            switch (command_id) {
                case 4:
                    std::snprintf(command_name, command_name_size, "CreateGeneralServiceOld");
                    break;
                case 5:
                    std::snprintf(command_name, command_name_size, "CreateGeneralService");
                    break;
            }
        }
        return;
    }

    if (cmd_depth == 1 && (last_creator == 4 || last_creator == 5)) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(object_kind, object_kind_size, "IGeneralService");
        }
        if (command_name != nullptr && command_name_size > 0) {
            switch (command_id) {
                case 4:
                    std::snprintf(command_name, command_name_size, "CreateRequest");
                    break;
                case 5:
                    std::snprintf(command_name, command_name_size, "GetCurrentNetworkProfile");
                    break;
                case 12:
                    std::snprintf(command_name, command_name_size, "GetCurrentIpAddress");
                    break;
                case 15:
                    std::snprintf(command_name, command_name_size, "GetCurrentIpConfigInfo");
                    break;
                case 17:
                    std::snprintf(command_name, command_name_size, "IsWirelessCommunicationEnabled");
                    break;
                case 18:
                    std::snprintf(command_name, command_name_size, "GetInternetConnectionStatus");
                    break;
                case 36:
                    std::snprintf(command_name, command_name_size, "GetCurrentAccessPoint");
                    break;
            }
        }
        return;
    }

    if (cmd_depth == 2 && last_creator == 4) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(object_kind, object_kind_size, "IRequest");
        }
        if (command_name != nullptr && command_name_size > 0) {
            switch (command_id) {
                case 0:
                    std::snprintf(command_name, command_name_size, "GetRequestState");
                    break;
                case 1:
                    std::snprintf(command_name, command_name_size, "GetResult");
                    break;
                case 2:
                    std::snprintf(command_name, command_name_size, "GetSystemEventReadableHandles");
                    break;
                case 4:
                    std::snprintf(command_name, command_name_size, "Submit");
                    break;
                case 6:
                    std::snprintf(command_name, command_name_size, "SetRequirementPreset");
                    break;
                case 12:
                    std::snprintf(command_name, command_name_size, "SetPersistent");
                    break;
            }
        }
    }
}

void FormatNifmSemanticRequestSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const RequestDecodeDetails &request_decode) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (object_path == nullptr || !IsKnownNifmObjectPath(object_path)) {
        return;
    }

    if (EndsWith(object_path, ".root") && command_id == 5 && request_decode.payload_size >= sizeof(u64)) {
        u64 reserved_pid = 0;
        std::memcpy(std::addressof(reserved_pid), request_decode.payload_bytes, sizeof(reserved_pid));
        std::snprintf(out, out_size, "reserved_pid=0x%016llX", static_cast<unsigned long long>(reserved_pid));
        return;
    }

    if (CountCmdSegments(object_path) == 1 && command_id == 4 && request_decode.payload_size >= sizeof(u32)) {
        std::snprintf(out, out_size, "requirement_preset=%u", request_decode.payload_words[0]);
        return;
    }

    if (CountCmdSegments(object_path) == 2 && GetLastCreatorCommandId(object_path) == 4) {
        switch (command_id) {
            case 6:
                if (request_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "requirement_preset=%u", request_decode.payload_words[0]);
                }
                break;
            case 12:
                if (request_decode.payload_size >= 1) {
                    std::snprintf(out, out_size, "persistent=%s", request_decode.payload_bytes[0] ? "true" : "false");
                }
                break;
        }
    }
}

void FormatNifmSemanticResponseSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const ParsedResponseInfo &response_info,
    const ResponseDecodeDetails &response_decode) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (object_path == nullptr || !IsKnownNifmObjectPath(object_path)) {
        return;
    }

    if (CountCmdSegments(object_path) == 1) {
        switch (command_id) {
            case 12:
                if (response_decode.payload_size >= 4) {
                    char ip[32];
                    FormatIpv4(ip, sizeof(ip), response_decode.payload_bytes, response_decode.payload_size);
                    std::snprintf(out, out_size, "ipv4=%s", ip);
                }
                return;
            case 15:
                if (response_decode.payload_size >= 22) {
                    char ip[32];
                    char mask[32];
                    char gateway[32];
                    char dns_preferred[32];
                    char dns_alternate[32];
                    FormatIpv4(ip, sizeof(ip), response_decode.payload_bytes + 1, response_decode.payload_size - 1);
                    FormatIpv4(mask, sizeof(mask), response_decode.payload_bytes + 5, response_decode.payload_size - 5);
                    FormatIpv4(gateway, sizeof(gateway), response_decode.payload_bytes + 9, response_decode.payload_size - 9);
                    FormatIpv4(dns_preferred, sizeof(dns_preferred), response_decode.payload_bytes + 14, response_decode.payload_size - 14);
                    FormatIpv4(dns_alternate, sizeof(dns_alternate), response_decode.payload_bytes + 18, response_decode.payload_size - 18);
                    std::snprintf(
                        out,
                        out_size,
                        "ip_auto=%u ip=%s mask=%s gateway=%s dns_auto=%u dns_pref=%s dns_alt=%s",
                        response_decode.payload_bytes[0],
                        ip,
                        mask,
                        gateway,
                        response_decode.payload_bytes[13],
                        dns_preferred,
                        dns_alternate);
                }
                return;
            case 17:
                if (response_decode.payload_size >= 1) {
                    std::snprintf(out, out_size, "enabled=%s", response_decode.payload_bytes[0] ? "true" : "false");
                }
                return;
            case 18:
                if (response_decode.payload_size >= 3) {
                    std::snprintf(
                        out,
                        out_size,
                        "type=%u wifi_strength=%u status=%u",
                        response_decode.payload_bytes[0],
                        response_decode.payload_bytes[1],
                        response_decode.payload_bytes[2]);
                }
                return;
        }
    }

    if (CountCmdSegments(object_path) == 2 && GetLastCreatorCommandId(object_path) == 4) {
        switch (command_id) {
            case 0:
                if (response_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "state=%u", response_decode.payload_words[0]);
                }
                return;
            case 1:
                std::snprintf(out, out_size, "result=0x%08X", response_info.result_value);
                return;
            case 2:
                std::snprintf(out, out_size, "copy_handles=%u", response_info.out_object_count);
                return;
        }
    }
}

bool IsKnownBsdObjectPath(const char *object_path) {
    return object_path != nullptr && EndsWith(object_path, ".root");
}

void GetBsdObjectKindAndCommandName(
    const char *object_path,
    u32 command_id,
    char *object_kind,
    size_t object_kind_size,
    char *command_name,
    size_t command_name_size) {
    if (object_kind != nullptr && object_kind_size > 0) {
        std::snprintf(object_kind, object_kind_size, "Unknown");
    }
    if (command_name != nullptr && command_name_size > 0) {
        std::snprintf(command_name, command_name_size, "Unknown");
    }

    if (!IsKnownBsdObjectPath(object_path)) {
        return;
    }

    if (object_kind != nullptr && object_kind_size > 0) {
        std::snprintf(object_kind, object_kind_size, "IBsdService");
    }
    if (command_name == nullptr || command_name_size == 0) {
        return;
    }

    switch (command_id) {
        case 0: std::snprintf(command_name, command_name_size, "RegisterClient"); break;
        case 1: std::snprintf(command_name, command_name_size, "StartMonitoring"); break;
        case 2: std::snprintf(command_name, command_name_size, "Socket"); break;
        case 3: std::snprintf(command_name, command_name_size, "SocketExempt"); break;
        case 4: std::snprintf(command_name, command_name_size, "Open"); break;
        case 5: std::snprintf(command_name, command_name_size, "Select"); break;
        case 6: std::snprintf(command_name, command_name_size, "Poll"); break;
        case 7: std::snprintf(command_name, command_name_size, "Sysctl"); break;
        case 8: std::snprintf(command_name, command_name_size, "Recv"); break;
        case 9: std::snprintf(command_name, command_name_size, "RecvFrom"); break;
        case 10: std::snprintf(command_name, command_name_size, "Send"); break;
        case 11: std::snprintf(command_name, command_name_size, "SendTo"); break;
        case 12: std::snprintf(command_name, command_name_size, "Accept"); break;
        case 13: std::snprintf(command_name, command_name_size, "Bind"); break;
        case 14: std::snprintf(command_name, command_name_size, "Connect"); break;
        case 15: std::snprintf(command_name, command_name_size, "GetPeerName"); break;
        case 16: std::snprintf(command_name, command_name_size, "GetSockName"); break;
        case 17: std::snprintf(command_name, command_name_size, "GetSockOpt"); break;
        case 18: std::snprintf(command_name, command_name_size, "Listen"); break;
        case 19: std::snprintf(command_name, command_name_size, "Ioctl"); break;
        case 20: std::snprintf(command_name, command_name_size, "Fcntl"); break;
        case 21: std::snprintf(command_name, command_name_size, "SetSockOpt"); break;
        case 22: std::snprintf(command_name, command_name_size, "Shutdown"); break;
        case 23: std::snprintf(command_name, command_name_size, "ShutdownAllSockets"); break;
        case 24: std::snprintf(command_name, command_name_size, "Write"); break;
        case 25: std::snprintf(command_name, command_name_size, "Read"); break;
        case 26: std::snprintf(command_name, command_name_size, "Close"); break;
        case 27: std::snprintf(command_name, command_name_size, "DuplicateSocket"); break;
        case 29: std::snprintf(command_name, command_name_size, "RecvMMsg"); break;
        case 30: std::snprintf(command_name, command_name_size, "SendMMsg"); break;
        default: break;
    }
}

bool IsKnownSslObjectPath(const char *object_path) {
    if (object_path == nullptr) {
        return false;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);
    if (EndsWith(object_path, ".root")) {
        return true;
    }
    if (cmd_depth == 1 && (last_creator == 0 || last_creator == 100)) {
        return true;
    }
    if (cmd_depth == 2 && (last_creator == 2 || last_creator == 100)) {
        return true;
    }

    return false;
}

void GetSslObjectKindAndCommandName(
    const char *service_name,
    const char *object_path,
    u32 command_id,
    char *object_kind,
    size_t object_kind_size,
    char *command_name,
    size_t command_name_size) {
    if (object_kind != nullptr && object_kind_size > 0) {
        std::snprintf(object_kind, object_kind_size, "Unknown");
    }
    if (command_name != nullptr && command_name_size > 0) {
        std::snprintf(command_name, command_name_size, "Unknown");
    }

    if (!IsKnownSslObjectPath(object_path)) {
        return;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);

    if (EndsWith(object_path, ".root")) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(
                object_kind,
                object_kind_size,
                "%s",
                std::strcmp(service_name, "ssl:s") == 0 ? "ISslServiceForSystem" : "ISslService");
        }
        if (command_name == nullptr || command_name_size == 0) {
            return;
        }

        switch (command_id) {
            case 0: std::snprintf(command_name, command_name_size, "CreateContext"); break;
            case 1: std::snprintf(command_name, command_name_size, "GetContextCount"); break;
            case 2: std::snprintf(command_name, command_name_size, "GetCertificates"); break;
            case 3: std::snprintf(command_name, command_name_size, "GetCertificateBufSize"); break;
            case 4: std::snprintf(command_name, command_name_size, "DebugIoctl"); break;
            case 5: std::snprintf(command_name, command_name_size, "SetInterfaceVersion"); break;
            case 6: std::snprintf(command_name, command_name_size, "FlushSessionCache"); break;
            case 7: std::snprintf(command_name, command_name_size, "SetDebugOption"); break;
            case 8: std::snprintf(command_name, command_name_size, "GetDebugOption"); break;
            case 9: std::snprintf(command_name, command_name_size, "ClearTls12FallbackFlag"); break;
            case 10: std::snprintf(command_name, command_name_size, "GetCertificateByIndex"); break;
            case 11: std::snprintf(command_name, command_name_size, "GetTrustedCertificateCount"); break;
            case 100: std::snprintf(command_name, command_name_size, "CreateContextForSystem"); break;
            case 101: std::snprintf(command_name, command_name_size, "SetThreadCoreMask"); break;
            case 102: std::snprintf(command_name, command_name_size, "GetThreadCoreMask"); break;
            case 103: std::snprintf(command_name, command_name_size, "VerifySignature"); break;
            default: break;
        }
        return;
    }

    if (cmd_depth == 1 && (last_creator == 0 || last_creator == 100)) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(
                object_kind,
                object_kind_size,
                "%s",
                last_creator == 100 ? "ISslContextForSystem" : "ISslContext");
        }
        if (command_name == nullptr || command_name_size == 0) {
            return;
        }

        switch (command_id) {
            case 0: std::snprintf(command_name, command_name_size, "SetOption"); break;
            case 1: std::snprintf(command_name, command_name_size, "GetOption"); break;
            case 2: std::snprintf(command_name, command_name_size, "CreateConnection"); break;
            case 3: std::snprintf(command_name, command_name_size, "GetConnectionCount"); break;
            case 4: std::snprintf(command_name, command_name_size, "ImportServerPki"); break;
            case 5: std::snprintf(command_name, command_name_size, "ImportClientPki"); break;
            case 6: std::snprintf(command_name, command_name_size, "RemoveServerPki"); break;
            case 7: std::snprintf(command_name, command_name_size, "RemoveClientPki"); break;
            case 8: std::snprintf(command_name, command_name_size, "RegisterInternalPki"); break;
            case 9: std::snprintf(command_name, command_name_size, "AddPolicyOid"); break;
            case 10: std::snprintf(command_name, command_name_size, "ImportCrl"); break;
            case 11: std::snprintf(command_name, command_name_size, "RemoveCrl"); break;
            case 12: std::snprintf(command_name, command_name_size, "ImportClientCertKeyPki"); break;
            case 13: std::snprintf(command_name, command_name_size, "GeneratePrivateKeyAndCert"); break;
            case 100: std::snprintf(command_name, command_name_size, "CreateConnectionEx"); break;
            default: break;
        }
        return;
    }

    if (cmd_depth == 2 && (last_creator == 2 || last_creator == 100)) {
        if (object_kind != nullptr && object_kind_size > 0) {
            std::snprintf(object_kind, object_kind_size, "ISslConnection");
        }
        if (command_name == nullptr || command_name_size == 0) {
            return;
        }

        switch (command_id) {
            case 0: std::snprintf(command_name, command_name_size, "SetSocketDescriptor"); break;
            case 1: std::snprintf(command_name, command_name_size, "SetHostName"); break;
            case 2: std::snprintf(command_name, command_name_size, "SetVerifyOption"); break;
            case 3: std::snprintf(command_name, command_name_size, "SetIoMode"); break;
            case 4: std::snprintf(command_name, command_name_size, "GetSocketDescriptor"); break;
            case 5: std::snprintf(command_name, command_name_size, "GetHostName"); break;
            case 6: std::snprintf(command_name, command_name_size, "GetVerifyOption"); break;
            case 7: std::snprintf(command_name, command_name_size, "GetIoMode"); break;
            case 8: std::snprintf(command_name, command_name_size, "DoHandshake"); break;
            case 9: std::snprintf(command_name, command_name_size, "DoHandshakeGetServerCert"); break;
            case 10: std::snprintf(command_name, command_name_size, "Read"); break;
            case 11: std::snprintf(command_name, command_name_size, "Write"); break;
            case 12: std::snprintf(command_name, command_name_size, "Pending"); break;
            case 13: std::snprintf(command_name, command_name_size, "Peek"); break;
            case 14: std::snprintf(command_name, command_name_size, "Poll"); break;
            case 15: std::snprintf(command_name, command_name_size, "GetVerifyCertError"); break;
            case 16: std::snprintf(command_name, command_name_size, "GetNeededServerCertBufferSize"); break;
            case 17: std::snprintf(command_name, command_name_size, "SetSessionCacheMode"); break;
            case 18: std::snprintf(command_name, command_name_size, "GetSessionCacheMode"); break;
            case 19: std::snprintf(command_name, command_name_size, "FlushSessionCache"); break;
            case 20: std::snprintf(command_name, command_name_size, "SetRenegotiationMode"); break;
            case 21: std::snprintf(command_name, command_name_size, "GetRenegotiationMode"); break;
            case 22: std::snprintf(command_name, command_name_size, "SetOption"); break;
            case 23: std::snprintf(command_name, command_name_size, "GetOption"); break;
            case 24: std::snprintf(command_name, command_name_size, "GetVerifyCertErrors"); break;
            case 25: std::snprintf(command_name, command_name_size, "GetCipherInfo"); break;
            case 26: std::snprintf(command_name, command_name_size, "SetNextAlpnProto"); break;
            case 27: std::snprintf(command_name, command_name_size, "GetNextAlpnProto"); break;
            case 28: std::snprintf(command_name, command_name_size, "SetDtlsSocketDescriptor"); break;
            case 29: std::snprintf(command_name, command_name_size, "GetDtlsHandshakeTimeout"); break;
            case 30: std::snprintf(command_name, command_name_size, "SetPrivateOption"); break;
            case 31: std::snprintf(command_name, command_name_size, "SetSrtpCiphers"); break;
            case 32: std::snprintf(command_name, command_name_size, "GetSrtpCipher"); break;
            case 33: std::snprintf(command_name, command_name_size, "ExportKeyingMaterial"); break;
            case 34: std::snprintf(command_name, command_name_size, "SetIoTimeout"); break;
            case 35: std::snprintf(command_name, command_name_size, "GetIoTimeout"); break;
            case 36: std::snprintf(command_name, command_name_size, "GetSessionTicket"); break;
            case 37: std::snprintf(command_name, command_name_size, "SetSessionTicket"); break;
            default: break;
        }
    }
}

void DescribeKnownObjectAndCommand(
    const char *service_name,
    const char *object_path,
    u32 command_id,
    char *object_kind,
    size_t object_kind_size,
    char *command_name,
    size_t command_name_size) {
    if (object_kind != nullptr && object_kind_size > 0) {
        std::snprintf(object_kind, object_kind_size, "Unknown");
    }
    if (command_name != nullptr && command_name_size > 0) {
        std::snprintf(command_name, command_name_size, "Unknown");
    }

    if (service_name == nullptr) {
        return;
    }

    if (StartsWith(service_name, "nifm:")) {
        GetNifmObjectKindAndCommandName(object_path, command_id, object_kind, object_kind_size, command_name, command_name_size);
        return;
    }

    if (StartsWith(service_name, "bsd:")) {
        GetBsdObjectKindAndCommandName(object_path, command_id, object_kind, object_kind_size, command_name, command_name_size);
        return;
    }

    if (StartsWith(service_name, "ssl")) {
        GetSslObjectKindAndCommandName(service_name, object_path, command_id, object_kind, object_kind_size, command_name, command_name_size);
    }
}

void GetHipcManagerCommandName(char *command_name, size_t command_name_size, u32 command_id) {
    if (command_name == nullptr || command_name_size == 0) {
        return;
    }

    std::snprintf(command_name, command_name_size, "Unknown");
    switch (command_id) {
        case 0:
            std::snprintf(command_name, command_name_size, "ConvertCurrentObjectToDomain");
            break;
        case 1:
            std::snprintf(command_name, command_name_size, "CopyFromCurrentDomain");
            break;
        case 2:
            std::snprintf(command_name, command_name_size, "CloneCurrentObject");
            break;
        case 3:
            std::snprintf(command_name, command_name_size, "QueryPointerBufferSize");
            break;
        case 4:
            std::snprintf(command_name, command_name_size, "CloneCurrentObjectEx");
            break;
        default:
            break;
    }
}

void FormatBsdSemanticRequestSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const RequestDecodeDetails &request_decode) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!IsKnownBsdObjectPath(object_path)) {
        return;
    }

    switch (command_id) {
        case 0: {
            u32 version = 0;
            u32 tcp_tx = 0;
            u32 tcp_rx = 0;
            u32 tcp_tx_max = 0;
            u32 tcp_rx_max = 0;
            u32 udp_tx = 0;
            u32 udp_rx = 0;
            u32 sb_eff = 0;
            u64 tmem_size = 0;
            if (ReadPod(std::addressof(version), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(tcp_tx), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(tcp_rx), request_decode.payload_bytes, request_decode.payload_size, 8) &&
                ReadPod(std::addressof(tcp_tx_max), request_decode.payload_bytes, request_decode.payload_size, 12) &&
                ReadPod(std::addressof(tcp_rx_max), request_decode.payload_bytes, request_decode.payload_size, 16) &&
                ReadPod(std::addressof(udp_tx), request_decode.payload_bytes, request_decode.payload_size, 20) &&
                ReadPod(std::addressof(udp_rx), request_decode.payload_bytes, request_decode.payload_size, 24) &&
                ReadPod(std::addressof(sb_eff), request_decode.payload_bytes, request_decode.payload_size, 28) &&
                ReadPod(std::addressof(tmem_size), request_decode.payload_bytes, request_decode.payload_size, 40)) {
                std::snprintf(
                    out,
                    out_size,
                    "version=%u tcp_tx=0x%X tcp_rx=0x%X tcp_tx_max=0x%X tcp_rx_max=0x%X udp_tx=0x%X udp_rx=0x%X sb_eff=%u tmem_size=0x%llX",
                    version,
                    tcp_tx,
                    tcp_rx,
                    tcp_tx_max,
                    tcp_rx_max,
                    udp_tx,
                    udp_rx,
                    sb_eff,
                    static_cast<unsigned long long>(tmem_size));
            }
            return;
        }
        case 1: {
            u64 pid = 0;
            if (ReadPod(std::addressof(pid), request_decode.payload_bytes, request_decode.payload_size)) {
                std::snprintf(out, out_size, "pid=0x%016llX", static_cast<unsigned long long>(pid));
            }
            return;
        }
        case 2:
        case 3: {
            s32 domain = 0;
            s32 type = 0;
            s32 protocol = 0;
            if (ReadPod(std::addressof(domain), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(type), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(protocol), request_decode.payload_bytes, request_decode.payload_size, 8)) {
                AppendNamedValue(out, out_size, "domain", domain, GetBsdAddressFamilyName(static_cast<u16>(domain)));
                AppendNamedValue(out, out_size, "type", type, GetBsdSocketTypeName(type));
                AppendNamedValue(out, out_size, "protocol", protocol, GetBsdProtocolName(protocol));
            }
            return;
        }
        case 4: {
            s32 flags = 0;
            if (ReadPod(std::addressof(flags), request_decode.payload_bytes, request_decode.payload_size)) {
                std::snprintf(out, out_size, "flags=0x%X", static_cast<u32>(flags));
            }
            return;
        }
        case 5: {
            s32 nfds = 0;
            if (ReadPod(std::addressof(nfds), request_decode.payload_bytes, request_decode.payload_size, 0)) {
                std::snprintf(out, out_size, "nfds=%d", nfds);
            }
            return;
        }
        case 6: {
            u32 nfds = 0;
            s32 timeout = 0;
            if (ReadPod(std::addressof(nfds), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(timeout), request_decode.payload_bytes, request_decode.payload_size, 4)) {
                std::snprintf(out, out_size, "nfds=%u timeout_ms=%d", nfds, timeout);
            }
            return;
        }
        case 8:
        case 9:
        case 10:
        case 11: {
            s32 sockfd = 0;
            s32 flags = 0;
            if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(flags), request_decode.payload_bytes, request_decode.payload_size, 4)) {
                std::snprintf(out, out_size, "sockfd=%d flags=0x%X", sockfd, static_cast<u32>(flags));
            }
            return;
        }
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 24:
        case 25:
        case 26:
        case 27: {
            s32 value = 0;
            if (ReadPod(std::addressof(value), request_decode.payload_bytes, request_decode.payload_size, 0)) {
                const char *field_name = (command_id >= 24 && command_id <= 26) ? "fd" : "sockfd";
                std::snprintf(out, out_size, "%s=%d", field_name, value);
            }
            return;
        }
        case 17:
        case 21: {
            s32 sockfd = 0;
            s32 level = 0;
            s32 optname = 0;
            if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(level), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(optname), request_decode.payload_bytes, request_decode.payload_size, 8)) {
                AppendSummaryText(out, out_size, "sockfd=%d", sockfd);
                AppendNamedValue(out, out_size, "level", level, GetBsdSocketLevelName(level));
                AppendNamedHexValue(out, out_size, "optname", static_cast<u32>(optname), GetBsdSocketOptionName(level, optname));
                if (command_id == 17) {
                    u32 optlen = 0;
                    if (ReadPod(std::addressof(optlen), request_decode.payload_bytes, request_decode.payload_size, 12)) {
                        AppendSummaryText(out, out_size, "optlen=%u", optlen);
                    }
                }
            }
            return;
        }
        case 18: {
            s32 sockfd = 0;
            s32 backlog = 0;
            if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(backlog), request_decode.payload_bytes, request_decode.payload_size, 4)) {
                std::snprintf(out, out_size, "sockfd=%d backlog=%d", sockfd, backlog);
            }
            return;
        }
        case 19: {
            s32 fd = 0;
            s32 request = 0;
            s32 bufcount = 0;
            if (ReadPod(std::addressof(fd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(request), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(bufcount), request_decode.payload_bytes, request_decode.payload_size, 8)) {
                std::snprintf(out, out_size, "fd=%d request=0x%X bufcount=%d", fd, static_cast<u32>(request), bufcount);
            }
            return;
        }
        case 20: {
            s32 fd = 0;
            s32 cmd = 0;
            s32 flags = 0;
            if (ReadPod(std::addressof(fd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(cmd), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(flags), request_decode.payload_bytes, request_decode.payload_size, 8)) {
                AppendSummaryText(out, out_size, "fd=%d", fd);
                AppendNamedValue(out, out_size, "cmd", cmd, GetBsdFcntlCommandName(cmd));
                AppendSummaryText(out, out_size, "flags=0x%X", static_cast<u32>(flags));
            }
            return;
        }
        case 22: {
            s32 sockfd = 0;
            s32 how = 0;
            if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(how), request_decode.payload_bytes, request_decode.payload_size, 4)) {
                AppendSummaryText(out, out_size, "sockfd=%d", sockfd);
                AppendNamedValue(out, out_size, "how", how, GetBsdShutdownHowName(how));
            }
            return;
        }
        case 23: {
            s32 how = 0;
            if (ReadPod(std::addressof(how), request_decode.payload_bytes, request_decode.payload_size, 0)) {
                std::snprintf(out, out_size, "how=%d", how);
            }
            return;
        }
        case 29:
        case 30: {
            s32 sockfd = 0;
            s32 vlen = 0;
            s32 flags = 0;
            if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0) &&
                ReadPod(std::addressof(vlen), request_decode.payload_bytes, request_decode.payload_size, 4) &&
                ReadPod(std::addressof(flags), request_decode.payload_bytes, request_decode.payload_size, 8)) {
                std::snprintf(out, out_size, "sockfd=%d vlen=%d flags=0x%X", sockfd, vlen, static_cast<u32>(flags));
            }
            return;
        }
        default:
            return;
    }
}

void FormatBsdSemanticResponseSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const ResponseDecodeDetails &response_decode) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!IsKnownBsdObjectPath(object_path) || response_decode.payload_size < (2 * sizeof(s32))) {
        return;
    }

    s32 ret = 0;
    s32 errno_value = 0;
    if (!ReadPod(std::addressof(ret), response_decode.payload_bytes, response_decode.payload_size, 0) ||
        !ReadPod(std::addressof(errno_value), response_decode.payload_bytes, response_decode.payload_size, 4)) {
        return;
    }

    switch (command_id) {
        case 0: {
            u64 assigned_pid = 0;
            if (ReadPod(std::addressof(assigned_pid), response_decode.payload_bytes, response_decode.payload_size, 8)) {
                std::snprintf(
                    out,
                    out_size,
                    "ret=%d errno=%d assigned_pid=0x%016llX",
                    ret,
                    errno_value,
                    static_cast<unsigned long long>(assigned_pid));
                return;
            }
            break;
        }
        case 9:
        case 12:
        case 15:
        case 16:
        case 17: {
            u32 out_len = 0;
            if (ReadPod(std::addressof(out_len), response_decode.payload_bytes, response_decode.payload_size, 8)) {
                std::snprintf(out, out_size, "ret=%d errno=%d out_len=%u", ret, errno_value, out_len);
                return;
            }
            break;
        }
        default:
            break;
    }

    std::snprintf(out, out_size, "ret=%d errno=%d", ret, errno_value);
}

bool CopyTextPreview(char *out, size_t out_size, const u8 *bytes, size_t size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    if (bytes == nullptr || size == 0) {
        return false;
    }

    size_t cursor = 0;
    while (cursor + 1 < out_size && cursor < size) {
        const u8 ch = bytes[cursor];
        if (ch == 0) {
            break;
        }
        if (ch < 0x20 || ch >= 0x7F || ch == '"' || ch == '\\') {
            out[cursor] = '.';
        } else {
            out[cursor] = static_cast<char>(ch);
        }
        ++cursor;
    }
    out[cursor] = '\0';
    return cursor > 0;
}

void FormatSslSemanticRequestSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const RequestDecodeDetails &request_decode,
    const ::HipcParsedRequest *request) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!IsKnownSslObjectPath(object_path)) {
        return;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);

    if (EndsWith(object_path, ".root")) {
        switch (command_id) {
            case 0:
            case 100: {
                if (request_decode.payload_size >= (sizeof(u32) + sizeof(u64))) {
                    u32 ssl_version = 0;
                    u64 pid_placeholder = 0;
                    std::memcpy(std::addressof(ssl_version), request_decode.payload_bytes, sizeof(ssl_version));
                    std::memcpy(std::addressof(pid_placeholder), request_decode.payload_bytes + sizeof(u32), sizeof(pid_placeholder));
                    std::snprintf(
                        out,
                        out_size,
                        "ssl_version=0x%X pid_placeholder=0x%016llX",
                        ssl_version,
                        static_cast<unsigned long long>(pid_placeholder));
                }
                return;
            }
            case 5:
            case 101: {
                if (request_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "value=0x%X", request_decode.payload_words[0]);
                } else if (request_decode.payload_size >= sizeof(u64)) {
                    u64 value = 0;
                    std::memcpy(std::addressof(value), request_decode.payload_bytes, sizeof(value));
                    std::snprintf(out, out_size, "value=0x%016llX", static_cast<unsigned long long>(value));
                }
                return;
            }
            case 6:
            case 7:
            case 8:
            case 103: {
                if (request_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "value=0x%X", request_decode.payload_words[0]);
                }
                break;
            }
            default:
                break;
        }
    }

    if (cmd_depth == 1 && (last_creator == 0 || last_creator == 100)) {
        switch (command_id) {
            case 0:
            case 1:
            case 8:
            case 100:
                if (request_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "value=0x%X", request_decode.payload_words[0]);
                }
                break;
            default:
                break;
        }
    }

    if (cmd_depth == 2 && (last_creator == 2 || last_creator == 100)) {
        switch (command_id) {
            case 0:
            case 28: {
                s32 sockfd = 0;
                if (ReadPod(std::addressof(sockfd), request_decode.payload_bytes, request_decode.payload_size, 0)) {
                    AppendSummaryText(out, out_size, "sockfd=%d", sockfd);
                }
                if (command_id == 28 && request != nullptr) {
                    const u8 *bytes = nullptr;
                    size_t size = 0;
                    if (GetFirstRequestInputView(*request, std::addressof(bytes), std::addressof(size), nullptr)) {
                        char sockaddr_text[160] = {};
                        if (TryFormatSockaddr(sockaddr_text, sizeof(sockaddr_text), bytes, size)) {
                            AppendSummaryText(out, out_size, "peer={%s}", sockaddr_text);
                        }
                    }
                }
                return;
            }
            case 1: {
                if (request != nullptr) {
                    const u8 *bytes = nullptr;
                    size_t size = 0;
                    if (GetFirstRequestInputView(*request, std::addressof(bytes), std::addressof(size), nullptr)) {
                        char text[128] = {};
                        if (CopyTextPreview(text, sizeof(text), bytes, size)) {
                            AppendSummaryText(out, out_size, "hostname=%s", text);
                        }
                    }
                }
                return;
            }
            case 2:
            case 3:
            case 17:
            case 20:
            case 22:
            case 23:
            case 30:
            case 34:
                if (request_decode.payload_size >= sizeof(u32)) {
                    std::snprintf(out, out_size, "value=0x%X", request_decode.payload_words[0]);
                }
                return;
            case 10:
            case 11:
            case 13:
            case 24:
            case 27:
            case 29:
            case 33:
            case 36:
                if (request != nullptr) {
                    const u8 *bytes = nullptr;
                    size_t size = 0;
                    if (GetFirstRequestInputView(*request, std::addressof(bytes), std::addressof(size), nullptr)) {
                        std::snprintf(out, out_size, "in_buf_size=%zu", size);
                    }
                }
                return;
            default:
                return;
        }
    }
}

void FormatSslSemanticResponseSummary(
    char *out,
    size_t out_size,
    const char *object_path,
    u32 command_id,
    const ParsedResponseInfo &response_info,
    const ResponseDecodeDetails &response_decode,
    const ::HipcParsedRequest *request,
    const ::HipcResponse *response,
    bool have_response) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!IsKnownSslObjectPath(object_path)) {
        return;
    }

    const int cmd_depth = CountCmdSegments(object_path);
    const int last_creator = GetLastCreatorCommandId(object_path);

    if (response_decode.payload_size >= sizeof(u32)) {
        AppendSummaryText(out, out_size, "value0=0x%X", response_decode.payload_words[0]);
    }
    if (response_decode.payload_size >= sizeof(u64) && command_id == 102) {
        u64 value = 0;
        std::memcpy(std::addressof(value), response_decode.payload_bytes, sizeof(value));
        out[0] = '\0';
        AppendSummaryText(out, out_size, "mask=0x%016llX", static_cast<unsigned long long>(value));
        return;
    }
    if (response_info.out_object_count > 0) {
        AppendSummaryText(out, out_size, "out_objects=%u", response_info.out_object_count);
    }

    if (cmd_depth == 2 && (last_creator == 2 || last_creator == 100)) {
        switch (command_id) {
            case 0:
            case 4:
            case 28: {
                if (response_decode.payload_size >= sizeof(s32)) {
                    s32 sockfd = 0;
                    std::memcpy(std::addressof(sockfd), response_decode.payload_bytes, sizeof(sockfd));
                    out[0] = '\0';
                    AppendSummaryText(out, out_size, "sockfd=%d", sockfd);
                }
                break;
            }
            case 5:
            case 9:
            case 24:
            case 27:
            case 29:
            case 36: {
                if (request != nullptr && response != nullptr && have_response) {
                    const u8 *bytes = nullptr;
                    size_t size = 0;
                    if (GetFirstResponseOutputView(*request, *response, have_response, std::addressof(bytes), std::addressof(size), nullptr)) {
                        AppendSummaryText(out, out_size, "out_buf_size=%zu", size);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

bool GetRequestRawData(const ams::sf::cmif::PointerAndSize &message, ::HipcParsedRequest *out_request, ams::sf::cmif::PointerAndSize *out_raw) {
    if (message.GetPointer() == nullptr || message.GetSize() == 0) {
        return false;
    }

    const auto request = hipcParseRequest(message.GetPointer());
    const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(request.data.data_words);
    const size_t raw_size = request.meta.num_data_words * sizeof(u32);
    if (raw_size < 0x10) {
        return false;
    }

    const uintptr_t aligned_addr = ::ams::util::AlignUp(raw_addr, 0x10);
    if (aligned_addr + (raw_size - 0x10) > message.GetAddress() + message.GetSize()) {
        return false;
    }

    if (out_request != nullptr) {
        *out_request = request;
    }
    if (out_raw != nullptr) {
        *out_raw = ams::sf::cmif::PointerAndSize(aligned_addr, raw_size - 0x10);
    }
    return true;
}

bool GetResponseRawData(const ams::sf::cmif::PointerAndSize &message, ::HipcResponse *out_response, ams::sf::cmif::PointerAndSize *out_raw) {
    if (message.GetPointer() == nullptr || message.GetSize() == 0) {
        return false;
    }

    const auto response = hipcParseResponse(message.GetPointer());
    const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(response.data_words);
    const size_t raw_size = response.num_data_words * sizeof(u32);
    if (raw_size < 0x10) {
        return false;
    }

    const uintptr_t aligned_addr = ::ams::util::AlignUp(raw_addr, 0x10);
    if (aligned_addr + (raw_size - 0x10) > message.GetAddress() + message.GetSize()) {
        return false;
    }

    if (out_response != nullptr) {
        *out_response = response;
    }
    if (out_raw != nullptr) {
        *out_raw = ams::sf::cmif::PointerAndSize(aligned_addr, raw_size - 0x10);
    }
    return true;
}

RequestDecodeDetails GetRequestDecodeDetails(const ams::sf::cmif::PointerAndSize &message) {
    RequestDecodeDetails details = {};
    ::HipcParsedRequest request = {};
    ams::sf::cmif::PointerAndSize raw_data;
    if (!GetRequestRawData(message, std::addressof(request), std::addressof(raw_data))) {
        return details;
    }

    details.have_raw = true;
    details.command_type = request.meta.type;
    details.raw_size = raw_data.GetSize();

    const size_t preview_size = std::min(raw_data.GetSize(), HexPreviewBytes);
    HexEncode(static_cast<const u8 *>(raw_data.GetPointer()), preview_size, details.raw_preview_hex, sizeof(details.raw_preview_hex));

    const size_t raw_word_count = std::min(raw_data.GetSize() / sizeof(u32), std::size(details.raw_words));
    if (raw_word_count > 0) {
        std::memcpy(details.raw_words, raw_data.GetPointer(), raw_word_count * sizeof(u32));
    }

    if (raw_data.GetSize() >= sizeof(details.root_header)) {
        std::memcpy(std::addressof(details.root_header), raw_data.GetPointer(), sizeof(details.root_header));
        details.root_cmif_valid = details.root_header.magic == InHeaderMagic;
        details.root_close =
            !details.root_cmif_valid &&
            details.root_header.magic == ::CmifCommandType_Close &&
            raw_data.GetSize() == sizeof(details.root_header);
        if (details.root_cmif_valid) {
            const auto *payload = static_cast<const u8 *>(raw_data.GetPointer()) + sizeof(details.root_header);
            details.payload_size = raw_data.GetSize() - sizeof(details.root_header);
            const size_t payload_preview_size = std::min(details.payload_size, HexPreviewBytes);
            HexEncode(payload, payload_preview_size, details.payload_preview_hex, sizeof(details.payload_preview_hex));
            const size_t payload_word_count = std::min(details.payload_size / sizeof(u32), std::size(details.payload_words));
            if (payload_word_count > 0) {
                std::memcpy(details.payload_words, payload, payload_word_count * sizeof(u32));
            }
            const size_t payload_byte_count = std::min(details.payload_size, std::size(details.payload_bytes));
            if (payload_byte_count > 0) {
                std::memcpy(details.payload_bytes, payload, payload_byte_count);
            }
        }
    }

    if (raw_data.GetSize() >= sizeof(details.domain_header) + sizeof(details.domain_cmif_header)) {
        std::memcpy(std::addressof(details.domain_header), raw_data.GetPointer(), sizeof(details.domain_header));
        std::memcpy(
            std::addressof(details.domain_cmif_header),
            static_cast<const u8 *>(raw_data.GetPointer()) + sizeof(details.domain_header),
            sizeof(details.domain_cmif_header));
        details.domain_close = details.domain_header.type == ::CmifDomainRequestType_Close;
        details.domain_cmif_valid =
            details.domain_header.type == ::CmifDomainRequestType_SendMessage &&
            details.domain_cmif_header.magic == InHeaderMagic;
        if (details.domain_cmif_valid && details.domain_header.data_size >= sizeof(details.domain_cmif_header)) {
            const size_t payload_offset = sizeof(details.domain_header) + sizeof(details.domain_cmif_header);
            const size_t payload_size = details.domain_header.data_size - sizeof(details.domain_cmif_header);
            if (raw_data.GetSize() >= payload_offset + payload_size) {
                const auto *payload = static_cast<const u8 *>(raw_data.GetPointer()) + payload_offset;
                details.payload_size = payload_size;
                const size_t payload_preview_size = std::min(details.payload_size, HexPreviewBytes);
                HexEncode(payload, payload_preview_size, details.payload_preview_hex, sizeof(details.payload_preview_hex));
                const size_t payload_word_count = std::min(details.payload_size / sizeof(u32), std::size(details.payload_words));
                if (payload_word_count > 0) {
                    std::memcpy(details.payload_words, payload, payload_word_count * sizeof(u32));
                }
                const size_t payload_byte_count = std::min(details.payload_size, std::size(details.payload_bytes));
                if (payload_byte_count > 0) {
                    std::memcpy(details.payload_bytes, payload, payload_byte_count);
                }
            }
        }
    }

    return details;
}

ResponseDecodeDetails GetResponseDecodeDetails(const ams::sf::cmif::PointerAndSize &message) {
    ResponseDecodeDetails details = {};
    ::HipcResponse response = {};
    ams::sf::cmif::PointerAndSize raw_data;
    if (!GetResponseRawData(message, std::addressof(response), std::addressof(raw_data))) {
        return details;
    }

    details.have_raw = true;
    details.raw_size = raw_data.GetSize();

    const size_t preview_size = std::min(raw_data.GetSize(), HexPreviewBytes);
    HexEncode(static_cast<const u8 *>(raw_data.GetPointer()), preview_size, details.raw_preview_hex, sizeof(details.raw_preview_hex));

    const size_t raw_word_count = std::min(raw_data.GetSize() / sizeof(u32), std::size(details.raw_words));
    if (raw_word_count > 0) {
        std::memcpy(details.raw_words, raw_data.GetPointer(), raw_word_count * sizeof(u32));
    }

    if (raw_data.GetSize() >= sizeof(details.root_header)) {
        std::memcpy(std::addressof(details.root_header), raw_data.GetPointer(), sizeof(details.root_header));
        details.root_cmif_valid = details.root_header.magic == OutHeaderMagic;
        if (details.root_cmif_valid) {
            const auto *payload = static_cast<const u8 *>(raw_data.GetPointer()) + sizeof(details.root_header);
            details.payload_size = raw_data.GetSize() - sizeof(details.root_header);
            const size_t payload_preview_size = std::min(details.payload_size, HexPreviewBytes);
            HexEncode(payload, payload_preview_size, details.payload_preview_hex, sizeof(details.payload_preview_hex));
            const size_t payload_word_count = std::min(details.payload_size / sizeof(u32), std::size(details.payload_words));
            if (payload_word_count > 0) {
                std::memcpy(details.payload_words, payload, payload_word_count * sizeof(u32));
            }
            const size_t payload_byte_count = std::min(details.payload_size, std::size(details.payload_bytes));
            if (payload_byte_count > 0) {
                std::memcpy(details.payload_bytes, payload, payload_byte_count);
            }
        }
    }

    if (raw_data.GetSize() >= sizeof(details.domain_header) + sizeof(details.domain_cmif_header)) {
        std::memcpy(std::addressof(details.domain_header), raw_data.GetPointer(), sizeof(details.domain_header));
        std::memcpy(
            std::addressof(details.domain_cmif_header),
            static_cast<const u8 *>(raw_data.GetPointer()) + sizeof(details.domain_header),
            sizeof(details.domain_cmif_header));
        details.domain_cmif_valid = details.domain_cmif_header.magic == OutHeaderMagic;

        const u32 out_object_count = details.domain_header.num_out_objects;
        if (details.domain_cmif_valid &&
            out_object_count > 0 &&
            out_object_count <= std::size(details.out_object_ids) &&
            raw_data.GetSize() >= (sizeof(details.domain_header) + sizeof(details.domain_cmif_header) + (out_object_count * sizeof(u32)))) {
            const uintptr_t object_ids_addr = raw_data.GetAddress() + raw_data.GetSize() - (out_object_count * sizeof(u32));
            std::memcpy(details.out_object_ids, reinterpret_cast<const void *>(object_ids_addr), out_object_count * sizeof(u32));
            details.out_object_id_count = out_object_count;
            details.out_object_ids_valid = true;
        }

        if (details.domain_cmif_valid) {
            const size_t payload_offset = sizeof(details.domain_header) + sizeof(details.domain_cmif_header);
            const size_t trailer_size = details.out_object_id_count * sizeof(u32);
            if (raw_data.GetSize() >= payload_offset + trailer_size) {
                const size_t payload_size = raw_data.GetSize() - payload_offset - trailer_size;
                const auto *payload = static_cast<const u8 *>(raw_data.GetPointer()) + payload_offset;
                details.payload_size = payload_size;
                const size_t payload_preview_size = std::min(details.payload_size, HexPreviewBytes);
                HexEncode(payload, payload_preview_size, details.payload_preview_hex, sizeof(details.payload_preview_hex));
                const size_t payload_word_count = std::min(details.payload_size / sizeof(u32), std::size(details.payload_words));
                if (payload_word_count > 0) {
                    std::memcpy(details.payload_words, payload, payload_word_count * sizeof(u32));
                }
                const size_t payload_byte_count = std::min(details.payload_size, std::size(details.payload_bytes));
                if (payload_byte_count > 0) {
                    std::memcpy(details.payload_bytes, payload, payload_byte_count);
                }
            }
        }
    }

    return details;
}

ParsedRequestInfo ParseRequestInfo(const ams::sf::cmif::PointerAndSize &message) {
    ParsedRequestInfo info = {};
    const RequestDecodeDetails details = GetRequestDecodeDetails(message);
    if (!details.have_raw) {
        return info;
    }

    info.command_type = details.command_type;
    info.raw_size = details.raw_size;

    if (details.root_cmif_valid) {
        info.valid = true;
        info.command_id = details.root_header.command_id;
        return info;
    }

    if (details.root_close) {
        info.valid = true;
        info.is_close = true;
        return info;
    }

    if (details.domain_cmif_valid) {
        info.valid = true;
        info.is_domain = true;
        info.object_id = details.domain_header.object_id;
        info.command_id = details.domain_cmif_header.command_id;
        return info;
    }

    if (details.domain_close) {
        info.valid = true;
        info.is_domain = true;
        info.is_close = true;
        info.object_id = details.domain_header.object_id;
    }

    return info;
}

ParsedResponseInfo ParseResponseInfo(const ams::sf::cmif::PointerAndSize &message) {
    ParsedResponseInfo info = {};
    ::HipcResponse response = {};
    ams::sf::cmif::PointerAndSize raw_data;
    if (!GetResponseRawData(message, std::addressof(response), std::addressof(raw_data))) {
        return info;
    }

    const ResponseDecodeDetails details = GetResponseDecodeDetails(message);
    info.raw_size = raw_data.GetSize();

    if (details.root_cmif_valid) {
        info.valid = true;
        info.result_value = details.root_header.result;
        info.out_object_count = response.num_move_handles;
        return info;
    }

    if (details.domain_cmif_valid) {
        info.valid = true;
        info.is_domain = true;
        info.result_value = details.domain_cmif_header.result;
        info.out_object_count = details.domain_header.num_out_objects;
    }

    return info;
}

void RememberReturnedDomainObjects(u64 session_id, u32 parent_object_id, u32 command_id, const ResponseDecodeDetails &response_decode) {
    if (!response_decode.out_object_ids_valid || response_decode.out_object_id_count == 0) {
        return;
    }

    char parent_relative_path[MaxDomainPathLength] = {};
    GetRelativeDomainPath(parent_relative_path, sizeof(parent_relative_path), session_id, parent_object_id);

    for (u32 i = 0; i < response_decode.out_object_id_count; ++i) {
        const u32 child_object_id = response_decode.out_object_ids[i];
        if (child_object_id == 0) {
            continue;
        }

        char child_relative_path[MaxDomainPathLength] = {};
        std::snprintf(
            child_relative_path,
            sizeof(child_relative_path),
            "%s.cmd%u[%u]",
            parent_relative_path,
            command_id,
            i);
        RememberDomainPath(session_id, child_object_id, child_relative_path);
    }
}

void AppendJsonLine(TraceFamily family, const char *json) {
    std::scoped_lock lk(g_trace_lock);
    AppendLine(family, json);
}

void AppendJsonLineForService(const char *service_name, const char *json) {
    AppendJsonLine(GetTraceFamilyForServiceName(service_name), json);
}

void EmitBufferRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sm::MitmProcessInfo &client_info,
    const ParsedRequestInfo &request_info,
    u64 session_id,
    u64 request_id,
    const char *capture_phase,
    BinaryCapturePhase binary_phase,
    const char *direction_name,
    BinaryBufferDirection direction,
    u32 index,
    uintptr_t address,
    size_t declared_size,
    bool is_pointer_static,
    bool capture_payload,
    u64 timestamp_ns) {
        const auto *sink = GetTraceSinkConfigForServiceName(service_name);
        char line[1024];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_buffer\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"session_id\":%llu,\"object_id\":%u,\"request_id\":%llu,\"phase\":\"%s\",\"buffer_index\":%u,\"buffer_direction\":\"%s\",\"descriptor_kind\":\"%s\",\"declared_size\":%zu,\"buffer_address\":\"0x%016llX\",\"captured_binary\":%s,\"binary_path\":\"%s\"}\n",
            run_id,
            scenario,
            static_cast<unsigned long long>(timestamp_ns),
            service_name,
            client_program_id,
            static_cast<unsigned long long>(session_id),
            request_info.object_id,
            static_cast<unsigned long long>(request_id),
            capture_phase,
            index,
            direction_name,
            is_pointer_static ? "pointer_static" : "map_alias",
            declared_size,
            static_cast<unsigned long long>(address),
            (capture_payload && address != 0 && declared_size != 0) ? "true" : "false",
            sink != nullptr ? sink->binary_path : "");
        if (written > 0) {
            AppendJsonLineForService(service_name, line);
        }

        if (!capture_payload || address == 0 || declared_size == 0) {
            return;
        }

        EmitBinaryPayloadRecords(
            service_name,
            timestamp_ns,
            client_info,
            session_id,
            request_info.object_id,
            request_id,
            request_info.command_id,
            binary_phase,
            direction,
            static_cast<u8>(index),
            is_pointer_static,
            reinterpret_cast<const u8 *>(address),
            declared_size);
}

void EmitMapAliasBufferRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sm::MitmProcessInfo &client_info,
    const ParsedRequestInfo &request_info,
    u64 session_id,
    u64 request_id,
    const char *capture_phase,
    BinaryCapturePhase binary_phase,
    const char *direction_name,
    BinaryBufferDirection direction,
    u32 index,
    const ::HipcBufferDescriptor &desc,
    bool capture_payload,
    u64 timestamp_ns) {
    EmitBufferRecord(
        run_id,
        scenario,
        service_name,
        client_program_id,
        client_info,
        request_info,
        session_id,
        request_id,
        capture_phase,
        binary_phase,
        direction_name,
        direction,
        index,
        reinterpret_cast<uintptr_t>(hipcGetBufferAddress(std::addressof(desc))),
        hipcGetBufferSize(std::addressof(desc)),
        false,
        capture_payload,
        timestamp_ns);
}

void EmitPointerStaticRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sm::MitmProcessInfo &client_info,
    const ParsedRequestInfo &request_info,
    u64 session_id,
    u64 request_id,
    const char *capture_phase,
    BinaryCapturePhase binary_phase,
    const char *direction_name,
    BinaryBufferDirection direction,
    u32 index,
    const ::HipcStaticDescriptor &desc,
    bool capture_payload,
    u64 timestamp_ns) {
    EmitBufferRecord(
        run_id,
        scenario,
        service_name,
        client_program_id,
        client_info,
        request_info,
        session_id,
        request_id,
        capture_phase,
        binary_phase,
        direction_name,
        direction,
        index,
        reinterpret_cast<uintptr_t>(hipcGetStaticAddress(std::addressof(desc))),
        hipcGetStaticSize(std::addressof(desc)),
        true,
        capture_payload,
        timestamp_ns);
}

void LogRequestBufferRecords(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sm::MitmProcessInfo &client_info,
    const ::HipcParsedRequest &request,
    const ParsedRequestInfo &request_info,
    u64 session_id,
    u64 request_id,
    u64 timestamp_ns) {
    auto emit_buffer = [&](const char *direction_name, BinaryBufferDirection direction, u32 index, const ::HipcBufferDescriptor &desc, bool capture_payload) {
        EmitMapAliasBufferRecord(
            run_id,
            scenario,
            service_name,
            client_program_id,
            client_info,
            request_info,
            session_id,
            request_id,
            "request",
            BinaryCapturePhase::Request,
            direction_name,
            direction,
            index,
            desc,
            capture_payload,
            timestamp_ns);
    };

    auto emit_static = [&](const char *direction_name, BinaryBufferDirection direction, u32 index, const ::HipcStaticDescriptor &desc, bool capture_payload) {
        EmitPointerStaticRecord(
            run_id,
            scenario,
            service_name,
            client_program_id,
            client_info,
            request_info,
            session_id,
            request_id,
            "request",
            BinaryCapturePhase::Request,
            direction_name,
            direction,
            index,
            desc,
            capture_payload,
            timestamp_ns);
    };

    for (u32 i = 0; i < request.meta.num_send_statics; ++i) {
        emit_static("send", BinaryBufferDirection::Send, i, request.data.send_statics[i], true);
    }

    for (u32 i = 0; i < request.meta.num_send_buffers; ++i) {
        emit_buffer("send", BinaryBufferDirection::Send, i, request.data.send_buffers[i], true);
    }
    for (u32 i = 0; i < request.meta.num_recv_buffers; ++i) {
        emit_buffer("recv", BinaryBufferDirection::Recv, i, request.data.recv_buffers[i], false);
    }
    for (u32 i = 0; i < request.meta.num_exch_buffers; ++i) {
        emit_buffer("exchange", BinaryBufferDirection::Exchange, i, request.data.exch_buffers[i], true);
    }
}

void LogResponseBufferRecords(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sm::MitmProcessInfo &client_info,
    const ::HipcParsedRequest &request,
    const ::HipcResponse &response,
    const ParsedRequestInfo &request_info,
    u64 session_id,
    u64 request_id,
    u64 timestamp_ns) {
    auto emit_buffer = [&](const char *direction_name, BinaryBufferDirection direction, u32 index, const ::HipcBufferDescriptor &desc) {
        EmitMapAliasBufferRecord(
            run_id,
            scenario,
            service_name,
            client_program_id,
            client_info,
            request_info,
            session_id,
            request_id,
            "response",
            BinaryCapturePhase::Response,
            direction_name,
            direction,
            index,
            desc,
            true,
            timestamp_ns);
    };

    auto emit_static = [&](const char *direction_name, BinaryBufferDirection direction, u32 index, const ::HipcStaticDescriptor &desc) {
        EmitPointerStaticRecord(
            run_id,
            scenario,
            service_name,
            client_program_id,
            client_info,
            request_info,
            session_id,
            request_id,
            "response",
            BinaryCapturePhase::Response,
            direction_name,
            direction,
            index,
            desc,
            true,
            timestamp_ns);
    };

    for (u32 i = 0; i < request.meta.num_recv_buffers; ++i) {
        emit_buffer("recv", BinaryBufferDirection::Recv, i, request.data.recv_buffers[i]);
    }
    for (u32 i = 0; i < request.meta.num_exch_buffers; ++i) {
        emit_buffer("exchange", BinaryBufferDirection::Exchange, i, request.data.exch_buffers[i]);
    }
    for (u32 i = 0; i < response.num_statics; ++i) {
        emit_static("recv", BinaryBufferDirection::Recv, i, response.statics[i]);
    }
}

void LogHandleRecords(const char *run_id, const char *scenario, const char *service_name, const char *client_program_id, u64 session_id, u64 request_id, const ::HipcResponse &response, u32 object_id, const char *object_path) {
    for (u32 i = 0; i < response.num_copy_handles; ++i) {
        char line[768];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_handle\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"phase\":\"out\",\"handle_index\":%u,\"handle_type\":\"copy\",\"handle_value\":%u}\n",
            run_id,
            scenario,
            static_cast<unsigned long long>(GetMonotonicNs()),
            service_name,
            client_program_id,
            static_cast<unsigned long long>(session_id),
            object_id,
            object_path,
            static_cast<unsigned long long>(request_id),
            i,
            response.copy_handles[i]);
        if (written > 0) {
            AppendJsonLineForService(service_name, line);
        }
    }

    for (u32 i = 0; i < response.num_move_handles; ++i) {
        char line[768];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_handle\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"phase\":\"out\",\"handle_index\":%u,\"handle_type\":\"move\",\"handle_value\":%u}\n",
            run_id,
            scenario,
            static_cast<unsigned long long>(GetMonotonicNs()),
            service_name,
            client_program_id,
            static_cast<unsigned long long>(session_id),
            object_id,
            object_path,
            static_cast<unsigned long long>(request_id),
            i,
            response.move_handles[i]);
        if (written > 0) {
            AppendJsonLineForService(service_name, line);
        }
    }
}

void LogDecodeRecords(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sf::hipc::mitm_monitor::ForwardRequestTraceContext &ctx,
    u64 request_id,
    const ParsedRequestInfo &request_info,
    const ParsedResponseInfo &response_info,
    const RequestDecodeDetails &request_decode,
    const ResponseDecodeDetails &response_decode,
    const char *object_path) {
    const char *selected_kind = GetSelectedKindText(request_info);
    char known_object_kind[48] = {};
    char known_command_name[64] = {};
    DescribeKnownObjectAndCommand(
        service_name,
        object_path,
        request_info.command_id,
        known_object_kind,
        sizeof(known_object_kind),
        known_command_name,
        sizeof(known_command_name));
    if (request_decode.have_raw) {
        char line[2048];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_decode_request\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"selected_kind\":\"%s\",\"object_kind\":\"%s\",\"command_name\":\"%s\",\"selected_command_id\":%u,\"hipc_command_type\":%u,\"hipc_command_type_name\":\"%s\",\"raw_size\":%zu,\"raw_word0\":\"0x%08X\",\"raw_word1\":\"0x%08X\",\"raw_word2\":\"0x%08X\",\"raw_word3\":\"0x%08X\",\"root_magic\":\"0x%08X\",\"root_version\":\"0x%08X\",\"root_command_id\":%u,\"root_token\":\"0x%08X\",\"root_valid\":%s,\"root_close\":%s,\"domain_type\":%u,\"domain_num_in_objects\":%u,\"domain_data_size\":%u,\"domain_object_id\":\"0x%08X\",\"domain_token\":\"0x%08X\",\"domain_cmif_magic\":\"0x%08X\",\"domain_cmif_version\":\"0x%08X\",\"domain_cmif_command_id\":%u,\"domain_cmif_token\":\"0x%08X\",\"domain_valid\":%s,\"domain_close\":%s,\"payload_size\":%zu,\"payload_word0\":\"0x%08X\",\"payload_word1\":\"0x%08X\",\"payload_word2\":\"0x%08X\",\"payload_word3\":\"0x%08X\",\"payload_preview_hex\":\"%s\",\"raw_preview_hex\":\"%s\"}\n",
            run_id,
            scenario,
            static_cast<unsigned long long>(ams::os::ConvertToTimeSpan(ctx.start_tick).GetNanoSeconds()),
            service_name,
            static_cast<unsigned long long>(ctx.client_info.process_id.value),
            client_program_id,
            static_cast<unsigned long long>(ctx.session_id),
            request_info.object_id,
            object_path,
            static_cast<unsigned long long>(request_id),
            selected_kind,
            known_object_kind,
            known_command_name,
            request_info.command_id,
            request_decode.command_type,
            GetHipcCommandTypeName(request_decode.command_type),
            request_decode.raw_size,
            request_decode.raw_words[0],
            request_decode.raw_words[1],
            request_decode.raw_words[2],
            request_decode.raw_words[3],
            request_decode.root_header.magic,
            request_decode.root_header.version,
            request_decode.root_header.command_id,
            request_decode.root_header.token,
            request_decode.root_cmif_valid ? "true" : "false",
            request_decode.root_close ? "true" : "false",
            request_decode.domain_header.type,
            request_decode.domain_header.num_in_objects,
            request_decode.domain_header.data_size,
            request_decode.domain_header.object_id,
            request_decode.domain_header.token,
            request_decode.domain_cmif_header.magic,
            request_decode.domain_cmif_header.version,
            request_decode.domain_cmif_header.command_id,
            request_decode.domain_cmif_header.token,
            request_decode.domain_cmif_valid ? "true" : "false",
            request_decode.domain_close ? "true" : "false",
            request_decode.payload_size,
            request_decode.payload_words[0],
            request_decode.payload_words[1],
            request_decode.payload_words[2],
            request_decode.payload_words[3],
            request_decode.payload_preview_hex,
            request_decode.raw_preview_hex);
        if (written > 0) {
            AppendJsonLineForService(service_name, line);
        }
    }

    if (response_decode.have_raw) {
        char returned_object_ids[160] = "[]";
        if (response_decode.out_object_ids_valid) {
            size_t cursor = 0;
            returned_object_ids[cursor++] = '[';
            for (u32 i = 0; i < response_decode.out_object_id_count && cursor + 16 < sizeof(returned_object_ids); ++i) {
                const int written = std::snprintf(
                    returned_object_ids + cursor,
                    sizeof(returned_object_ids) - cursor,
                    "%s%u",
                    i == 0 ? "" : ",",
                    response_decode.out_object_ids[i]);
                if (written <= 0) {
                    break;
                }
                cursor += static_cast<size_t>(written);
                if (cursor >= sizeof(returned_object_ids) - 2) {
                    cursor = sizeof(returned_object_ids) - 2;
                    break;
                }
            }
            returned_object_ids[cursor++] = ']';
            returned_object_ids[cursor] = '\0';
        }

        char line[1792];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_decode_response\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"selected_kind\":\"%s\",\"object_kind\":\"%s\",\"command_name\":\"%s\",\"selected_result\":\"0x%08X\",\"raw_size\":%zu,\"raw_word0\":\"0x%08X\",\"raw_word1\":\"0x%08X\",\"raw_word2\":\"0x%08X\",\"raw_word3\":\"0x%08X\",\"root_magic\":\"0x%08X\",\"root_version\":\"0x%08X\",\"root_result\":\"0x%08X\",\"root_token\":\"0x%08X\",\"root_valid\":%s,\"domain_num_out_objects\":%u,\"returned_object_ids\":%s,\"domain_cmif_magic\":\"0x%08X\",\"domain_cmif_version\":\"0x%08X\",\"domain_cmif_result\":\"0x%08X\",\"domain_cmif_token\":\"0x%08X\",\"domain_valid\":%s,\"payload_size\":%zu,\"payload_word0\":\"0x%08X\",\"payload_word1\":\"0x%08X\",\"payload_word2\":\"0x%08X\",\"payload_word3\":\"0x%08X\",\"payload_preview_hex\":\"%s\",\"raw_preview_hex\":\"%s\"}\n",
            run_id,
            scenario,
            static_cast<unsigned long long>(ams::os::ConvertToTimeSpan(ctx.end_tick).GetNanoSeconds()),
            service_name,
            static_cast<unsigned long long>(ctx.client_info.process_id.value),
            client_program_id,
            static_cast<unsigned long long>(ctx.session_id),
            request_info.object_id,
            object_path,
            static_cast<unsigned long long>(request_id),
            selected_kind,
            known_object_kind,
            known_command_name,
            response_info.valid ? response_info.result_value : ctx.forward_result.GetValue(),
            response_decode.raw_size,
            response_decode.raw_words[0],
            response_decode.raw_words[1],
            response_decode.raw_words[2],
            response_decode.raw_words[3],
            response_decode.root_header.magic,
            response_decode.root_header.version,
            response_decode.root_header.result,
            response_decode.root_header.token,
            response_decode.root_cmif_valid ? "true" : "false",
            response_decode.domain_header.num_out_objects,
            returned_object_ids,
            response_decode.domain_cmif_header.magic,
            response_decode.domain_cmif_header.version,
            response_decode.domain_cmif_header.result,
            response_decode.domain_cmif_header.token,
            response_decode.domain_cmif_valid ? "true" : "false",
            response_decode.payload_size,
            response_decode.payload_words[0],
            response_decode.payload_words[1],
            response_decode.payload_words[2],
            response_decode.payload_words[3],
            response_decode.payload_preview_hex,
            response_decode.raw_preview_hex);
        if (written > 0) {
            AppendJsonLineForService(service_name, line);
        }
    }
}

void EmitNifmSemanticRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sf::hipc::mitm_monitor::ForwardRequestTraceContext &ctx,
    u64 request_id,
    const ParsedRequestInfo &request_info,
    const ParsedResponseInfo &response_info,
    const RequestDecodeDetails &request_decode,
    const ResponseDecodeDetails &response_decode,
    const char *object_path) {
    if (!StartsWith(service_name, "nifm:") || request_info.is_close || !IsKnownNifmObjectPath(object_path)) {
        return;
    }

    char object_kind[48] = {};
    char command_name[64] = {};
    char request_summary[192] = {};
    char response_summary[256] = {};
    GetNifmObjectKindAndCommandName(
        object_path,
        request_info.command_id,
        object_kind,
        sizeof(object_kind),
        command_name,
        sizeof(command_name));
    FormatNifmSemanticRequestSummary(
        request_summary,
        sizeof(request_summary),
        object_path,
        request_info.command_id,
        request_decode);
    FormatNifmSemanticResponseSummary(
        response_summary,
        sizeof(response_summary),
        object_path,
        request_info.command_id,
        response_info,
        response_decode);

    char line[1024];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"nifm_semantic\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"object_kind\":\"%s\",\"command_id\":%u,\"command_name\":\"%s\",\"result\":\"0x%08X\",\"request_summary\":\"%s\",\"response_summary\":\"%s\"}\n",
        run_id,
        scenario,
        static_cast<unsigned long long>(ams::os::ConvertToTimeSpan(ctx.end_tick).GetNanoSeconds()),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        request_info.object_id,
        object_path,
        static_cast<unsigned long long>(request_id),
        object_kind,
        request_info.command_id,
        command_name,
        response_info.valid ? response_info.result_value : ctx.forward_result.GetValue(),
        request_summary,
        response_summary);
    if (written > 0) {
        AppendJsonLineForService(service_name, line);
    }
}

void EmitBsdSemanticRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sf::hipc::mitm_monitor::ForwardRequestTraceContext &ctx,
    u64 request_id,
    const ParsedRequestInfo &request_info,
    const RequestDecodeDetails &request_decode,
    const ResponseDecodeDetails &response_decode,
    const char *object_path,
    const ::HipcParsedRequest *request,
    const ::HipcResponse *response,
    bool have_response,
    bool have_request) {
    if (!StartsWith(service_name, "bsd:") || request_info.is_close || !IsKnownBsdObjectPath(object_path)) {
        return;
    }

    char object_kind[48] = {};
    char command_name[64] = {};
    char request_summary[384] = {};
    char response_summary[256] = {};
    GetBsdObjectKindAndCommandName(
        object_path,
        request_info.command_id,
        object_kind,
        sizeof(object_kind),
        command_name,
        sizeof(command_name));
    FormatBsdSemanticRequestSummary(
        request_summary,
        sizeof(request_summary),
        object_path,
        request_info.command_id,
        request_decode);
    FormatBsdSemanticResponseSummary(
        response_summary,
        sizeof(response_summary),
        object_path,
        request_info.command_id,
        response_decode);

    if (have_request && request != nullptr) {
        static_cast<void>(TryAppendBsdSockaddrRequestSummary(
            request_summary,
            sizeof(request_summary),
            request_info.command_id,
            *request));
        static_cast<void>(TryAppendBsdSockOptRequestSummary(
            request_summary,
            sizeof(request_summary),
            request_info.command_id,
            *request));
        static_cast<void>(TryAppendBsdSockaddrResponseSummary(
            response_summary,
            sizeof(response_summary),
            request_info.command_id,
            *request,
            response != nullptr ? *response : ::HipcResponse{},
            have_response,
            response_decode));
        static_cast<void>(TryAppendBsdSockOptResponseSummary(
            response_summary,
            sizeof(response_summary),
            request_info.command_id,
            *request,
            response != nullptr ? *response : ::HipcResponse{},
            have_response));
    }

    char line[1280];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"bsd_semantic\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"object_kind\":\"%s\",\"command_id\":%u,\"command_name\":\"%s\",\"result\":\"0x%08X\",\"request_summary\":\"%s\",\"response_summary\":\"%s\"}\n",
        run_id,
        scenario,
        static_cast<unsigned long long>(ams::os::ConvertToTimeSpan(ctx.end_tick).GetNanoSeconds()),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        request_info.object_id,
        object_path,
        static_cast<unsigned long long>(request_id),
        object_kind,
        request_info.command_id,
        command_name,
        ctx.forward_result.GetValue(),
        request_summary,
        response_summary);
    if (written > 0) {
        AppendJsonLineForService(service_name, line);
    }
}

void EmitSslSemanticRecord(
    const char *run_id,
    const char *scenario,
    const char *service_name,
    const char *client_program_id,
    const ams::sf::hipc::mitm_monitor::ForwardRequestTraceContext &ctx,
    u64 request_id,
    const ParsedRequestInfo &request_info,
    const ParsedResponseInfo &response_info,
    const RequestDecodeDetails &request_decode,
    const ResponseDecodeDetails &response_decode,
    const char *object_path,
    const ::HipcParsedRequest *request,
    const ::HipcResponse *response,
    bool have_response) {
    if (!StartsWith(service_name, "ssl") || request_info.is_close || !IsKnownSslObjectPath(object_path)) {
        return;
    }

    char object_kind[48] = {};
    char command_name[64] = {};
    char request_summary[256] = {};
    char response_summary[256] = {};
    GetSslObjectKindAndCommandName(
        service_name,
        object_path,
        request_info.command_id,
        object_kind,
        sizeof(object_kind),
        command_name,
        sizeof(command_name));
    FormatSslSemanticRequestSummary(
        request_summary,
        sizeof(request_summary),
        object_path,
        request_info.command_id,
        request_decode,
        request);
    FormatSslSemanticResponseSummary(
        response_summary,
        sizeof(response_summary),
        object_path,
        request_info.command_id,
        response_info,
        response_decode,
        request,
        response,
        have_response);

    char line[1280];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ssl_semantic\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"object_kind\":\"%s\",\"command_id\":%u,\"command_name\":\"%s\",\"result\":\"0x%08X\",\"request_summary\":\"%s\",\"response_summary\":\"%s\"}\n",
        run_id,
        scenario,
        static_cast<unsigned long long>(ams::os::ConvertToTimeSpan(ctx.end_tick).GetNanoSeconds()),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        request_info.object_id,
        object_path,
        static_cast<unsigned long long>(request_id),
        object_kind,
        request_info.command_id,
        command_name,
        response_info.valid ? response_info.result_value : ctx.forward_result.GetValue(),
        request_summary,
        response_summary);
    if (written > 0) {
        AppendJsonLineForService(service_name, line);
    }
}

void OnForwardRequestTrace(const ams::sf::hipc::mitm_monitor::ForwardRequestTraceContext &ctx) {
    char service_name[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    char run_id[64] = {};
    EncodeServiceName(service_name, sizeof(service_name), ctx.service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), ctx.client_info.program_id);
    std::snprintf(run_id, sizeof(run_id), "tick-%llu", static_cast<unsigned long long>(g_run_tick));

    const char *scenario = "unknown";
    const u64 request_id = g_next_request_id.fetch_add(1);
    const RequestDecodeDetails request_decode = GetRequestDecodeDetails(ctx.request_message);
    const ResponseDecodeDetails response_decode = ctx.has_response ? GetResponseDecodeDetails(ctx.response_message) : ResponseDecodeDetails{};
    const ParsedRequestInfo request_info = ParseRequestInfo(ctx.request_message);
    const ParsedResponseInfo response_info = ctx.has_response ? ParseResponseInfo(ctx.response_message) : ParsedResponseInfo{};
    const u64 request_ts_ns = static_cast<u64>(ams::os::ConvertToTimeSpan(ctx.start_tick).GetNanoSeconds());
    const u64 response_ts_ns = static_cast<u64>(ams::os::ConvertToTimeSpan(ctx.end_tick).GetNanoSeconds());
    char object_path[160] = {};
    FormatObjectPath(object_path, sizeof(object_path), service_name, ctx.session_id, request_info.object_id);
    const char *selected_kind = GetSelectedKindText(request_info);
    char known_object_kind[48] = {};
    char known_command_name[64] = {};
    DescribeKnownObjectAndCommand(
        service_name,
        object_path,
        request_info.command_id,
        known_object_kind,
        sizeof(known_object_kind),
        known_command_name,
        sizeof(known_command_name));

    if (ShouldSuppressBsdTrace(service_name, request_info.command_id)) {
        return;
    }

    ::HipcParsedRequest request = {};
    ams::sf::cmif::PointerAndSize ignored_raw_data;
    const bool have_request = GetRequestRawData(ctx.request_message, std::addressof(request), std::addressof(ignored_raw_data));
    ::HipcResponse response = {};
    const bool have_response = ctx.has_response;
    if (have_response) {
        response = hipcParseResponse(ctx.response_message.GetPointer());
    }

    char request_line[1024];
    const int request_written = std::snprintf(
        request_line,
        sizeof(request_line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_request\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"request_id\":%llu,\"command_id\":%u,\"command_name\":\"%s\",\"object_kind\":\"%s\",\"known_object_kind\":\"%s\",\"object_path\":\"%s\",\"in_raw_size\":%zu,\"buffer_count\":%u,\"in_interface_count\":0,\"out_interface_expected\":0,\"copy_handle_count\":%u,\"move_handle_count\":%u,\"send_pid\":%s}\n",
        run_id,
        scenario,
        static_cast<unsigned long long>(request_ts_ns),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        request_info.object_id,
        static_cast<unsigned long long>(request_id),
        request_info.command_id,
        known_command_name,
        selected_kind,
        known_object_kind,
        object_path,
        request_info.raw_size,
        have_request ? (request.meta.num_send_statics + request.meta.num_send_buffers + request.meta.num_recv_buffers + request.meta.num_exch_buffers) : 0,
        have_request ? request.meta.num_copy_handles : 0,
        have_request ? request.meta.num_move_handles : 0,
        have_request && request.meta.send_pid ? "true" : "false");
    if (request_written > 0) {
        AppendJsonLineForService(service_name, request_line);
    }

    if (have_request) {
        LogRequestBufferRecords(
            run_id,
            scenario,
            service_name,
            client_program_id,
            ctx.client_info,
            request,
            request_info,
            ctx.session_id,
            request_id,
            request_ts_ns);
    }

    LogDecodeRecords(run_id, scenario, service_name, client_program_id, ctx, request_id, request_info, response_info, request_decode, response_decode, object_path);

    char response_line[1024];
    const int response_written = std::snprintf(
        response_line,
        sizeof(response_line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"%s\",\"event\":\"ipc_response\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"object_path\":\"%s\",\"request_id\":%llu,\"command_id\":%u,\"command_name\":\"%s\",\"object_kind\":\"%s\",\"result\":\"0x%08X\",\"duration_ns\":%llu,\"out_raw_size\":%zu,\"out_interface_count\":%u,\"copy_handle_count\":%u,\"move_handle_count\":%u,\"server_closed_session\":false}\n",
        run_id,
        scenario,
        static_cast<unsigned long long>(response_ts_ns),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        request_info.object_id,
        object_path,
        static_cast<unsigned long long>(request_id),
        request_info.command_id,
        known_command_name,
        known_object_kind,
        response_info.valid ? response_info.result_value : ctx.forward_result.GetValue(),
        static_cast<unsigned long long>(GetDurationNs(ctx.start_tick, ctx.end_tick)),
        response_info.raw_size,
        response_info.out_object_count,
        ctx.has_response ? hipcParseResponse(ctx.response_message.GetPointer()).num_copy_handles : 0,
        ctx.has_response ? hipcParseResponse(ctx.response_message.GetPointer()).num_move_handles : 0);
    if (response_written > 0) {
        AppendJsonLineForService(service_name, response_line);
    }

    EmitNifmSemanticRecord(
        run_id,
        scenario,
        service_name,
        client_program_id,
        ctx,
        request_id,
        request_info,
        response_info,
        request_decode,
        response_decode,
        object_path);
    EmitBsdSemanticRecord(
        run_id,
        scenario,
        service_name,
        client_program_id,
        ctx,
        request_id,
        request_info,
        request_decode,
        response_decode,
        object_path,
        have_request ? std::addressof(request) : nullptr,
        have_response ? std::addressof(response) : nullptr,
        have_response,
        have_request);
    EmitSslSemanticRecord(
        run_id,
        scenario,
        service_name,
        client_program_id,
        ctx,
        request_id,
        request_info,
        response_info,
        request_decode,
        response_decode,
        object_path,
        have_request ? std::addressof(request) : nullptr,
        have_response ? std::addressof(response) : nullptr,
        have_response);

    RememberReturnedDomainObjects(ctx.session_id, request_info.object_id, request_info.command_id, response_decode);

    if (ctx.has_response) {
        if (have_request) {
            LogResponseBufferRecords(
                run_id,
                scenario,
                service_name,
                client_program_id,
                ctx.client_info,
                request,
                response,
                request_info,
                ctx.session_id,
                request_id,
                response_ts_ns);
        }
        const auto response = hipcParseResponse(ctx.response_message.GetPointer());
        LogHandleRecords(run_id, scenario, service_name, client_program_id, ctx.session_id, request_id, response, request_info.object_id, object_path);
    }
}

void OnDomainTrace(const ams::sf::hipc::mitm_monitor::DomainTraceContext &ctx) {
    char service_name[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    char run_id[64] = {};
    char event_type[48] = {};
    char object_ids[160] = "[]";
    EncodeServiceName(service_name, sizeof(service_name), ctx.service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), ctx.client_info.program_id);
    std::snprintf(run_id, sizeof(run_id), "tick-%llu", static_cast<unsigned long long>(g_run_tick));

    switch (ctx.event_type) {
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::ConvertCurrentObjectToDomain:
            std::snprintf(event_type, sizeof(event_type), "convert_current_object_to_domain");
            break;
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::DispatchSendMessage:
            std::snprintf(event_type, sizeof(event_type), "dispatch_send_message");
            break;
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::DispatchClose:
            std::snprintf(event_type, sizeof(event_type), "dispatch_close");
            break;
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::ForwardRequestForMissingObject:
            std::snprintf(event_type, sizeof(event_type), "forward_request_for_missing_object");
            break;
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::SetOutObjects:
            std::snprintf(event_type, sizeof(event_type), "set_out_objects");
            break;
        case ams::sf::hipc::mitm_monitor::DomainTraceEventType::MirrorForwardedOutObjects:
            std::snprintf(event_type, sizeof(event_type), "mirror_forwarded_out_objects");
            break;
        default:
            std::snprintf(event_type, sizeof(event_type), "unknown");
            break;
    }

    const char *detail_name = "none";
    if (ctx.event_type == ams::sf::hipc::mitm_monitor::DomainTraceEventType::ConvertCurrentObjectToDomain) {
        switch (ctx.detail_code) {
            case 0:
                detail_name = "none";
                break;
            case 1:
                detail_name = "forward_pre_convert";
                break;
            case 2:
                detail_name = "forward_convert_failed";
                break;
            case 3:
                detail_name = "forward_convert_succeeded";
                break;
            default:
                detail_name = "unknown";
                break;
        }
    } else if (ctx.event_type == ams::sf::hipc::mitm_monitor::DomainTraceEventType::MirrorForwardedOutObjects) {
        switch (ctx.detail_code) {
            case 1:
                detail_name = "raw_size_too_small";
                break;
            case 2:
                detail_name = "raw_buffer_out_of_bounds";
                break;
            case 3:
                detail_name = "raw_data_too_small";
                break;
            case 4:
                detail_name = "cmif_magic_invalid";
                break;
            case 5:
                detail_name = "no_out_objects";
                break;
            case 6:
                detail_name = "object_id_trailer_too_small";
                break;
            case 7:
                detail_name = "skip_invalid_object_id";
                break;
            case 8:
                detail_name = "skip_existing_object";
                break;
            case 9:
                detail_name = "copy_from_current_domain_failed";
                break;
            case 10:
                detail_name = "registered_object";
                break;
            case 11:
                detail_name = "complete";
                break;
            case 12:
                detail_name = "too_many_out_objects";
                break;
            case 13:
                detail_name = "passive_proxy_skipped";
                break;
            default:
                detail_name = "unknown";
                break;
        }
    }

    if (ctx.num_out_objects > 0) {
        size_t cursor = 0;
        object_ids[cursor++] = '[';
        for (u32 i = 0; i < ctx.num_out_objects && i < 8 && cursor + 16 < sizeof(object_ids); ++i) {
            const int written = std::snprintf(
                object_ids + cursor,
                sizeof(object_ids) - cursor,
                "%s%u",
                i == 0 ? "" : ",",
                ctx.object_ids[i]);
            if (written <= 0) {
                break;
            }
            cursor += static_cast<size_t>(written);
            if (cursor >= sizeof(object_ids) - 2) {
                cursor = sizeof(object_ids) - 2;
                break;
            }
        }
        object_ids[cursor++] = ']';
        object_ids[cursor] = '\0';
    }

    char forward_state[192] = {};
    if (ctx.has_forward_state) {
        std::snprintf(
            forward_state,
            sizeof(forward_state),
            ",\"forward_handle\":%d,\"forward_own_handle\":%s,\"forward_object_id\":%u,\"forward_pointer_buffer_size\":%u",
            ctx.forward_handle,
            ctx.forward_own_handle ? "true" : "false",
            ctx.forward_object_id,
            ctx.forward_pointer_buffer_size);
    }

    char line[1400];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"%s\",\"scenario\":\"unknown\",\"event\":\"domain_trace\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"domain_event\":\"%s\",\"request_object_id\":%u,\"data_size\":%u,\"num_in_objects\":%u,\"num_out_objects\":%u,\"object_ids\":%s,\"object_found\":%s,\"detail_code\":%u,\"detail_name\":\"%s\",\"detail_value\":%u,\"detail_result\":\"0x%08X\",\"has_detail_result\":%s%s}\n",
        run_id,
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        event_type,
        ctx.request_object_id,
        ctx.data_size,
        ctx.num_in_objects,
        ctx.num_out_objects,
        object_ids,
        ctx.object_found ? "true" : "false",
        ctx.detail_code,
        detail_name,
        ctx.detail_value,
        ctx.detail_result.GetValue(),
        ctx.has_detail_result ? "true" : "false",
        forward_state);
    if (written > 0) {
        AppendJsonLineForService(service_name, line);
    }

    if (ctx.event_type == ams::sf::hipc::mitm_monitor::DomainTraceEventType::ConvertCurrentObjectToDomain &&
        ctx.num_out_objects > 0 &&
        ctx.object_ids[0] != 0) {
        RememberDomainPath(ctx.session_id, ctx.object_ids[0], "root");
    }
}

} // namespace

void Initialize() {
    if (g_initialized) {
        return;
    }

    g_initialized = true;
    g_run_tick = static_cast<u64>(svcGetSystemTick());
    ResetDomainPathState();
    ResetSessionState();
    for (const auto &sink : g_trace_sinks) {
        WriteMetaFile(sink);
    }

    char line[512];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"trace_start\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"mitm\",\"client_program_id\":\"0x010000000000EAD1\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":0,\"object_id\":0,\"request_id\":0}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()));
    if (written > 0) {
        AppendJsonLine(TraceFamily::Broadcast, line);
    }
}

u64 AllocateSessionId() {
    return g_next_session_id.fetch_add(1);
}

void LogSmLifecycle(const ams::sf::hipc::mitm_monitor::SmLifecycleTraceContext &ctx) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), ctx.service_name);

    char extra[320];
    extra[0] = '\0';
    size_t cursor = 0;

    if (ctx.has_result) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"rc\":\"0x%08X\"",
            ctx.result.GetValue());
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_bool_out && cursor < sizeof(extra)) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"has_mitm\":%s",
            ctx.bool_out ? "true" : "false");
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.port_handle != ams::os::InvalidNativeHandle && cursor < sizeof(extra)) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"port_handle\":%d",
            ctx.port_handle);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.query_handle != ams::os::InvalidNativeHandle && cursor < sizeof(extra)) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"query_handle\":%d",
            ctx.query_handle);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_client_info && cursor < sizeof(extra)) {
        char client_program_id[32] = {};
        FormatProgramId(client_program_id, sizeof(client_program_id), ctx.client_info.program_id);
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"client_pid\":%llu,\"client_program_id\":\"%s\"",
            static_cast<unsigned long long>(ctx.client_info.process_id.value),
            client_program_id);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    char line[1024];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"sm_mitm\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"operation\":\"%s\",\"phase\":\"%s\",\"client_program_id\":\"0x010000000000EAD1\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":0,\"object_id\":0,\"request_id\":0%s}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        GetSmLifecycleOperation(ctx.event_type),
        GetSmLifecyclePhase(ctx.event_type),
        extra);
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogDispatchTrace(const ams::sf::hipc::mitm_monitor::DispatchTraceContext &ctx) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), ctx.service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), ctx.client_info.program_id);

    if (ctx.has_command_id && ShouldSuppressBsdTrace(service_name_text, ctx.command_id)) {
        return;
    }

    char command_name[64] = "Unknown";
    char object_kind[48] = "Unknown";
    if (ctx.has_command_id) {
        const bool is_manager_request =
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestBegin ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestEnd ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestDomainOverride ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestBaseFallback ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifParseFail ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifHandlerLookup ||
            ctx.event_type == ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifHandlerResult;
        if (is_manager_request) {
            std::snprintf(object_kind, sizeof(object_kind), "IHipcManager");
            GetHipcManagerCommandName(command_name, sizeof(command_name), ctx.command_id);
        } else {
            char object_path[160] = {};
            FormatObjectPath(object_path, sizeof(object_path), service_name_text, ctx.session_id, ctx.object_id);
            DescribeKnownObjectAndCommand(
                service_name_text,
                object_path,
                ctx.command_id,
                object_kind,
                sizeof(object_kind),
                command_name,
                sizeof(command_name));
        }
    }

    char extra[768];
    extra[0] = '\0';
    size_t cursor = 0;

    if (ctx.has_command_id) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"command_id\":%u,\"command_name\":\"%s\",\"object_kind\":\"%s\"",
            ctx.command_id,
            command_name,
            object_kind);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_hipc_command_type && cursor < sizeof(extra)) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"hipc_command_type\":%u,\"hipc_command_type_name\":\"%s\"",
            ctx.hipc_command_type,
            GetHipcCommandTypeName(ctx.hipc_command_type));
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_result && cursor < sizeof(extra)) {
        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"rc\":\"0x%08X\"",
            ctx.result.GetValue());
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_detail && cursor < sizeof(extra)) {
        const char *detail_name = "unknown";
        switch (ctx.event_type) {
            case ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifParseFail:
                switch (ctx.detail_code) {
                    case 1:
                        detail_name = "header_too_small";
                        break;
                    case 2:
                        detail_name = "invalid_in_header";
                        break;
                    default:
                        break;
                }
                break;
            case ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifHandlerLookup:
                detail_name = "handler_lookup";
                break;
            case ams::sf::hipc::mitm_monitor::DispatchTraceEventType::ManagerRequestCmifHandlerResult:
                detail_name = "handler_result";
                break;
            default:
                detail_name = "detail";
                break;
        }

        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"detail_code\":%u,\"detail_name\":\"%s\",\"detail_value0\":%u,\"detail_value1\":%u,\"detail_value2\":%u,\"detail_value3\":%u",
            ctx.detail_code,
            detail_name,
            ctx.detail_value0,
            ctx.detail_value1,
            ctx.detail_value2,
            ctx.detail_value3);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));
    }

    if (ctx.has_response_snapshot && cursor < sizeof(extra)) {
        char raw_preview[(8 * 8) + 8] = {};
        size_t preview_cursor = 0;
        for (u32 i = 0; i < ctx.response_raw_word_count && i < 8; ++i) {
            const int preview_written = std::snprintf(
                raw_preview + preview_cursor,
                sizeof(raw_preview) - preview_cursor,
                "%s%08X",
                i == 0 ? "" : "_",
                ctx.response_raw_words[i]);
            preview_cursor += static_cast<size_t>(ClampLength(preview_written, sizeof(raw_preview) - preview_cursor));
            if (preview_cursor >= sizeof(raw_preview) - 1) {
                break;
            }
        }

        const int written = std::snprintf(
            extra + cursor,
            sizeof(extra) - cursor,
            ",\"response_data_words\":%u,\"response_raw_size\":%u,\"response_raw_word_count\":%u,\"response_raw_preview\":\"%s\",\"response_num_copy_handles\":%u,\"response_num_move_handles\":%u",
            ctx.response_data_words,
            ctx.response_raw_size,
            ctx.response_raw_word_count,
            raw_preview,
            ctx.response_num_copy_handles,
            ctx.response_num_move_handles);
        cursor += static_cast<size_t>(ClampLength(written, sizeof(extra) - cursor));

        if (ctx.response_num_copy_handles > 0 && cursor < sizeof(extra)) {
            const int handle_written = std::snprintf(
                extra + cursor,
                sizeof(extra) - cursor,
                ",\"response_first_copy_handle\":%d",
                ctx.response_copy_handles[0]);
            cursor += static_cast<size_t>(ClampLength(handle_written, sizeof(extra) - cursor));
        }

        if (ctx.response_num_move_handles > 0 && cursor < sizeof(extra)) {
            const int handle_written = std::snprintf(
                extra + cursor,
                sizeof(extra) - cursor,
                ",\"response_first_move_handle\":%d",
                ctx.response_move_handles[0]);
            cursor += static_cast<size_t>(ClampLength(handle_written, sizeof(extra) - cursor));
        }

        if (ctx.has_command_id && ctx.response_raw_word_count >= 5 && cursor < sizeof(extra)) {
            switch (ctx.command_id) {
                case 0:
                {
                    const int semantic_written = std::snprintf(
                        extra + cursor,
                        sizeof(extra) - cursor,
                        ",\"response_domain_object_id\":%u",
                        ctx.response_raw_words[4]);
                    cursor += static_cast<size_t>(ClampLength(semantic_written, sizeof(extra) - cursor));
                    break;
                }
                case 3:
                {
                    const int semantic_written = std::snprintf(
                        extra + cursor,
                        sizeof(extra) - cursor,
                        ",\"response_pointer_buffer_size\":%u",
                        ctx.response_raw_words[4] & 0xFFFFu);
                    cursor += static_cast<size_t>(ClampLength(semantic_written, sizeof(extra) - cursor));
                    break;
                }
                default:
                    break;
            }
        }
    }

    char line[1792];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"mitm_dispatch\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":%u,\"request_id\":0,\"dispatch_event\":\"%s\",\"object_found\":%s%s}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        ctx.object_id,
        GetDispatchEventName(ctx.event_type),
        ctx.object_found ? "true" : "false",
        extra);
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogSessionTrace(const ams::sf::hipc::mitm_monitor::SessionTraceContext &ctx) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), ctx.service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), ctx.client_info.program_id);

    size_t remaining_count = 0;
    {
        std::scoped_lock lk(g_state_lock);
        SessionEntry ignored = {};
        static_cast<void>(ForgetSessionLocked(ctx.session_id, std::addressof(ignored), std::addressof(remaining_count)));
    }

    const char *event_name = "client_session_event";
    switch (ctx.event_type) {
        case ams::sf::hipc::mitm_monitor::SessionTraceEventType::Close:
            event_name = "client_disconnected";
            break;
        AMS_UNREACHABLE_DEFAULT_CASE();
    }

    char line[1152];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"%s\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":0,\"request_id\":0,\"session_handle\":%d,\"active_session_count\":%zu}\n",
        static_cast<unsigned long long>(g_run_tick),
        event_name,
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        static_cast<unsigned long long>(ctx.client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(ctx.session_id),
        ctx.session_handle,
        remaining_count);
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogSessionConnected(const ams::sm::ServiceName &service_name, u64 session_id, const ams::sm::MitmProcessInfo &client_info) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), client_info.program_id);

    size_t active_session_count = 0;
    {
        std::scoped_lock lk(g_state_lock);
        active_session_count = RememberSessionLocked(service_name, session_id, client_info);
    }

    char line[1024];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"client_connected\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":0,\"request_id\":0,\"active_session_count\":%zu}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        static_cast<unsigned long long>(client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(session_id),
        active_session_count);
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogSessionAcceptFailure(const ams::sm::ServiceName &service_name, const ams::sm::MitmProcessInfo &client_info, ams::Result result) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), client_info.program_id);

    char line[1024];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"error\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":0,\"object_id\":0,\"request_id\":0,\"notes\":\"accept failed: 0x%08X\"}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        static_cast<unsigned long long>(client_info.process_id.value),
        client_program_id,
        result.GetValue());
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogForwardServiceState(
    const ams::sm::ServiceName &service_name,
    const ams::sm::MitmProcessInfo &client_info,
    u64 session_id,
    const char *phase,
    ams::os::NativeHandle handle,
    bool own_handle,
    u32 object_id,
    u32 pointer_buffer_size) {
    Initialize();

    char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
    char client_program_id[32] = {};
    EncodeServiceName(service_name_text, sizeof(service_name_text), service_name);
    FormatProgramId(client_program_id, sizeof(client_program_id), client_info.program_id);

    char line[1280];
    const int written = std::snprintf(
        line,
        sizeof(line),
        "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"forward_service_state\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":0,\"request_id\":0,\"phase\":\"%s\",\"forward_handle\":%d,\"forward_own_handle\":%s,\"forward_object_id\":%u,\"forward_pointer_buffer_size\":%u}\n",
        static_cast<unsigned long long>(g_run_tick),
        static_cast<unsigned long long>(GetMonotonicNs()),
        service_name_text,
        static_cast<unsigned long long>(client_info.process_id.value),
        client_program_id,
        static_cast<unsigned long long>(session_id),
        phase != nullptr ? phase : "unknown",
        handle,
        own_handle ? "true" : "false",
        object_id,
        pointer_buffer_size);
    if (written > 0) {
        AppendJsonLineForService(service_name_text, line);
    }
}

void LogSessionSnapshot(const char *phase) {
    Initialize();

    SessionEntry entries[MaxTrackedSessions] = {};
    const size_t active_count = CopyTrackedSessions(entries, MaxTrackedSessions);

    if (active_count == 0) {
        char line[768];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"session_snapshot\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"mitm\",\"client_program_id\":\"0x010000000000EAD1\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":0,\"object_id\":0,\"request_id\":0,\"phase\":\"%s\",\"active_session_count\":0,\"snapshot_size\":0,\"snapshot_index\":0}\n",
            static_cast<unsigned long long>(g_run_tick),
            static_cast<unsigned long long>(GetMonotonicNs()),
            phase != nullptr ? phase : "unknown");
        if (written > 0) {
            AppendJsonLine(TraceFamily::Broadcast, line);
        }
        return;
    }

    for (size_t i = 0; i < active_count && i < MaxTrackedSessions; ++i) {
        char service_name_text[ams::sm::ServiceName::MaxLength + 8] = {};
        char client_program_id[32] = {};
        EncodeServiceName(service_name_text, sizeof(service_name_text), entries[i].service_name);
        FormatProgramId(client_program_id, sizeof(client_program_id), entries[i].client_info.program_id);

        char line[1024];
        const int written = std::snprintf(
            line,
            sizeof(line),
            "{\"schema_version\":1,\"run_id\":\"tick-%llu\",\"scenario\":\"unknown\",\"event\":\"session_snapshot\",\"ts_monotonic_ns\":%llu,\"ts_utc\":\"unknown\",\"service\":\"%s\",\"client_pid\":%llu,\"client_program_id\":\"%s\",\"server_program_id\":\"0x010000000000EAD1\",\"thread_id\":0,\"session_id\":%llu,\"object_id\":0,\"request_id\":0,\"phase\":\"%s\",\"active_session_count\":%zu,\"snapshot_size\":%zu,\"snapshot_index\":%zu}\n",
            static_cast<unsigned long long>(g_run_tick),
            static_cast<unsigned long long>(GetMonotonicNs()),
            service_name_text,
            static_cast<unsigned long long>(entries[i].client_info.process_id.value),
            client_program_id,
            static_cast<unsigned long long>(entries[i].session_id),
            phase != nullptr ? phase : "unknown",
            active_count,
            active_count,
            i);
        if (written > 0) {
            AppendJsonLineForService(service_name_text, line);
        }
    }
}

void RegisterMonitorHooks() {
    ams::sf::hipc::mitm_monitor::SetSmLifecycleTraceCallback(LogSmLifecycle);
    ams::sf::hipc::mitm_monitor::SetDispatchTraceCallback(LogDispatchTrace);
    ams::sf::hipc::mitm_monitor::SetSessionTraceCallback(LogSessionTrace);
    ams::sf::hipc::mitm_monitor::SetForwardRequestTraceCallback(OnForwardRequestTrace);
    ams::sf::hipc::mitm_monitor::SetDomainTraceCallback(OnDomainTrace);
}

} // namespace wgnx::net_probe::mitm_trace
