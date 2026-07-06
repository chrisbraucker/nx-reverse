# 20.5.0 Resolver Path

## Scope

Verified against:

- `workspace/20.5.0/exefs/bsdsockets_main`
- `workspace/20.5.0/exefs/account_main`
- `workspace/20.5.0/exefs/nifm_main`
- `/opt/devkitpro/libnx/include/switch/services/sfdnsres.h`
- `/opt/devkitpro/libnx/include/netdb.h`
- `net-probe/lib/Atmosphere-libs/libstratosphere/source/socket/impl/socket_api.os.horizon.cpp`

## Answer

On Horizon `20.5.0`, the current evidence says the DNS resolver service does exist inside the
`bsdsockets` title.

The important service names present in `bsdsockets_main` are:

- `sfdnsres`
- `dns:priv`

This is no longer just a strings-only guess. `bsdsockets_main` contains a concrete registration
routine for both services.

Cross-check across the current extracted `*_main` set:

- only `bsdsockets_main` contains `sfdnsres`
- only `bsdsockets_main` contains `dns:priv`
- `nsd:u` appears more broadly
  - `bsdsockets_main`
  - `account_main`
  - `nifm_main`
  - `netConnect_main`

That split matters. It supports treating `sfdnsres` / `dns:priv` as the resolver IPC surface,
while `nsd:u` remains a more general shared service dependency.

## Confirmed registration path in `bsdsockets_main`

`FUN_71000d0bcc(...)` registers:

- `sfdnsres` via `FUN_71000d9994(param_1, 0, ..., 8, ...)`
- `dns:priv` via `FUN_71000d9994(param_1, 0, ..., 1, ...)`

After those two registrations succeed, the same function starts eight worker instances named:

- `nn.socket.ResolverIpcServer`

The direct xrefs are:

- `sfdnsres`
  - string at `0x710012e76d`
  - data xref from `0x71000d0bf4`
- `dns:priv`
  - string at `0x710012e776`
  - data xref from `0x71000d0c50`
- `nn.socket.ResolverIpcServer`
  - string at `0x710012004b`
  - parameter xrefs from eight call sites inside `FUN_71000d0bcc(...)`

The surrounding bootstrap function is:

- `FUN_71000d0ff0(...)`
  - initializes the resolver-server state block
  - calls `FUN_71000d0bcc(...)`

This matches the already recovered `bsd:s` / `bsd:a` / `bsd:u` registration style closely enough
that the resolver should be treated as another real IPC surface hosted by `bsdsockets_main`, not
just a library-only helper.

## Resolver code present in `bsdsockets_main`

The binary also contains the expected libc-style resolver entry points and labels:

- `getaddrinfo`
- `gethostbyaddr`
- `resolver_error`
- `ResolverCacheFindAndPossiblyRemoveIpAddress`
- `ResolverSetDnsServersIntoTLS`

Important note:

- `ResolverSetDnsServersIntoTLS` here appears to refer to thread-local storage for the resolver
  state, not Transport Layer Security.

Recovered helpers:

- `FUN_7100083350(...)`
  - large `getaddrinfo`-side implementation
  - contains a local `hosts` path and a larger network-backed resolver path
- `FUN_7100088cd0(...)`
  - `gethostbyaddr`-side helper
  - explicitly routes through a `"hosts"` source first
- `FUN_710008a6b0(...)`
  - updates resolver per-thread state and logs
    `__res_thread structure is NULL for %s`
  - called with the label `ResolverSetDnsServersIntoTLS`

So the resolver is not only registered as an IPC service. The expected socket resolver
implementation also lives in the same binary.

## Correlation with local SDK headers

The local `libnx` headers line up with the reverse findings directly.

`/opt/devkitpro/libnx/include/switch/services/sfdnsres.h` documents:

- `sfdnsresGetHostByNameRequest(...)`
- `sfdnsresGetHostByAddrRequest(...)`
- `sfdnsresGetAddrInfoRequest(...)`
- `sfdnsresGetNameInfoRequest(...)`
- `sfdnsresGetCancelHandleRequest(...)`
- `sfdnsresCancelRequest(...)`

Two points matter:

1. the file describes `sfdnsres` as the domain name resolution service IPC wrapper
2. the request APIs take a `bool use_nsd`

That second point is especially useful:

- it suggests `nsd` is a selectable backend/input to resolver requests
- it does not suggest that generic socket DNS bypasses `sfdnsres` entirely in favor of a separate
  public `nsd`-only path

`/opt/devkitpro/libnx/include/netdb.h` also explicitly tells users to use the standard BSD-style
resolver surface:

- `gethostbyname`
- `gethostbyaddr`
- `getaddrinfo`
- `getnameinfo`

That is consistent with the `bsdsockets_main` implementation above.

## Relationship to `account_main` and `nifm_main`

`account_main`, `nifm_main`, and `netConnect_main` all show signs of `nsd` usage.

The clearest currently observed strings are in `account_main` and `nifm_main`:

- `Couldn't resolve proxy name`
- `Error: Failed to allocate memory for nn::nsd::ResolveEx().`
- `Error: nn::nsd::ResolveEx() failed. Err Desc: %d`
- `nsd:u`

This is strong evidence that those modules do perform `nsd`-backed name resolution for at least
some higher-level tasks, especially proxy handling and service/FQDN lookups.

Current interpretation:

- `bsdsockets_main` owns the generic socket resolver IPC surface
  - `sfdnsres`
  - `dns:priv`
- `account_main` and `nifm_main` also call into `nsd`
  - likely for specific higher-level name-resolution tasks
  - not yet proven to be the ordinary `getaddrinfo` path used by application sockets

That last point is still an inference, but it is the best fit with the combined binary and header
evidence.

## What this means for probing

The earlier ambiguity is now resolved enough to guide the next runtime step:

- there is no need to keep treating DNS as "maybe hidden in some unknown module"
- the first resolver-specific trace target should be `sfdnsres`
- `dns:priv` should be treated as a likely private companion surface

`bsd:u` still matters for actual TCP/UDP transport, but resolver traffic is now best modeled as a
parallel IPC path hosted by the same `bsdsockets` title.

## Current open questions

- whether ordinary `getaddrinfo()` from user titles reaches `sfdnsres` directly or through another
  local wrapper first
- whether `dns:priv` is only used internally by `sfdnsres`, or whether some privileged callers use
  it directly
- whether the resolver path is what we previously failed to observe while tracing only
  `nifm:u`, `bsd:u`, and `ssl`
- whether any part of the resolver lifecycle still depends on `bsd:a`

## Practical next step

For the runtime probe, the next DNS-focused step should be:

1. add a narrow passive MITM for `sfdnsres`
2. keep it isolated from the existing `bsd:u` / `ssl` tracer logic
3. rerun the controlled requester DNS scenario
4. correlate `getaddrinfo()` with the observed `sfdnsres` command flow

That should answer the remaining question of where requester DNS lives on-device without having to
guess from `bsd:a` side effects alone.
