# NIFM MITM Procedures

The field-level contract is [`../contracts/services/nifm.json`](../contracts/services/nifm.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.

## Passive MITM

Forward the root, general-service, and request-object calls without creating a second NIFM request.
Correlate `IRequest` state, result, event handles, and socket-descriptor operations by the trace object path.

NIFM exposes connectivity policy and lifecycle state.
It is not a packet-routing interface.

## Descriptor Lifecycle

`RegisterSocketDescriptor` and `UnregisterSocketDescriptor` are state-sensitive operations on `IRequest`.
The recovered firmware `20.5.0` contract permits them only while the request state is `3`.

Register exactly one descriptor when no descriptor is tracked.
Unregister the tracked descriptor or `-1` before attempting a replacement.
Treat a mismatched descriptor or a different request state as a lifecycle failure rather than reusing the request blindly.

Passive MITM must observe this sequence rather than injecting it.
