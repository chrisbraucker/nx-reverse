#include "logger.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "fs_runtime.hpp"

namespace wgnx::net_probe::logger {

namespace {

constexpr const char *LogDirectory = "sdmc:/wgnx";
constexpr const char *LogFilePath = "sdmc:/wgnx/net-probe.log";
constexpr size_t QueueEntryCapacity = 896;
constexpr size_t BinaryQueueEntryCapacity = 96;
constexpr size_t MaxQueuedPathLength = 96;
constexpr size_t MaxQueuedLineLength = 2048;
constexpr size_t MaxQueuedBlobLength = 2048;
constexpr auto FlushInterval = ams::TimeSpan::FromMilliSeconds(100);
constexpr size_t FlushLineBatchSize = 16;
constexpr size_t FlushBlobBatchSizePerFamily = 8;

enum class FileBackendState : std::uint8_t {
    Uninitialized,
    Ready,
    Disabled,
};

struct QueuedLine {
    char path[MaxQueuedPathLength]{};
    char line[MaxQueuedLineLength]{};
    u16 path_size{0};
    u16 line_size{0};
};

struct QueuedBlob {
    char path[MaxQueuedPathLength]{};
    u8 data[MaxQueuedBlobLength]{};
    u16 path_size{0};
    u16 data_size{0};
};

enum class BlobQueueKind : size_t {
    Generic = 0,
    Nifm,
    Bsd,
    Ssl,
    Count,
};

bool g_logger_initialized = false;
FileBackendState g_file_backend_state = FileBackendState::Uninitialized;
ams::os::SdkMutex g_logger_lock;
QueuedLine g_queue[QueueEntryCapacity] = {};
size_t g_queue_head = 0;
size_t g_queue_tail = 0;
size_t g_queue_count = 0;
u64 g_dropped_line_count = 0;
QueuedBlob g_blob_queues[static_cast<size_t>(BlobQueueKind::Count)][BinaryQueueEntryCapacity] = {};
size_t g_blob_queue_heads[static_cast<size_t>(BlobQueueKind::Count)] = {};
size_t g_blob_queue_tails[static_cast<size_t>(BlobQueueKind::Count)] = {};
size_t g_blob_queue_counts[static_cast<size_t>(BlobQueueKind::Count)] = {};
u64 g_dropped_blob_counts[static_cast<size_t>(BlobQueueKind::Count)] = {};
QueuedLine g_flush_line_batch[FlushLineBatchSize] = {};
QueuedBlob g_flush_blob_batch[static_cast<size_t>(BlobQueueKind::Count) * FlushBlobBatchSizePerFamily] = {};
ams::os::EventType g_flush_event = {};
bool g_flush_event_initialized = false;
bool g_flush_thread_started = false;
bool g_flush_thread_stop_requested = false;
alignas(::ams::os::ThreadStackAlignment) u8 g_flush_thread_stack[8 * 1024] = {};
ams::os::ThreadType g_flush_thread = {};

void EmitDebugString(const char *line, size_t line_size) {
    if (line == nullptr || line_size == 0) {
        return;
    }

    static_cast<void>(::svcOutputDebugString(line, line_size));
}

void EnsureFileBackendInitialized() {
    if (g_file_backend_state != FileBackendState::Uninitialized) {
        return;
    }

    ams::Result rc = fs_runtime::EnsureReady();
    if (R_FAILED(rc)) {
        g_file_backend_state = FileBackendState::Disabled;
        return;
    }

    rc = fs_runtime::EnsureDirectoryExists(LogDirectory);
    if (R_FAILED(rc)) {
        g_file_backend_state = FileBackendState::Disabled;
        return;
    }

    g_file_backend_state = FileBackendState::Ready;
}

void WriteBytesToFile(const char *path, const void *data, size_t data_size) {
    EnsureFileBackendInitialized();
    if (g_file_backend_state != FileBackendState::Ready || path == nullptr || data == nullptr || data_size == 0) {
        return;
    }

    ams::fs::FileHandle file;
    const ams::Result open_rc = ams::fs::OpenFile(
        std::addressof(file),
        path,
        ams::fs::OpenMode_Write | ams::fs::OpenMode_AllowAppend);
    if (R_FAILED(open_rc)) {
        const ams::Result create_rc = ams::fs::CreateFile(path, 0);
        if (R_FAILED(create_rc) && !ams::fs::ResultPathAlreadyExists::Includes(create_rc)) {
            return;
        }

        const ams::Result retry_rc = ams::fs::OpenFile(
            std::addressof(file),
            path,
            ams::fs::OpenMode_Write | ams::fs::OpenMode_AllowAppend);
        if (R_FAILED(retry_rc)) {
            return;
        }
    }
    ON_SCOPE_EXIT { ams::fs::CloseFile(file); };

    s64 file_size = 0;
    if (R_FAILED(ams::fs::GetFileSize(std::addressof(file_size), file))) {
        return;
    }

    if (R_FAILED(ams::fs::WriteFile(file, file_size, data, data_size, ams::fs::WriteOption::Flush))) {
        return;
    }
}

void WriteLineToFile(const char *path, const char *line, size_t line_size) {
    WriteBytesToFile(path, line, line_size);
}

void EnsureFlushEventInitialized() {
    if (AMS_LIKELY(g_flush_event_initialized)) {
        return;
    }

    ::ams::os::InitializeEvent(std::addressof(g_flush_event), false, ::ams::os::EventClearMode_ManualClear);
    g_flush_event_initialized = true;
}

BlobQueueKind GetBlobQueueKind(const char *path) {
    if (path == nullptr) {
        return BlobQueueKind::Generic;
    }

    if (std::strstr(path, "probe-mitm-nifm.bin") != nullptr) {
        return BlobQueueKind::Nifm;
    }
    if (std::strstr(path, "probe-mitm-bsd.bin") != nullptr) {
        return BlobQueueKind::Bsd;
    }
    if (std::strstr(path, "probe-mitm-ssl.bin") != nullptr) {
        return BlobQueueKind::Ssl;
    }

    return BlobQueueKind::Generic;
}

const char *GetBlobQueueLabel(BlobQueueKind kind) {
    switch (kind) {
        case BlobQueueKind::Generic:
            return "binary";
        case BlobQueueKind::Nifm:
            return "binary-nifm";
        case BlobQueueKind::Bsd:
            return "binary-bsd";
        case BlobQueueKind::Ssl:
            return "binary-ssl";
        case BlobQueueKind::Count:
            break;
    }

    return "binary";
}

bool QueueLineLocked(const char *path, size_t path_size, const char *line, size_t line_size) {
    if (path == nullptr || line == nullptr || path_size == 0 || line_size == 0) {
        return false;
    }

    QueuedLine &entry = g_queue[g_queue_head];
    if (g_queue_count == QueueEntryCapacity) {
        g_queue_tail = (g_queue_tail + 1) % QueueEntryCapacity;
        --g_queue_count;
        ++g_dropped_line_count;
    }

    const size_t clamped_path_size = std::min(path_size, MaxQueuedPathLength - 1);
    const size_t clamped_line_size = std::min(line_size, MaxQueuedLineLength - 1);

    std::memcpy(entry.path, path, clamped_path_size);
    entry.path[clamped_path_size] = '\0';
    std::memcpy(entry.line, line, clamped_line_size);
    entry.line[clamped_line_size] = '\0';
    entry.path_size = static_cast<u16>(clamped_path_size);
    entry.line_size = static_cast<u16>(clamped_line_size);

    g_queue_head = (g_queue_head + 1) % QueueEntryCapacity;
    ++g_queue_count;
    return true;
}

bool PopLineLocked(QueuedLine *out) {
    if (out == nullptr || g_queue_count == 0) {
        return false;
    }

    *out = g_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1) % QueueEntryCapacity;
    --g_queue_count;
    return true;
}

bool QueueBlobLocked(const char *path, size_t path_size, const void *data, size_t data_size) {
    if (path == nullptr || data == nullptr || path_size == 0 || data_size == 0 || data_size > MaxQueuedBlobLength) {
        ++g_dropped_blob_counts[static_cast<size_t>(BlobQueueKind::Generic)];
        return false;
    }

    const BlobQueueKind kind = GetBlobQueueKind(path);
    const size_t kind_index = static_cast<size_t>(kind);
    QueuedBlob &entry = g_blob_queues[kind_index][g_blob_queue_heads[kind_index]];
    if (g_blob_queue_counts[kind_index] == BinaryQueueEntryCapacity) {
        g_blob_queue_tails[kind_index] = (g_blob_queue_tails[kind_index] + 1) % BinaryQueueEntryCapacity;
        --g_blob_queue_counts[kind_index];
        ++g_dropped_blob_counts[kind_index];
    }

    const size_t clamped_path_size = std::min(path_size, MaxQueuedPathLength - 1);
    std::memcpy(entry.path, path, clamped_path_size);
    entry.path[clamped_path_size] = '\0';
    std::memcpy(entry.data, data, data_size);
    entry.path_size = static_cast<u16>(clamped_path_size);
    entry.data_size = static_cast<u16>(data_size);

    g_blob_queue_heads[kind_index] = (g_blob_queue_heads[kind_index] + 1) % BinaryQueueEntryCapacity;
    ++g_blob_queue_counts[kind_index];
    return true;
}

bool PopBlobLocked(BlobQueueKind kind, QueuedBlob *out) {
    if (out == nullptr) {
        return false;
    }

    const size_t kind_index = static_cast<size_t>(kind);
    if (g_blob_queue_counts[kind_index] == 0) {
        return false;
    }

    *out = g_blob_queues[kind_index][g_blob_queue_tails[kind_index]];
    g_blob_queue_tails[kind_index] = (g_blob_queue_tails[kind_index] + 1) % BinaryQueueEntryCapacity;
    --g_blob_queue_counts[kind_index];
    return true;
}

void EmitDropWarning(const char *label, u64 dropped_count) {
    if (dropped_count == 0) {
        return;
    }

    char warning[160];
    const int warning_written = std::snprintf(
        warning,
        sizeof(warning),
        "[%llu] logger dropped %llu queued %s entries\n",
        static_cast<unsigned long long>(svcGetSystemTick()),
        static_cast<unsigned long long>(dropped_count),
        label);
    if (warning_written <= 0) {
        return;
    }

    const size_t warning_size = static_cast<size_t>(
        (warning_written < static_cast<int>(sizeof(warning))) ? warning_written : (sizeof(warning) - 1));
    EmitDebugString(warning, warning_size);
    WriteLineToFile(LogFilePath, warning, warning_size);
}

void EmitPendingDropWarnings(u64 dropped_line_count, const u64 *dropped_blob_counts, size_t dropped_blob_count_size) {
    EmitDropWarning("text", dropped_line_count);
    for (size_t i = 0; i < dropped_blob_count_size; ++i) {
        const auto kind = static_cast<BlobQueueKind>(i);
        EmitDropWarning(GetBlobQueueLabel(kind), dropped_blob_counts[i]);
    }
}

void FlushPendingEntries() {
    while (true) {
        size_t line_count = 0;
        size_t blob_count = 0;
        u64 dropped_line_count = 0;
        u64 dropped_blob_counts[static_cast<size_t>(BlobQueueKind::Count)] = {};
        bool empty = false;
        {
            std::scoped_lock lk(g_logger_lock);
            dropped_line_count = g_dropped_line_count;
            g_dropped_line_count = 0;
            for (size_t i = 0; i < static_cast<size_t>(BlobQueueKind::Count); ++i) {
                dropped_blob_counts[i] = g_dropped_blob_counts[i];
                g_dropped_blob_counts[i] = 0;
            }

            while (line_count < FlushLineBatchSize && PopLineLocked(std::addressof(g_flush_line_batch[line_count]))) {
                ++line_count;
            }

            for (size_t i = 0; i < static_cast<size_t>(BlobQueueKind::Count); ++i) {
                const auto kind = static_cast<BlobQueueKind>(i);
                for (size_t per_family = 0; per_family < FlushBlobBatchSizePerFamily; ++per_family) {
                    if (!PopBlobLocked(kind, std::addressof(g_flush_blob_batch[blob_count]))) {
                        break;
                    }
                    ++blob_count;
                }
            }

            empty = line_count == 0 && blob_count == 0;
        }

        EmitPendingDropWarnings(dropped_line_count, dropped_blob_counts, static_cast<size_t>(BlobQueueKind::Count));
        if (empty) {
            break;
        }

        for (size_t i = 0; i < line_count; ++i) {
            WriteLineToFile(g_flush_line_batch[i].path, g_flush_line_batch[i].line, g_flush_line_batch[i].line_size);
        }
        for (size_t i = 0; i < blob_count; ++i) {
            WriteBytesToFile(g_flush_blob_batch[i].path, g_flush_blob_batch[i].data, g_flush_blob_batch[i].data_size);
        }
    }
}

void FlushThreadMain(void *) {
    while (true) {
        const bool signaled = ::ams::os::TimedWaitEvent(std::addressof(g_flush_event), FlushInterval);
        if (signaled) {
            ::ams::os::ClearEvent(std::addressof(g_flush_event));
        }

        FlushPendingEntries();

        {
            std::scoped_lock lk(g_logger_lock);
            bool have_pending_blobs = false;
            for (size_t i = 0; i < static_cast<size_t>(BlobQueueKind::Count); ++i) {
                if (g_blob_queue_counts[i] != 0) {
                    have_pending_blobs = true;
                    break;
                }
            }
            if (g_flush_thread_stop_requested && g_queue_count == 0 && !have_pending_blobs) {
                break;
            }
        }
    }
}

void StartFlushThread() {
    EnsureFlushEventInitialized();
    if (AMS_LIKELY(g_flush_thread_started)) {
        return;
    }

    const ams::Result rc = ::ams::os::CreateThread(
        std::addressof(g_flush_thread),
        FlushThreadMain,
        nullptr,
        g_flush_thread_stack,
        sizeof(g_flush_thread_stack),
        23);
    AMS_ABORT_UNLESS(R_SUCCEEDED(rc));
    ::ams::os::SetThreadNamePointer(std::addressof(g_flush_thread), "wgnx-log");
    ::ams::os::StartThread(std::addressof(g_flush_thread));
    g_flush_thread_started = true;
}

void StopFlushThread() {
    if (!g_flush_thread_started) {
        return;
    }

    {
        std::scoped_lock lk(g_logger_lock);
        g_flush_thread_stop_requested = true;
    }
    ::ams::os::SignalEvent(std::addressof(g_flush_event));
    ::ams::os::WaitThread(std::addressof(g_flush_thread));
    g_flush_thread_started = false;
}

} // namespace

