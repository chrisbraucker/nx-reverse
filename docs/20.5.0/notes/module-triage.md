# 20.5.0 Module Triage

When extracted binaries have opaque names, keep the original filename and sort
candidates into these buckets based on strings, imports, and nearby notes.

High-priority targets:

- `bsd`
  - look for strings such as `bsd:u`, `bsd:s`, `bsd:nu`
  - look for `ISfUserServiceCreator`, `ISfAssignedNetworkInterfaceService`
- `nifm`
  - look for `nifm`, `IRequest`, `RegisterSocketDescriptor`
- `ifcfg`
  - look for `ifcfg`, interface configuration, IP/DNS reporting
- `anif`
  - look for `anif`, `ISfNetworkInterfaceService`, `CreateSharedMemory`
  - also check for `GetRxQueue`, `GetTxQueue`, `BringUp`, `StartCommunication`

If a binary is still unidentified:

1. Leave it in `raw/` with its original name.
2. Add any quick string matches to this notes directory.
3. Copy it into a module bucket only once there is a concrete reason.
