# Glue notification-state persistence

## Scope

This note records a firmware `20.5.0` Glue crash investigation on 2026-08-13.

The investigation combines a persisted fatal report, a controlled delayed-response experiment, static analysis, and the published Glue service table.

## Finding

Glue's notification-service implementation receives the registration command documented as `RegisterAppletResourceUserId`.

The relevant registration path can persist Glue-owned migration-related state.

If its filesystem operation returns an error, Glue deliberately enters its fatal path.

The observed fatal result is produced while Glue tries to report that failure through `fatal:u`.

It does not expose the original filesystem error.

The current Glue save archive validates structurally, so the available export is not evidence of persistent save corruption.

No direct Glue-to-NIM IPC dependency is visible in the current service-access inventory.

The public command name and role are documented in [Switchbrew's Glue services reference](https://switchbrew.org/wiki/Glue_services).

## Controlled response timing result

Delaying the patched NIM connection-test time response by 60 seconds prevented the Glue crash during the observed run.

NIM had already issued multiple connection-test requests and an unrelated observed service request before the first delayed response was released.

A successful time response is therefore not, by itself, sufficient to trigger the Glue crash.

The delay may change ordering or timing before another process issues Glue's notification-service registration request.

It must not yet be treated as a Glue fix or as proof of a direct network-to-Glue dependency.

## Next question

Passively record notification-service caller identity, command ID, and timestamp.

Then correlate registration command `8000` with immediate and delayed connection-test response runs.

This should identify the caller that reaches Glue's failing persistence path and distinguish a timing dependency from a state dependency.

`net-probe` now provides this as a startup-configured `notif:s` passive target and records only the registration command.

No device capture from that target has been interpreted yet.
