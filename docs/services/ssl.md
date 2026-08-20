# SSL MITM Procedures

The field-level contract is [`../contracts/services/ssl.json`](../contracts/services/ssl.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.

## Passive MITM

Forward root-service context creation without changing the service variant, version, caller metadata, or returned objects.
Follow the context and connection objects that the client creates naturally.

Record only the decoded hostname, socket descriptor, peer sockaddr, scalar settings, and buffer metadata.
Unknown commands remain opaque IPC records.

Do not disable certificate verification, replace certificates, alter hostnames, rewrite payloads, or add artificial timing.
`ssl` and `ssl:s` remain distinct MITM targets even where their decoded object flow overlaps.
