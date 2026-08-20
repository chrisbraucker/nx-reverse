# Notification MITM Procedures

The field-level contract is [`../contracts/services/notif.json`](../contracts/services/notif.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.
Read the shared [`listener lifecycle`](listener-lifecycle.md) before changing MITM setup or teardown.

`notif:s` is intentionally a one-command observation target.
Forward command `8000`, `RegisterAppletResourceUserId`, unchanged and record only caller identity, request and response timing, result, and duration.

Do not retain the request payload.
Do not probe, synthesize, or document behavior for other notification commands until a separate controlled experiment establishes a bounded need.

## Listener Lifecycle

Treat each accepted `notif:s` root as a self-contained observation lifetime.
Record acceptance, command `8000`, the response disposition, and final handle or session close without retaining the request payload.

Do not assume that one observed command creates a reusable child object or authorizes discovery of adjacent commands.
Release the listener-side session record at final close and verify that repeated applet launches return listener resource counts to baseline.
