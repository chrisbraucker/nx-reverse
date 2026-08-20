# BSD MITM Procedures

The field-level contract is [`../contracts/services/bsd.json`](../contracts/services/bsd.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.

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

## Lifecycle Diagnosis

To isolate a relaunch or ownership failure, begin with one data session, transfer memory, and `RegisterClient`.
Verify that a second identical launch succeeds before adding the monitor session, root clones, sockets, and traffic in that order.

This ladder is a diagnostic procedure.
It is not a production initialization recipe.

## Controlled SendTo Mutation

Only mutate a `SendTo` destination after the client has completed its own normal lifecycle.
Match one exact configured IPv4 destination, copy its sockaddr into MITM-owned storage, and replace the corresponding static descriptor.
Never write to client-owned memory.