void Initialize() {
    std::scoped_lock lk(g_logger_lock);
    if (g_logger_initialized) {
        return;
    }

    g_flush_thread_stop_requested = false;
    g_logger_initialized = true;
    StartFlushThread();
    const char *banner = "\n=== net-probe boot ===\n";
    EmitDebugString(banner, std::strlen(banner));
    QueueLineLocked(LogFilePath, std::strlen(LogFilePath), banner, std::strlen(banner));
    ::ams::os::SignalEvent(std::addressof(g_flush_event));
}

void Shutdown() {
    {
        std::scoped_lock lk(g_logger_lock);
        if (!g_logger_initialized) {
            return;
        }
    }

    StopFlushThread();
}

void AppendLine(const char *path, const char *line) {
    if (path == nullptr || line == nullptr) {
        return;
    }

    const size_t path_size = std::strlen(path);
    const size_t line_size = std::strlen(line);
    if (path_size == 0 || line_size == 0) {
        return;
    }

    {
        std::scoped_lock lk(g_logger_lock);
        if (!g_logger_initialized) {
            return;
        }
        QueueLineLocked(path, path_size, line, line_size);
    }
    ::ams::os::SignalEvent(std::addressof(g_flush_event));
}

void AppendBytes(const char *path, const void *data, size_t size) {
    if (path == nullptr || data == nullptr || size == 0) {
        return;
    }

    const size_t path_size = std::strlen(path);
    if (path_size == 0) {
        return;
    }

    {
        std::scoped_lock lk(g_logger_lock);
        if (!g_logger_initialized) {
            return;
        }
        QueueBlobLocked(path, path_size, data, size);
    }
    ::ams::os::SignalEvent(std::addressof(g_flush_event));
}

void Log(const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }

    char line[640];
    const auto tick = static_cast<unsigned long long>(svcGetSystemTick());
    const int line_written = std::snprintf(line, sizeof(line), "[%llu] %s\n", tick, message);
    if (line_written <= 0) {
        return;
    }

    const size_t line_size = static_cast<size_t>((line_written < static_cast<int>(sizeof(line))) ? line_written : (sizeof(line) - 1));
    EmitDebugString(line, line_size);
    {
        std::scoped_lock lk(g_logger_lock);
        if (!g_logger_initialized) {
            return;
        }
        QueueLineLocked(LogFilePath, std::strlen(LogFilePath), line, line_size);
    }
    ::ams::os::SignalEvent(std::addressof(g_flush_event));
}

} // namespace wgnx::net_probe::logger
