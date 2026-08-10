# Net Probe

## Verification

Formatting and static-analysis checks remain enabled for first-party sources.
Clang-tidy is intentionally deferred because it must parse the included Atmosphere headers, which currently produce target-specific Clang parser diagnostics outside this project's ownership.
Header filters cannot avoid those parser failures because dependency headers must be parsed before clang-tidy can analyze `net-probe` translation units.

## Local Build Overrides

Copy `local.mk.example` to `local.mk` to keep device-specific build overrides outside Git history.
Set `TOOLBOX_FORWARDER_PROGRAM_ID` there to the installed toolbox or toolbox-forwarder title ID that the toolbox's probe should admit.
Target builds fail before compilation when that variable is absent.
Use `EXTRA_DEFINES` in the same file only for focused local experiments.
