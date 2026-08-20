# SSL MITM Procedures

The field-level contract is [`../contracts/services/ssl.json`](../contracts/services/ssl.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.
Read the shared [`listener lifecycle`](listener-lifecycle.md) before changing MITM setup or teardown.

## Passive MITM

Forward root-service context creation without changing the service variant, version, caller metadata, or returned objects.
Follow the context and connection objects that the client creates naturally.

Record only the decoded hostname, socket descriptor, peer sockaddr, scalar settings, and buffer metadata.
Unknown commands remain opaque IPC records.

Do not disable certificate verification, replace certificates, alter hostnames, rewrite payloads, or add artificial timing.
`ssl` and `ssl:s` remain distinct MITM targets even where their decoded object flow overlaps.

## Listener Lifecycle

Treat each returned SSL context and connection object as a child of the accepted root session that produced it.
Keep the object path through context creation and connection creation so decoded settings and socket descriptors retain their client ownership.

Do not reuse a context or connection object across root sessions, including a later launch by the same title.
When a child object closes, keep the root-session trace while sibling objects or handles remain live.
On final close, release the full context and connection graph and confirm that listener resources return to baseline before treating a relaunch failure as an SSL finding.
