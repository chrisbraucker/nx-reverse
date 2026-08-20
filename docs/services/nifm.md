# NIFM MITM Procedures

The field-level contract is [`../contracts/services/nifm.json`](../contracts/services/nifm.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.
Read the shared [`listener lifecycle`](listener-lifecycle.md) before changing MITM setup or teardown.

## Passive MITM

Forward the root, general-service, and request-object calls without creating a second NIFM request.
Correlate `IRequest` state, result, event handles, and socket-descriptor operations by the trace object path.

NIFM exposes connectivity policy and lifecycle state.
It is not a packet-routing interface.

## Listener Lifecycle

Treat the root service, returned general-service object, and each returned request object as one client-owned object graph.
Record the exact path that produced an `IRequest` and keep it associated with the accepting root session until it closes.

Do not create a replacement request after a request returns an error or closes.
Do not carry a request object, event handle, or socket-descriptor association into another client session.
When the client closes a child object, retain the root-session trace until its remaining child objects and handles have closed.

The listener may observe a partial request graph when a client abandons setup.
Forward and record that result without completing the graph or inferring an unobserved request state.

## Descriptor Lifecycle

`RegisterSocketDescriptor` and `UnregisterSocketDescriptor` are state-sensitive operations on `IRequest`.
The recovered firmware `20.5.0` contract permits them only while the request state is `3`.

Register exactly one descriptor when no descriptor is tracked.
Unregister the tracked descriptor or `-1` before attempting a replacement.
Treat a mismatched descriptor or a different request state as a lifecycle failure rather than reusing the request blindly.

Passive MITM must observe this sequence rather than injecting it.
