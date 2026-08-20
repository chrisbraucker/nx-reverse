# BSD MITM Procedures

The field-level contract is [`../contracts/services/bsd.json`](../contracts/services/bsd.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.
Read the shared [`listener lifecycle`](listener-lifecycle.md) before changing MITM setup or teardown.

## Passive MITM

Passive BSD observation forwards every request, PID descriptor, handle, clone, and close exactly as received.
Do not fabricate `RegisterClient`, `StartMonitoring`, or a socket operation to make a trace look complete.

The service name matters even when the packet traffic does not.
`bsd:s` is required for system-capability clients, while `bsd:u` and `bsd:a` have their own client populations.

## Common Client Shape

The observed libnx shape opens two root BSD sessions.
The data session creates transfer memory and calls `RegisterClient`.
The short monitor session calls `StartMonitoring` with the client identifier returned by registration.

Additional data sessions are clones of the data root when the client configures more than one BSD session.
Close cloned sessions before their owning root sessions and preserve client PID ownership through the full teardown.

This is common observed libnx behavior on firmware `20.5.0`.
It is not a claim that every BSD client always needs two sessions.

## Listener Lifecycle

Treat the data root, monitor root, and each data-root clone as separate listener resources.
The data root owns `RegisterClient` and the transfer-memory lifecycle.
The monitor root owns `StartMonitoring` and receives the client identifier returned by registration.

Do not merge the monitor root into the data root or create either root on the client's behalf.
Track a clone as a child of its data root until the clone closes, even when the client closes another handle first.
Only release a BSD client lifetime after its roots, clones, and any associated domain objects have all closed.

For a controlled listener change, verify two consecutive client launches with no growth in active listener resources.
That check catches leaked clone, monitor, and domain ownership before packet traffic obscures the cause.

## Lifecycle Diagnosis

To isolate a relaunch or ownership failure, begin with one data session, transfer memory, and `RegisterClient`.
Verify that a second identical launch succeeds before adding the monitor session, root clones, sockets, and traffic in that order.

This ladder is a diagnostic procedure.
It is not a production initialization recipe.

## Controlled SendTo Mutation

Only mutate a `SendTo` destination after the client has completed its own normal lifecycle.
Match one exact configured IPv4 destination, copy its sockaddr into MITM-owned storage, and replace the corresponding static descriptor.
Never write to client-owned memory.
