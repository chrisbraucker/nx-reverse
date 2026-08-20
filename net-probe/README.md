# Net Probe

## Configuration

The compiled MITM target set is selected at boot through Atmosphère's `system_settings.ini`.

Use the `[net_probe]` section.

Missing keys disable their target.

For the qLaunch early-boot `nifm:s` timing experiment, enable only `nifm:s`:

```ini
[net_probe]
enable_nifm_s = u8!0x1
```

The currently compiled target keys are `enable_nifm_u`, `enable_nifm_s`, `enable_bsd_u`, `enable_bsd_s`, `enable_bsd_a`, `enable_ssl`, `enable_ssl_s`, and `enable_notif_s`.

The setting is sampled once during net-probe startup.

The compiled `nifm:s` target is qLaunch only.

The `notif:s` trace remains available as a separate target and records only command `8000`, `RegisterAppletResourceUserId`, plus its client identity, monotonic request and response timestamps, result, and duration.

Logs use the existing fixed-capacity in-memory queue and dedicated flush thread.

## Verification

Formatting and static-analysis checks remain enabled for first-party sources.
Clang-tidy is intentionally deferred because it must parse the included Atmosphere headers, which currently produce target-specific Clang parser diagnostics outside this project's ownership.
Header filters cannot avoid those parser failures because dependency headers must be parsed before clang-tidy can analyze `net-probe` translation units.

## Local Build Overrides

Copy `local.mk.example` to `local.mk` to keep device-specific build overrides outside Git history.
Set `TOOLBOX_FORWARDER_PROGRAM_ID` there to the installed toolbox or toolbox-forwarder title ID that the toolbox's probe should admit.
Target builds fail before compilation when that variable is absent.
Use `EXTRA_DEFINES` in the same file only for focused local experiments.
