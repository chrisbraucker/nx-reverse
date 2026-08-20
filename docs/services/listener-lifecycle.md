# Passive MITM Listener Lifecycle

This procedure applies to passive `net-probe` targets.
It describes listener ownership rather than an IPC contract a client must invoke.

## Listener Startup

Register the complete configured target set before accepting client traffic.
If any target cannot be registered, tear down the partial listener set and report startup failure.
Do not silently omit a requested target because that changes the experiment's observation boundary.

All targets share the listener's server-manager resources.
Reserve capacity for long-lived system sessions, their domain objects, and short-lived client work before expanding the target set.
Capacity exhaustion is a listener failure, not evidence about a client IPC contract.

## Session and Object Ownership

Treat every accepted root session as a separate client lifetime.
Forward its requests and responses unchanged while recording the session identity and service target.

Associate every cloned session and returned domain object with the root session that produced it.
Do not reuse a clone, domain object, context, request, or connection for another accepted client.
Preserve the observed object path so later calls can be interpreted without guessing their owner.

Keep a client lifetime alive after one handle closes when the client still owns another root handle, clone, or live domain object.
A handle-close event is not necessarily client disconnection.
Release listener-side tracking only after the last associated client handle and object has closed.

## Forwarding and Teardown

Forward unknown commands as opaque IPC records.
Do not synthesize missing setup calls, compensate for a failed client call, or use a teardown event to probe additional commands.

Record domain-object close and root-session close separately when the service exposes both.
Close or release listener-side child tracking before its parent root tracking, mirroring the client object graph.
After final client teardown, check that the listener's active-session and active-domain counts return to the expected baseline.

These rules make relaunch failures and resource leaks observable without turning a passive listener into a second client.
