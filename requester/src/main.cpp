#include <switch.h>

#include <vector>

#include "config.hpp"
#include "logger.hpp"
#include "runtime.hpp"
#include "scenarios.hpp"

namespace {

using namespace requester;

void PrintSummary(AppContext& ctx, const std::vector<ScenarioResult>& results) {
    for (const auto& result : results) {
        const char *state = result.skipped ? "SKIP" : (result.success ? "OK" : "FAIL");
        logger::Status(
            ctx,
            "[%s] %s | sent=%zu recv=%zu | %s",
            state,
            result.name.c_str(),
            result.bytes_sent,
            result.bytes_received,
            result.detail.c_str());
    }
}

} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(nullptr);

    AppContext ctx {};

    std::printf("%s\n", APP_TITLE);
    consoleUpdate(nullptr);

    if (!EnsureLogDirectories()) {
        std::printf("Failed to create %s\n", config::LogDirectory);
        consoleUpdate(nullptr);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }

    if (!logger::OpenLog(ctx)) {
        std::printf("Failed to open requester log\n");
        consoleUpdate(nullptr);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }

    logger::Log(ctx, "requester start title=%s version=%s-%s", APP_TITLE, VERSION, BUILD_ID);
    logger::Log(ctx, "hos_version=%s", FormatHosVersion().c_str());

    const Result socket_rc = socketInitializeDefault();
    logger::Log(ctx, "socketInitializeDefault -> %s", FormatResult(socket_rc).c_str());
    if (R_FAILED(socket_rc)) {
        logger::Status(ctx, "socketInitializeDefault failed: %s", FormatResult(socket_rc).c_str());
        logger::CloseLog(ctx);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }

    const Result nifm_rc = nifmInitialize(NifmServiceType_User);
    logger::Log(ctx, "nifmInitialize(User) -> %s", FormatResult(nifm_rc).c_str());
    if (R_FAILED(nifm_rc)) {
        logger::Status(ctx, "nifmInitialize(User) failed: %s", FormatResult(nifm_rc).c_str());
        socketExit();
        logger::CloseLog(ctx);
        SleepMilliseconds(config::ExitDelayMs);
        consoleExit(nullptr);
        return 1;
    }

    ctx.ssl_initialize_rc = sslInitialize(3);
    ctx.ssl_initialized = R_SUCCEEDED(ctx.ssl_initialize_rc);
    logger::Log(ctx, "sslInitialize(3) -> %s", FormatResult(ctx.ssl_initialize_rc).c_str());

    logger::Status(ctx, "Requester run id: %s", ctx.run_id.c_str());
    logger::Status(ctx, "Log path: %s", ctx.log_path.c_str());
    logger::Status(ctx, "HOS Version: %s", FormatHosVersion().c_str());

    const auto results = RunScenarios(ctx);
    PrintSummary(ctx, results);

    logger::Status(ctx, "Requester finished; exiting in %u seconds", config::ExitDelayMs / 1000U);
    logger::Log(ctx, "requester finished");

    if (ctx.ssl_initialized) {
        sslExit();
    }
    nifmExit();
    socketExit();
    logger::CloseLog(ctx);

    SleepMilliseconds(config::ExitDelayMs);
    consoleExit(nullptr);
    return 0;
}
