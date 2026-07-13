#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern "C" u32 __nx_applet_type;
extern "C" void __libnx_init_time(void);
extern "C" void __libnx_init_cwd(void);
extern "C" void __attribute__((weak)) __nx_win_init(void);
extern "C" void __attribute__((weak)) userAppInit(void);

namespace {

constexpr const char *StartupLogPath = "sdmc:/nxrv/requester/requester-startup.log";
constexpr size_t MaxStartupEvents = 48;

struct StartupEvent {
    const char *phase;
    Result rc;
    bool has_rc;
};

StartupEvent g_startup_events[MaxStartupEvents];
size_t g_startup_event_count = 0;
bool g_startup_file_ready = false;

void EmitDebugString(const char *line) {
    svcOutputDebugString(line, std::strlen(line));
}

void AppendStartupFileLine(const char *line) {
    if (!g_startup_file_ready) {
        return;
    }

    FILE *file = std::fopen(StartupLogPath, "a");
    if (file == nullptr) {
        return;
    }

    std::fprintf(file, "%s\n", line);
    std::fflush(file);
    std::fclose(file);
}

void FormatAndEmit(const char *fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    EmitDebugString(line);
    AppendStartupFileLine(line);
}

void RecordStartupEvent(const char *phase, Result rc, bool has_rc) {
    if (g_startup_event_count < MaxStartupEvents) {
        g_startup_events[g_startup_event_count++] = StartupEvent{phase, rc, has_rc};
    }

    if (has_rc) {
        FormatAndEmit("requester startup: %s rc=0x%08x", phase, rc);
    } else {
        FormatAndEmit("requester startup: %s", phase);
    }
}

void EnableStartupFileLog() {
    mkdir("sdmc:/nxrv", 0777);
    mkdir("sdmc:/nxrv/requester", 0777);

    g_startup_file_ready = true;
    AppendStartupFileLine("requester startup: file log ready");

    for (size_t i = 0; i < g_startup_event_count; ++i) {
        const StartupEvent &event = g_startup_events[i];
        if (event.has_rc) {
            FormatAndEmit("requester startup replay: %s rc=0x%08x", event.phase, event.rc);
        } else {
            FormatAndEmit("requester startup replay: %s", event.phase);
        }
    }
}

void AbortWithTrace(const char *phase, Result rc, int error) {
    RecordStartupEvent(phase, rc, true);
    diagAbortWithResult(MAKERESULT(Module_Libnx, error));
}

} // namespace

extern "C" void __appInit(void) {
    Result rc;

    RecordStartupEvent("__appInit enter", 0, false);

    RecordStartupEvent("smInitialize begin", 0, false);
    rc = smInitialize();
    if (R_FAILED(rc)) {
        AbortWithTrace("smInitialize failed", rc, LibnxError_InitFail_SM);
    }
    RecordStartupEvent("smInitialize complete", rc, true);

    if (hosversionGet() == 0) {
        RecordStartupEvent("setsysInitialize begin", 0, false);
        rc = setsysInitialize();
        RecordStartupEvent("setsysInitialize complete", rc, true);
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            RecordStartupEvent("setsysGetFirmwareVersion begin", 0, false);
            rc = setsysGetFirmwareVersion(&fw);
            RecordStartupEvent("setsysGetFirmwareVersion complete", rc, true);
            if (R_SUCCEEDED(rc)) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            }
            setsysExit();
            RecordStartupEvent("setsysExit complete", 0, false);
        }
    } else {
        RecordStartupEvent("hosversion already set", hosversionGet(), true);
    }

    RecordStartupEvent("appletInitialize begin", 0, false);
    rc = appletInitialize();
    if (R_FAILED(rc)) {
        AbortWithTrace("appletInitialize failed", rc, LibnxError_InitFail_AM);
    }
    RecordStartupEvent("appletInitialize complete", rc, true);

    if (__nx_applet_type != static_cast<u32>(AppletType_None)) {
        RecordStartupEvent("hidInitialize begin", 0, false);
        rc = hidInitialize();
        if (R_FAILED(rc)) {
            AbortWithTrace("hidInitialize failed", rc, LibnxError_InitFail_HID);
        }
        RecordStartupEvent("hidInitialize complete", rc, true);
    } else {
        RecordStartupEvent("hidInitialize skipped applet_type_none", 0, false);
    }

    RecordStartupEvent("timeInitialize begin", 0, false);
    rc = timeInitialize();
    if (R_FAILED(rc)) {
        AbortWithTrace("timeInitialize failed", rc, LibnxError_InitFail_Time);
    }
    RecordStartupEvent("timeInitialize complete", rc, true);

    RecordStartupEvent("__libnx_init_time begin", 0, false);
    __libnx_init_time();
    RecordStartupEvent("__libnx_init_time complete", 0, false);

    RecordStartupEvent("fsInitialize begin", 0, false);
    rc = fsInitialize();
    if (R_FAILED(rc)) {
        AbortWithTrace("fsInitialize failed", rc, LibnxError_InitFail_FS);
    }
    RecordStartupEvent("fsInitialize complete", rc, true);

    RecordStartupEvent("fsdevMountSdmc begin", 0, false);
    rc = fsdevMountSdmc();
    RecordStartupEvent("fsdevMountSdmc complete", rc, true);
    if (R_SUCCEEDED(rc)) {
        EnableStartupFileLog();
    }

    RecordStartupEvent("__libnx_init_cwd begin", 0, false);
    __libnx_init_cwd();
    RecordStartupEvent("__libnx_init_cwd complete", 0, false);

    if (&__nx_win_init) {
        RecordStartupEvent("__nx_win_init begin", 0, false);
        __nx_win_init();
        RecordStartupEvent("__nx_win_init complete", 0, false);
    } else {
        RecordStartupEvent("__nx_win_init absent", 0, false);
    }

    if (&userAppInit) {
        RecordStartupEvent("userAppInit begin", 0, false);
        userAppInit();
        RecordStartupEvent("userAppInit complete", 0, false);
    } else {
        RecordStartupEvent("userAppInit absent", 0, false);
    }

    RecordStartupEvent("__appInit exit", 0, false);
}
