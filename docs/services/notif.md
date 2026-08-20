# Notification MITM Procedures

The field-level contract is [`../contracts/services/notif.json`](../contracts/services/notif.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.

`notif:s` is intentionally a one-command observation target.
Forward command `8000`, `RegisterAppletResourceUserId`, unchanged and record only caller identity, request and response timing, result, and duration.

Do not retain the request payload.
Do not probe, synthesize, or document behavior for other notification commands until a separate controlled experiment establishes a bounded need.
