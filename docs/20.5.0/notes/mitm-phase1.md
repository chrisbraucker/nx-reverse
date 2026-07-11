# 20.5.0 Passive MITM Phase 1

## 2026-06-29: pivot to split `nifm` / `bsd` passive capture

The probe now registers passive MITM servers for:

- `nifm:u`
- `nifm:s`
- `bsd:u`
- `bsd:s`
- `bsd:a`

Trace output is now split by service family instead of sharing one mixed file:

- `sdmc:/wgnx/probe-mitm-nifm.jsonl`
- `sdmc:/wgnx/probe-mitm-nifm-meta.json`
- `sdmc:/wgnx/probe-mitm-bsd.jsonl`
- `sdmc:/wgnx/probe-mitm-bsd-meta.json`

Current `bsd` trace additions:

- the passive MITM stays transparent; requests are still forwarded unchanged
- `bsd` traffic is labeled with public libnx-aligned root command names
- `bsd_semantic` records now summarize the high-value socket operations such as:
  - `RegisterClient`
  - `Socket`
  - `Connect`
  - `Send` / `Recv`
  - `Bind` / `Listen` / `Accept`
  - `SetSockOpt`
  - `Shutdown`

This is intentionally still an observation layer, not a traffic-rewriting layer.
The immediate runtime goal is to recover:

1. which titles actually open `bsd:*` sessions in the target scenarios
2. the initialization and monitoring sequence around `RegisterClient` / `StartMonitoring`
3. the concrete socket syscall mix used by homebrew and system clients

## 2026-06-21: compiled passive observation layer

Phase 1 now exists in `net-probe` and compiles into the probe title rather than
the main WireGuard sysmodule.

The current MITM coverage is intentionally passive and currently narrowed to
one target while `sm` registration state is being stabilized:

- `nifm:u`

## Temporary fork warning

This Phase 1 MITM path is temporary instrumentation, not an upstream-compatible
integration shape.

The current build depends on local changes inside the vendored
`net-probe/lib/Atmosphere-libs` subtree. Those changes are compiled into the
probe itself, so they do not require a patched Atmosphere installation on the
device. They do, however, create a source-level fork of `libstratosphere`.

Treat the current MITM build accordingly:

- use it to gather runtime traces and protocol observations
- do not treat the patched libstratosphere path as product architecture
- do not build further permanent abstractions on top of the local fork unless
  strictly needed for data collection
- plan to remove or isolate the vendored library changes after the current
  trace-gathering pass

Cleanup requirement:

- before hardening the real VPN integration path, revert the vendored
  `Atmosphere-libs` modifications and re-implement only the minimum necessary
  MITM/probe functionality against upstream/public APIs

The forward path is unchanged. The MITM layer only:

- accepts MITM sessions
- keeps the original forward `Service`
- records per-session metadata
- records forwarded request/response metadata through a libstratosphere hook

## Current implementation shape

### Service registration

The probe now starts MITM servers during its own startup path:

- `net-probe/src/main.cpp`
- `net-probe/src/mitm_service.cpp`

Important details:

- the probe excludes its own program ID `0x010000000000EAD1` from MITM capture
- each accepted MITM session is assigned a local monotonically increasing session ID
- the MITM server runs on a dedicated thread named `wgnx-mitm`

### Trace logging

Trace output is written to:

- `sdmc:/atmosphere/logs/probe-mitm.jsonl`
- `sdmc:/atmosphere/logs/probe-mitm-meta.json`

Current implementation:

- [mitm_trace.cpp](/workspaces/nx-reversing.git/net-probe/src/mitm_trace.cpp:1)
- [TRACE_SCHEMA.md](/workspaces/nx-reversing.git/docs/methods/TRACE_SCHEMA.md:1)

Currently logged:

- `trace_start`
- `sm_mitm`
- `client_connected`
- `error`
- `ipc_request`
- `ipc_response`
- `ipc_buffer`
- `ipc_handle`

Captured fields now include:

- service name
- caller program ID
- local MITM session ID
- command ID when CMIF parsing succeeds
- domain object ID when present
- request/response buffer previews and SHA-256
- moved/copied handle metadata
- forwarded result value
- coarse monotonic timing

## libstratosphere changes made locally

The bundled `libstratosphere` was extended so the passive MITM layer can observe
forwarded traffic without changing service behavior.

Key local changes:

- MITM session accept paths now preserve:
  - `sm::ServiceName`
  - `sm::MitmProcessInfo`
  - local MITM session ID
- MITM forwarding now exposes a monitoring callback
- `sm::mitm` lifecycle calls now expose a monitoring callback
- MITM server registration now accepts an explicit query callback instead of
  requiring `Interface::ShouldMitm`

This last change was necessary because the generated service interface type and
the implementation type have different template roles in Atmosphere's object
factory. Using the generated interface directly was the clean path.

These modifications are acceptable only as temporary instrumentation. They
should not remain the default long-term dependency shape of this repository.

## What is still unverified

- on-device trace output from the compiled Phase 1 probe
- the exact command mix seen for:
  - flight mode
  - Wi-Fi enabled but unassociated
  - Wi-Fi connected
- whether the new `sm_mitm` lifecycle records cleanly separate:
  - pre-existing MITM state
  - MITM install success/failure
  - `ClearFutureMitm` behavior
  - session acknowledgement behavior

## Immediate next runtime task

Deploy the newly built `net-probe` binary and collect the first passive traces
for:

1. flight mode
2. Wi-Fi enabled but unassociated
3. Wi-Fi connected

The goal of the next trace run is not packet work yet. It is to recover:

1. `sm` MITM lifecycle state for `nifm:u`
2. real `nifm:u` session flow and command ordering

## 2026-06-28: `nifm:s` tracer refinement

The passive tracer now keeps a local, trace-only map of forwarded domain object
IDs per MITM session. This does not change forwarding behavior; it only makes
the JSON trace easier to read.

New trace behavior:

- `ipc_request`, `ipc_response`, `ipc_decode_*`, and `ipc_handle` now include an
  `object_path` field
- the first converted session object is recorded as `root`
- returned domain objects are named from the parent path plus the creating
  command, e.g. `nifm:s.root.cmd5[0]`, then
  `nifm:s.root.cmd5[0].cmd4[0]`
- `ipc_decode_request` and `ipc_decode_response` now include:
  - `payload_size`
  - `payload_word0..3`
  - `payload_preview_hex`
- mirror trace detail code `13` is now labeled `passive_proxy_skipped`

Expected value of the next `nifm:s` run:

- the `qlaunch` monitor chain should no longer appear as a flat sequence on
  `nifm:s.root`
- command `17` / `18` replies can be compared by payload fields directly,
  instead of decoding them manually from the CMIF envelope
- command `15` / `36` replies should surface their small payloads and help
  distinguish status polling from configuration queries

## 2026-06-28: `nifm:s` client map and command graph

Latest passive traces show that the active public surface in these runs is
`nifm:s`, not `nifm:u`.

Observed service usage:

- `nifm:u`
  - installed successfully
  - no client sessions observed
  - no forwarded `ipc_request` traffic observed
- `nifm:s`
  - all observed clients connected here
  - long-lived polling and object creation both occur here

Observed client title IDs:

- `0x0100000000000023` = `am`
- `0x0100000000000025` = `nim`
- `0x010000000000003E` = `olsc`
- `0x0100000000001000` = `qlaunch` / `start`
- `0x010000000000100C` = `overlayDisp`

Observed object graph:

- `nifm:s.root`
  - cmd `5` returns child object `nifm:s.root.cmd5[0]`
- `nifm:s.root.cmd5[0]`
  - cmd `4` returns child object `nifm:s.root.cmd5[0].cmd4[0]`
  - cmd `17` and cmd `18` form the dominant polling loop for `qlaunch`
- `nifm:s.root.cmd5[0].cmd4[0]`
  - used heavily by `nim` and `olsc`
  - observed cmds: `0`, `1`, `2`, `4`, `6`, `12`

Observed per-client patterns:

- `am`
  - only observed creating the root child with cmd `5`
- `nim`
  - short setup sequence through root cmd `5`, child cmd `4`, then leaf cmds
    `2`, `6`, `4`, `0`, `1`
  - explicitly closes its leaf object in the clean trace
- `olsc`
  - uses the same root cmd `5` and child cmd `4` pattern as `nim`
  - leaf object traffic is denser than `nim`
- `qlaunch`
  - root cmd `5`, then child cmd `4`
  - repeatedly polls child cmds `17` and `18`
  - also touches cmds `5`, `15`, and `36`
- `overlayDisp`

## 2026-07-01: `bsd:s` boot sensitivity in `olsc`

Passive `bsd:s` MITM is still not transparent for every system client.

Latest boot traces show:

- `0x010000000000003E` = `olsc` reaches `bsd:s`
- the observed sequence is:
  - root `RegisterClient` (cmd `0`)
  - `StartMonitoring` (cmd `1`)
- both forwarded responses return success
- `olsc` still aborts immediately afterwards

Crash classification:

- the failure is a `User Break`, not a `Data Abort`
- crash report result: `0x81B` (`2027-0004`)
- this looks like an internal invariant/assert trip in `olsc`, not raw memory
  corruption in the client

Current working hypothesis:

- the highest-value suspect is copied-handle or session/domain identity
  semantics around `bsd:s` `RegisterClient`
- plain CMIF payload forwarding is less likely, because the visible request and
  response path completes successfully before `olsc` aborts

Operational decision for now:

- `olsc` is denylisted from passive `bsd:s` MITM together with `npns` and
  `eupld`
- this keeps boot stable while the tracer continues collecting from less
  sensitive `bsd:s` clients
- `net-probe` now also has build-time diagnostic overrides so those clients can
  be re-enabled one at a time without deleting the stable default denylist
  policy:
  - `make EXTRA_DEFINES='-DWGNX_DIAG_ALLOW_BSD_SYSTEM_OLSC=1'`
  - related toggles exist for `npns`, `eupld`, `qlaunch`, and the `ssl` family

This note is guidance from observed traces, not a proven root cause yet.
  - only observed creating the root child with cmd `5`

Observed non-zero forwarded results:

- `qlaunch` on `nifm:s.root.cmd5[0]`
  - cmd `18` -> `0x0000D46E`
- `nim` / `olsc` on `nifm:s.root.cmd5[0].cmd4[0]`
  - cmd `1` -> `0x0000DE6E`
  - cmd `1` -> `0x001F486E`
  - `olsc` also showed one `0x0000E06E`

Current interpretation:

- the passive MITM no longer appears to perturb the public `nifm:u` path
- the active compatibility surface for the current system state is `nifm:s`
- `nim` is not the only sensitive client; `qlaunch` is the more valuable
  lifecycle target because it keeps a long-lived polling session open
- future shutdown and transparency fixes should be validated primarily against
  the `qlaunch` `17` / `18` polling loop

## 2026-06-28: `nifm:s` `nim` sequence vs. SwitchBrew

This section uses SwitchBrew as guidance, not hard truth.

Reference page:

- https://switchbrew.org/wiki/Network_Interface_services

Why the caveat matters:

- the static service registration recovered from `nifm_main` currently shows
  `nifm:s` with `0x15`, while the public wiki documents max sessions `0x10`
- the wiki is still the best public map for naming the public IPC surface, but
  the binary and the live trace remain the authoritative sources for 20.5.0

That said, the observed `nim` sequence now correlates strongly with the public
`nifm:s` contracts documented there.

Observed `nim` runtime path:

- `nifm:s.root` cmd `5`
  - no meaningful payload beyond the expected 8-byte zero input
  - returns `nifm:s.root.cmd5[0]`
- `nifm:s.root.cmd5[0]` cmd `4` with payload `0x00000002`
  - returns `nifm:s.root.cmd5[0].cmd4[0]`
- `nifm:s.root.cmd5[0].cmd4[0]`
  - cmd `2` -> returns 2 copy handles
  - cmd `6` -> input `0x0000000b`
  - cmd `4` -> no payload, success
  - cmd `0` -> returns `0x00000002` first, later `0x00000003`
  - cmd `1` -> returns `0x0000DE6E`

SwitchBrew-guided interpretation:

- `nifm:s.root` cmd `5`
  - likely `CreateGeneralService`
  - this is already strongly supported by static reversing because the root
    handler consumes a PID-tagged 8-byte argument and returns the first child
    wrapper
- `nifm:s.root.cmd5[0]`
  - likely `IGeneralService`
- `nifm:s.root.cmd5[0]` cmd `4`
  - likely `CreateRequest`
- `nifm:s.root.cmd5[0].cmd4[0]`
  - very likely `IRequest`

The leaf object match is especially strong:

- `IRequest` cmd `2` is documented as `GetSystemEventReadableHandles`
  - the trace returns exactly 2 copy handles
- `IRequest` cmd `6` is documented as `SetRequirementPreset`
  - the trace sends a single `u32` preset value `0x0000000b`
- `IRequest` cmd `4` is documented as `Submit`
  - the trace sends no payload and gets a success result
- `IRequest` cmd `0` is documented as `GetRequestState`
  - the trace returns a small inline integer that changes from `2` to `3`
- `IRequest` cmd `1` is documented as `GetResult`
  - the trace returns a NIFM-style result code `0x0000DE6E`

Practical conclusion:

- for the current passive `nifm:s` work, the `nim` path is best modeled as
  `IStaticService -> IGeneralService -> IRequest`
- this is still a model, not a final name proof, but it is now consistent
  across:
  - live IPC shape
  - handle counts
  - payload widths
  - state transitions
  - the existing SwitchBrew public command map

## 2026-06-28: `nifm:u` root-child static correlation

Static reversing of `nifm_main` now lines up with the passive `sphaira` trace
well enough to pin down the first useful `nifm:u` object chain.

Recovered registration path:

- `FUN_710008f33c`
  - constructs and registers:
    - `nifm:a` with count `0x2`
    - `nifm:s` with count `0x15`
    - `nifm:u` with count `0x5`

Recovered `nifm:u` root dispatcher:

- `FUN_7100090698`
  - only accepts cmds `4` and `5`
  - cmd `4` -> `FUN_71000907d8`
  - cmd `5` -> `FUN_7100090980`
- `FUN_7100090980`
  - validates the incoming CMIF contract through `param_3`
  - requires at least 8 bytes of input payload
  - consumes exactly one `u64` argument from the request body
  - invokes the backing interface method at `*param_2 + 0x28`
  - allocates reply object space through the CMIF helper at `*param_3 + 0x28`
  - installs the returned child object under wrapper `PTR_PTR_710011d570`
  - on failure, uses the CMIF error writer at `*param_3 + 0x48`

Direct binary interpretation of `FUN_7100090980`:

- regardless of public naming, root cmd `5` is unquestionably a
  "take one `u64`, return one child object" command on 20.5.0
- the all-zero 8-byte payload observed in the live trace is therefore not
  incidental; it is part of the real command contract

SwitchBrew-guided naming:

- SwitchBrew documents `nifm:u` cmd `5` as `CreateGeneralService`
- that matches the recovered 20.5.0 shape well enough that using the name is
  now reasonable, as long as it is treated as a public-label hypothesis rather
  than a substitute for the binary

This matches the observed runtime path:

- `nifm:u.root`
  - real traffic uses cmd `5`
  - returns the first child object `nifm:u.root.cmd5[0]`

Important trace clarification:

- the apparent `nifm:u.root` cmd `0` in the passive trace is not a real public
  NIFM command
- it is the HIPC close packet (`raw_word0 = 0x00000002`) being carried through
  the generic request/response logger during object shutdown
- this removes the contradiction with the static root dispatcher, which only
  handles cmds `4` and `5`

Recovered first child dispatcher:

- `FUN_7100090c50`
  - broad switch over the child object command set
  - observed live `sphaira` traffic fits this dispatcher, not the other nearby
    NIFM dispatchers

Confirmed `sphaira`-relevant child commands:

- cmd `5`
  - handled by `FUN_7100090c50` case `5`
  - calls the backing interface method at `*param_2 + 0x38`
  - uses a `0x17c` local descriptor/scratch shape
  - returns data through the CMIF output-buffer path, not inline words
  - matches the documented `SfNetworkProfileData` size `0x17c`
  - SwitchBrew-guided name: `GetCurrentNetworkProfile`
- cmd `12`
  - handled by case `0xc`
  - calls `*param_2 + 0x70`
  - returns a 4-byte inline payload
  - observed payload: `c0 a8 40 a7` = `<REDACTED_IPV4_CURRENT>`
  - confirmed on-device to be the current Wi-Fi IPv4 address
  - SwitchBrew-guided name: `GetCurrentIpAddress`
- cmd `15`
  - handled by case `0xf`
  - calls `*param_2 + 0x88`
  - returns a 24-byte inline payload
  - observed payload is stable across polls:
    `01c0a840a7ffffff00c0a840bc01c0a840bc000000000000`
  - the static packing now matches `IpAddressSetting` (`0xd`) plus
    `DnsSetting` (`0x9`) from SwitchBrew, for `0x16` bytes total plus wire
    padding
  - decoded current sample:
    - `IpAddressSetting`
      - `is_auto = 1`
      - `ip = <REDACTED_IPV4_CURRENT>`
      - `mask = 255.255.255.0`
      - `gateway = <REDACTED_IPV4_GATEWAY_DNS>`
    - `DnsSetting`
      - `is_auto = 1`
      - `preferred_dns = <REDACTED_IPV4_GATEWAY_DNS>`
      - `alternate_dns = 0.0.0.0`
  - SwitchBrew-guided name: `GetCurrentIpConfigInfo`
- cmd `18`
  - handled by case `0x12`
  - calls `*param_2 + 0xa0`
  - writes a 3-byte status tuple into the reply body, padded to 4 bytes on the
    wire
  - observed payload: `01 03 04 00`
  - this value stayed stable across the `sphaira` polling loop in the healthy
    trace
  - the wire layout matches the documented
    `u8 NetworkInterfaceType, u8 wifiStrength, u8 connectionStatus`
  - the first two bytes line up cleanly with `Ieee80211` and strong Wi-Fi
  - the semantic meaning of status value `4` is still inferred, not proven
  - SwitchBrew-guided name: `GetInternetConnectionStatus`

Current interpretation:

- `sphaira`'s practical "internet available" path on `nifm:u` is centered on
  the child returned by root cmd `5`
- the first child returned by `FUN_7100090980` is now best modeled as
  `IGeneralService`
- cmd `12` is effectively confirmed as current IPv4 address
- cmd `15` is effectively confirmed as current IP config info, not an opaque
  route blob
- cmd `18` is effectively confirmed as a 3-field connection-status tuple
- cmd `5`, `12`, `15`, and `18` now form a coherent, SwitchBrew-aligned
  `IGeneralService` subset instead of four isolated observations

Next static target inside `nifm_main`:

- keep identifying the concrete vtable behind the child wrapper installed by
  `FUN_7100090980`
- correlate more `FUN_7100090c50` cases against the public `IGeneralService`
  list
- determine whether the `nifm:s` and `nifm:u` root cmd `5` paths land on the
  same concrete backend type or only on sibling interfaces with parallel
  command numbering

## 2026-06-28: shared root glue and `qlaunch` hot commands

Static reversing now tightens the relationship between `nifm:a`, `nifm:s`, and
`nifm:u`.

Recovered registration detail from `FUN_710008f33c`:

- all three services are registered through the same root CMIF wrapper
  `PTR_PTR_710011d558`
- the wrapper points at the same root dispatch glue table at `0x71001189d0`
- the difference between `a`, `s`, and `u` is therefore not the outer CMIF
  parser itself, but the backing object pointer passed to registration

Observed registration shape:

- `nifm:a`
  - name `"nifm:a"`
  - max sessions `0x2`
  - backing object slot `plVar7`
- `nifm:s`
  - name `"nifm:s"`
  - max sessions `0x15`
  - backing object slot `plVar6`
- `nifm:u`
  - name `"nifm:u"`
  - max sessions `0x5`
  - backing object slot `plVar5`

Practical interpretation:

- `FUN_7100090698` is now best treated as shared root glue for at least
  `nifm:s` and `nifm:u`
- it is reasonable that the same root cmd numbering is reused while the
  concrete backend implementation differs by service flavor

Additional root clarification:

- root cmd `4` (`FUN_71000907d8`) and root cmd `5` (`FUN_7100090980`) both
  return the same child wrapper `PTR_PTR_710011d570`
- that wrapper points at dispatch glue `FUN_7100090b48`, which forwards into
  the large child dispatcher `FUN_7100090c50`

That means the active `nifm:s.root.cmd5[0]` object seen in the passive traces
and the active `nifm:u.root.cmd5[0]` object seen from `sphaira` are not just
superficially similar. They share the same CMIF dispatch glue and differ at the
backend-object layer.

This makes the current model stronger:

- root object
  - shared CMIF glue
- first child returned by root cmd `5`
  - shared `IGeneralService`-like CMIF glue
- deeper objects
  - service-specific backend implementations behind the same glue pattern

### `qlaunch` polling commands on the shared child

Because `qlaunch` stays on `nifm:s.root.cmd5[0]`, the hot commands in its
polling loop are commands on `FUN_7100090c50`.

`cmd 17` / `0x11`

- static shape:
  - method offset `*param_2 + 0x98`
  - returns a single byte, padded into a normal CMIF reply
- best public label:
  - `IsWirelessCommunicationEnabled`
- confidence:
  - high

`cmd 18` / `0x12`

- static shape:
  - method offset `*param_2 + 0xa0`
  - returns a 3-byte tuple padded to 4 bytes on the wire
- previously decoded runtime sample:
  - `01 03 04`
- best public label:
  - `GetInternetConnectionStatus`
- confidence:
  - high

`cmd 36` / `0x24`

- static shape:
  - method offset `*param_2 + 0x130`
  - uses CMIF output-buffer plumbing
  - hardcodes local descriptor size `0x34`
- best public label:
  - `GetCurrentAccessPoint` (`4.0.0+`)
- confidence:
  - medium-high

This is important for the runtime traces:

- the `qlaunch` polling loop is not just generic "is internet up" traffic
- it appears to combine:
  - wireless enabled state
  - coarse connection status
  - current access-point metadata

That is a better explanation for why the Home UI can become "network confused"
while actual outbound traffic still works: the UI is likely sensitive to the
shape and freshness of this higher-level `nifm:s` polling surface.

### Leaf object returned by child cmd `4`

The shared child dispatcher also now tightens the `nim` path.

Recovered child-creation edge:

- `FUN_7100090c50` case `4`
  - consumes one `u32`
  - invokes backend method offset `*param_2 + 0x30`
  - returns wrapper `PTR_PTR_710011d5a0`
- `PTR_PTR_710011d5a0`
  - points at glue dispatcher `FUN_710009640c`
  - which immediately forwards into `FUN_7100096514`

`FUN_7100096514` static command surface now matches the earlier `nim` trace well
enough to reinforce the `IRequest` hypothesis:

- cmd `0`
  - no input
  - scalar inline reply via backend offset `+0x20`
- cmd `1`
  - no input
  - scalar inline reply via backend offset `+0x28`
- cmd `2`
  - no inline payload
  - backend offset `+0x30`
  - returns two handles through CMIF handle-copy plumbing
- cmd `4`
  - no input
  - backend offset `+0x40`
- cmd `6`
  - consumes one `u32`
  - backend offset `+0x50`
- cmd `12`
  - consumes one byte interpreted as boolean
  - backend offset `+0x78`

Practical conclusion:

- `nifm:s.root.cmd5[0]` and `nifm:u.root.cmd5[0]` are now best modeled as the
  same shared `IGeneralService`-style glue object
- `cmd 4` on that object returns a separate leaf object under
  `FUN_7100096514`, which remains the strongest `IRequest` candidate
- another child-creation edge also exists on cmd `2`, returning wrapper
  `PTR_PTR_710011d590`; that object is not yet named

## 2026-06-28: shared child cmd `2` is most likely `CreateScanRequest`

The remaining unnamed child returned by the shared `IGeneralService`-style
dispatcher is now narrow enough to label provisionally.

Static shape from `FUN_7100090c50` case `2`:

- no inline input
- backend call through offset `*param_2 + 0x28`
- returns a child object by installing wrapper `PTR_PTR_710011d590`

Wrapper layout:

- `PTR_PTR_710011d590`
  - first slot `0x71001189e0`
  - second slot `0x71000e54aa`
- this matches the same wrapper pattern used by the already-correlated
  `IRequest` child:
  - `PTR_PTR_710011d5a0`
    - first slot `0x71001189e8`
    - second slot `0x71000e581e`

Important correction:

- the second slot here is not directly executable code in the current Ghidra
  project
- it resolves to a descriptor-like data region, not a discovered function
- that means this child is being described through the same generic CMIF object
  machinery as the `IRequest` leaf, not through a one-off hand-written wrapper

Public guidance from SwitchBrew's `Network_Interface_services` page now fits
this static edge very closely:

- on `IGeneralService`
  - cmd `2` is documented as `CreateScanRequest`
  - cmd `4` is documented as `CreateRequest`
- `CreateScanRequest`
  - takes no input
  - returns `IScanRequest`
- `CreateRequest`
  - takes an input `s32 RequirementPreset`
  - returns `IRequest`

This is an unusually strong match for the two child-creation cases already
recovered:

- shared child cmd `2`
  - no input
  - child return
  - best label: `CreateScanRequest`
- shared child cmd `4`
  - one `u32` input
  - child return
  - best label: `CreateRequest`

Current best model:

- `nifm:{s,u}.root.cmd5[0].cmd2`
  - `CreateScanRequest`
  - returns `IScanRequest`
- `nifm:{s,u}.root.cmd5[0].cmd4`
  - `CreateRequest`
  - returns `IRequest`

Confidence:

- medium-high
- this still depends partly on public service documentation, not only on
  private implementation detail recovered from the binary

Most useful next confirmation target:

- identify the command surface behind `PTR_PTR_710011d590`
- if it reduces to the public `IScanRequest` set
  - `Submit`
  - `IsProcessing`
  - `GetResult`
  - `GetSystemEventReadableHandle`
  - optional `SetChannels`
  then the label can be treated as effectively confirmed

## 2026-06-28: root cmd `4` and the `IScanRequest` leaf now line up cleanly

Two more static edges now fit the public `nifm` model tightly enough to use as
working labels.

### Root cmd `4` is the older no-input general-service constructor

`FUN_71000907d8` is the sibling to the already-recovered root cmd `5` path.

Recovered shape:

- validates the request through `PTR_DAT_710011d568`
- no inline input payload
- invokes backend method offset `*param_2 + 0x20`
- receives a returned backend object through an out-parameter
- allocates CMIF child-object reply storage through `*param_3 + 0x28`
- installs wrapper `PTR_PTR_710011d570`
- on failure, uses the normal CMIF error writer at `*param_3 + 0x48`

This is materially different from root cmd `5`:

- `FUN_7100090980`
  - consumes one inline `u64`
  - invokes backend offset `*param_2 + 0x28`
  - returns the same child wrapper

SwitchBrew's public `IStaticService` table matches this split closely:

- root cmd `4`
  - `CreateGeneralServiceOld`
  - no input
  - returns `IGeneralService`
- root cmd `5`
  - `[3.0.0+] CreateGeneralService`
  - takes PID plus inline `u64 reserved_pid`
  - returns `IGeneralService`

Current best model:

- `nifm:{s,u}.root.cmd4`
  - `CreateGeneralServiceOld`
- `nifm:{s,u}.root.cmd5`
  - `CreateGeneralService`

Confidence:

- high for the no-input vs one-`u64` split
- medium-high for the exact public names

### The unnamed child returned by shared child cmd `2` now reduces to `IScanRequest`

The dispatch pointer at `0x71001189e0` lands at code entry `0x7100095d10`,
which validates CMIF framing and forwards into:

- `FUN_7100095e18(...)`

That function's switch surface is now narrow enough to map directly.

Recovered command surface:

- cmd `0`
  - no input
  - backend method offset `*param_2 + 0x20`
  - no inline output on success
  - best label: `Submit`
- cmd `1`
  - no input
  - backend method offset `*param_2 + 0x28`
  - returns one byte inline
  - best label: `IsProcessing`
- cmd `2`
  - no input
  - backend method offset `*param_2 + 0x30`
  - no inline output on success
  - best label: `GetResult`
- cmd `3`
  - no input
  - backend method offset `*param_2 + 0x38`
  - returns one copied handle through the CMIF handle path
  - best label: `GetSystemEventReadableHandle`
- cmd `4`
  - present on 20.5.0
  - consumes an input buffer and passes `(ptr, size >> 1)` to backend offset
    `*param_2 + 0x40`
  - best label: `SetChannels`

Important note on cmd `4`:

- the `size >> 1` conversion strongly suggests the backend expects a count of
  16-bit channel entries rather than a raw byte count
- that matches the public idea of "set the channels to scan", even though the
  exact element type is still inferred from the binary shape

Practical conclusion:

- `nifm:{s,u}.root.cmd5[0].cmd2`
  - child constructor
  - best label: `CreateScanRequest`
- returned child wrapper `PTR_PTR_710011d590`
  - concrete leaf dispatcher `FUN_7100095e18`
  - best label: `IScanRequest`

This is now one of the cleaner parts of the `nifm` reverse:

- root cmd `4/5`
  - `IStaticService`-style general-service constructors
- shared child cmd `2`
  - `CreateScanRequest`
- resulting leaf
  - `IScanRequest`

## Recommended next autoboot probe runs

If time permits, collect two runs instead of one. They answer different
questions and are easier to compare when kept separate.

Run A: passive boot and settings path

1. reboot with probe in autoboot
2. wait on Home for 20-30 seconds
3. open Internet Settings / connection test
4. wait another 10-20 seconds
5. stop the probe cleanly

Goal:

- capture the steady-state `qlaunch` polling baseline
- see whether the settings path introduces new `nifm:s` clients or command
  sequences

Run B: passive boot and homebrew path

1. reboot with probe in autoboot
2. wait on Home for 20-30 seconds
3. launch `sphaira`
4. wait 10-20 seconds inside it
5. exit back to Home
6. wait another 10-20 seconds
7. stop the probe cleanly

Goal:

- capture the delta between Home-only traffic and a live homebrew consumer
- verify whether `sphaira` causes additional `nifm:s` sessions beyond the
  already-known system clients

If only one run is practical, prefer Run A first. It gives the cleanest
baseline for the `qlaunch` polling path.

## 2026-06-28: broad `IGeneralService` surface now lines up through 20.5.0

The large shared child dispatcher `FUN_7100090c50` can now be read as a mostly
coherent `IGeneralService` implementation rather than a handful of isolated
matches.

Public reference points used here:

- SwitchBrew `Network_Interface_services`
  - https://switchbrew.org/wiki/Network_Interface_services
- local `libnx` public header
  - `/opt/devkitpro/libnx/include/switch/services/nifm.h`

The caution from earlier still applies:

- the binary remains authoritative for 20.5.0
- SwitchBrew and `libnx` are being used as naming guidance where the signature
  overlap is high

### High-confidence `IGeneralService` command map

The following cases now line up cleanly by command ID, input shape, output
shape, and where available live trace behavior.

- cmd `1`
  - `GetClientId`
  - no input
  - one 4-byte scalar reply
- cmd `2`
  - `CreateScanRequest`
  - no input
  - returns child wrapper `PTR_PTR_710011d590`
- cmd `4`
  - `CreateRequest`
  - input `s32`
  - returns child wrapper `PTR_PTR_710011d5a0`
- cmd `5`
  - `GetCurrentNetworkProfile`
  - type-`0x1A` style output buffer
  - fixed local descriptor size `0x17c`
- cmd `6`
  - `EnumerateNetworkInterfaces`
  - input `u32`
  - output buffer element stride `0x8`
  - trailing inline `s32` count
- cmd `7`
  - `EnumerateNetworkProfiles`
  - input `u8`
  - output buffer element stride `0x75`
  - trailing inline `s32` count
- cmd `8`
  - `GetNetworkProfile`
  - input `Uuid` (`0x10` bytes)
  - output buffer descriptor size `0x17c`
- cmd `9`
  - `SetNetworkProfile`
  - type-`0x19` style input buffer
  - returns one `Uuid` (`0x10` bytes) inline
- cmd `10`
  - `RemoveNetworkProfile`
  - input `Uuid` (`0x10` bytes)
  - no success payload
- cmd `11`
  - `GetScanDataOld`
  - output buffer element stride `0x34`
  - trailing inline `s32` count
- cmd `12`
  - `GetCurrentIpAddress`
  - no input
  - one 4-byte IPv4 reply
- cmd `13`
  - `GetCurrentAccessPointOld`
  - type-`0x1A` style output buffer
  - fixed local descriptor size `0x34`
- cmd `15`
  - `GetCurrentIpConfigInfo`
  - no input
  - 24-byte inline reply
- cmd `16`
  - `SetWirelessCommunicationEnabled`
  - input `bool`
  - no success payload
- cmd `17`
  - `IsWirelessCommunicationEnabled`
  - no input
  - one-byte reply
- cmd `18`
  - `GetInternetConnectionStatus`
  - no input
  - 3-byte tuple padded in the CMIF reply
- cmd `31`
  - `GetTelemetorySystemEventReadableHandle`
  - no input
  - returns one copied handle
- cmd `32`
  - `GetTelemetryInfo`
  - type-`0x1A` style output buffer
  - fixed local descriptor size `0x620`
- cmd `33`
  - `ConfirmSystemAvailability`
  - no input
  - no success payload
- cmd `34`
  - `SetBackgroundRequestEnabled`
  - input `bool`
  - no success payload
- cmd `35`
  - `GetScanData`
  - output buffer element stride `0x34`
  - trailing inline `s32` count
- cmd `36`
  - `GetCurrentAccessPoint`
  - type-`0x1A` style output buffer
  - fixed local descriptor size `0x34`
- cmd `37`
  - `Shutdown`
  - no input
  - no success payload
- cmd `38`
  - `GetAllowedChannels`
  - output buffer element stride `0x2`
  - trailing inline `s32` count
- cmd `39`
  - `NotifyApplicationSuspended`
  - input `u64`
  - no success payload
- cmd `40`
  - `SetAcceptableNetworkTypeFlag`
  - input `u32`
  - no success payload
- cmd `41`
  - `GetAcceptableNetworkTypeFlag`
  - no input
  - one 4-byte scalar reply
- cmd `42`
  - `NotifyConnectionStateChanged`
  - no input
  - no success payload
- cmd `43`
  - `SetWowlDelayedWakeTime`
  - input `s32`
  - no success payload

### 20.x tail: network-emulation and rewrite commands are present

The same dispatcher continues through cmd `57`, which matters because it shows
that the 20.5.0 `nifm` surface already includes the newer debug/emulation
controls documented publicly for 18.x to 20.x.

High-confidence signature matches:

- cmd `44`
  - `IsWiredConnectionAvailable`
  - one-byte reply
- cmd `45`
  - `IsNetworkEmulationFeatureEnabled`
  - one-byte reply
- cmd `46`
  - `SelectActiveNetworkEmulationProfileIdForDebug`
  - input `u32`
  - no success payload
- cmd `47`
  - `GetScanData`
  - input `u32`
  - output buffer element stride `0x34`
  - trailing inline `s32` count
- cmd `48`
  - `ResetActiveNetworkEmulationProfileId`
  - no input
  - no success payload
- cmd `49`
  - `GetActiveNetworkEmulationProfileId`
  - no input
  - one 4-byte scalar reply
- cmd `50`
  - `IsRewriteFeatureEnabled`
  - no input
  - one-byte reply
- cmd `51`
  - `CreateRewriteRule`
  - input `u8`
  - output `u64`
- cmd `52`
  - `DestroyRewriteRule`
  - input `u64`
  - no success payload
- cmd `53`
  - `IsActiveNetworkEmulationProfileIdSelected`
  - no input
  - one-byte reply
- cmd `54`
  - `SelectDefaultNetworkEmulationProfileId`
  - input `u32`
  - no success payload
- cmd `55`
  - `GetDefaultNetworkEmulationProfileId`
  - no input
  - one 4-byte scalar reply
- cmd `56`
  - `GetNetworkEmulationProfile`
  - input `u32`
  - output buffer descriptor size `0x1438`
- cmd `57`
  - `SetWowlTcpKeepAliveTimeout`
  - input `u32`
  - no success payload

Practical conclusion:

- 20.5.0 does not just implement the older connectivity subset that most
  homebrew wrappers expose
- the private dispatcher already carries the later emulation/rewrite surface
- that is still policy/state machinery, not packet-routing machinery

## 2026-06-28: cmd `14` is a real child-object edge, best modeled as `INetworkProfile`

The earlier uncertainty around shared child cmd `14` is now resolved enough to
name it provisionally.

Recovered creation path from `FUN_7100090c50` case `0xe`:

- type-`0x1A` style buffer descriptor setup with local size `0x17c`
- backend call through `*param_2 + 0x80`
- returns:
  - inline `u64`-sized state/value
  - child object wrapper rooted at `PTR_FUN_71001189f0`

Wrapper and leaf:

- wrapper data
  - `0x710011d5f8`
  - first slot `0x71001189f0`
  - second slot `0x71000e59d8`
- dispatch glue
  - `0x71001189f0 -> 0x71000974fc`
- concrete leaf
  - `FUN_7100097604`

`FUN_7100097604` only accepts cmd IDs `0`, `1`, and `2`.

That matches the public `INetworkProfile` surface unusually well:

- SwitchBrew documents `INetworkProfile` with exactly three commands
  - `0 Update`
  - `1 PersistOld` / `[3.0.0+] Persist`
  - `2 [3.0.0+] Persist`

Current best model:

- `nifm:{s,u}.root.cmd5[0].cmd14`
  - `CreateTemporaryNetworkProfile`
- returned child
  - best label: `INetworkProfile`

Confidence:

- medium-high on the object identity
- lower on exact per-command naming inside the child, because this pass only
  established the 3-command leaf shape cleanly

## 2026-06-28: `IRequest` late commands now line up with the public API too

The `IRequest` leaf `FUN_7100096514` now reduces cleanly against both
SwitchBrew and the installed `libnx` header.

Previously confirmed:

- cmd `0`
  - `GetRequestState`
- cmd `1`
  - `GetResult`
- cmd `2`
  - `GetSystemEventReadableHandles`
- cmd `4`
  - `Submit`
- cmd `6`
  - `SetRequirementPreset`

Additional high-confidence matches:

- cmd `5`
  - `SetRequirement`
  - input size `0x24`
- cmd `8`
  - `SetPriority`
  - input `u8`
- cmd `9`
  - `SetNetworkProfileId`
  - input `Uuid` (`0x10` bytes)
- cmd `10`
  - `SetRejectable`
  - input `bool`
- cmd `11`
  - `SetConnectionConfirmationOption`
  - input `u8`
- cmd `12`
  - `SetPersistent`
  - input `bool`
- cmd `13`
  - `SetInstant`
  - input `bool`
- cmd `14`
  - `SetSustainable`
  - input `bool, u8`
- cmd `15`
  - `SetRawPriority`
  - input `u8`
- cmd `16`
  - `SetGreedy`
  - input `bool`
- cmd `17`
  - `SetSharable`
  - input `bool`
- cmd `18`
  - `SetRequirementByRevision`
  - input `u32`
- cmd `19`
  - `GetRequirement`
  - 0x34-byte reply shape on 20.5.0
- cmd `20`
  - `GetRevision`
  - one 4-byte scalar reply
- cmd `21`
  - `GetAppletInfo`
  - input `u32`
  - output buffer
  - trailing inline `AppletId`, mode, and size fields
- cmd `22`
  - `GetAdditionalInfo`
  - one 4-byte scalar reply
- cmd `23`
  - `[3.0.0+] SetKeptInSleep`
  - input `bool`
- cmd `24`
  - `[3.0.0+] RegisterSocketDescriptor`
  - input `s32`
- cmd `25`
  - `[3.0.0+] UnregisterSocketDescriptor`
  - input `s32`

Why cmds `24` and `25` matter for the VPN project:

- this is the concrete user-visible edge where `nifm` tracks a socket against
  request lifecycle
- SwitchBrew notes that registered request sockets later feed
  `wlan:inf` sleep-entry handling
- that is one of the few public places where `nifm` and WLAN state management
  meet a specific socket object rather than only abstract connectivity policy

Practical conclusion:

- `nifm` already has a formal notion of socket lifecycle attachment
- that is still not routing control, but it is highly relevant to making any
  tunneled transport survive sleep, interface loss, and foreground/background
  transitions

## 2026-06-29: tracer refined for the next passive `nifm` run

The current `net-probe` MITM build was updated in a tracer-only pass.

Behavioral intent:

- keep request forwarding unchanged
- avoid reopening the earlier shutdown / transparency regressions
- make the next `nifm` trace materially easier to interpret without offline
  manual decoding

New trace behavior:

- close packets are now classified explicitly as:
  - `root_close`
  - `domain_close`
- `ipc_decode_request` now includes:
  - `hipc_command_type_name`
  - `root_close`
  - `domain_close`
- a new `nifm_semantic` event is emitted for the currently understood hot
  paths:
  - `IStaticService`
    - `CreateGeneralServiceOld`
    - `CreateGeneralService`
  - `IGeneralService`
    - `CreateRequest`
    - `GetCurrentIpAddress`
    - `GetCurrentIpConfigInfo`
    - `IsWirelessCommunicationEnabled`
    - `GetInternetConnectionStatus`
  - `IRequest`
    - `GetRequestState`
    - `GetResult`
    - `GetSystemEventReadableHandles`
    - `SetRequirementPreset`
    - `SetPersistent`

The semantic record does not replace the raw CMIF logs. It only adds summaries
for fields we already have high confidence in, for example:

- `reserved_pid`
- `requirement_preset`
- decoded IPv4
- decoded IP config tuple
- wireless-enabled boolean
- internet-status tuple
- request state
- request result
- copied-handle count on `GetSystemEventReadableHandles`

Expected value of the next run:

- earlier apparent root cmd `0` noise should now collapse into explicit close
  traffic instead of looking like a public API contradiction
- `qlaunch` and `nim` lifecycles should be readable directly from the JSONL
  without separate hand-decoding of payload bytes
- the next comparison between `nifm:s` and `nifm:u` should be about semantic
  behavior, not CMIF framing ambiguity

## 2026-06-29: current manual-launch probe result

The latest manual probe run covered:

- launch probe from Home
- open `sphaira`
- return to Home
- run connection test
- return to Home
- stop probe

Observed trace shape:

- only one real `nifm` client was captured
- all semantic traffic hit `nifm:u`
- no client traffic hit `nifm:s`
- the entire `nifm:u` burst completed roughly 100 ms after MITM start, then
  the trace stayed quiet until shutdown

Captured `nifm:u` traffic was limited to the already understood
`IGeneralService` polling loop:

- `CreateGeneralService`
- `GetCurrentNetworkProfile`
- `GetCurrentIpConfigInfo`
- repeated `GetInternetConnectionStatus`
- repeated `GetCurrentIpAddress`

Practical interpretation:

- the tracer is now passive and stable enough to capture the startup poll loop
- the Home-launched probe is still late for long-lived clients that opened
  handles before MITM registration
- for `nifm:s` and deeper `nim` lifecycle work, autoboot remains the higher
  value capture mode

## 2026-07-01: `bsd:a` connection-test sensitivity in `qlaunch`

An autoboot run that stayed on Home, entered Internet Settings, executed the
connection test, and attempted to return Home crashed `qlaunch`
(`0x0100000000001000`).

Observed `bsd:a` sequence for `qlaunch`:

- two `bsd:a` sessions were accepted
- observed root commands:
  - cmd `0` -> `RegisterClient`
  - cmd `1` -> `StartMonitoring`
- both forwarded replies returned success
- the client disconnected immediately afterwards
- `qlaunch` then aborted with `User Break`

Crash classification:

- title: `qlaunch` (`0x0100000000001000`)
- result: `0xD401` (`2001-0106`)
- exception type: `User Break`

Operational conclusion:

- passive `bsd:a` MITM is not yet transparent for `qlaunch`
- this is structurally similar to the earlier `olsc` / `bsd:s` failure:
  `RegisterClient` / monitoring setup appears sensitive to session, handle, or
  domain identity semantics even when the visible CMIF payloads round-trip
  successfully

Immediate mitigation:

- `qlaunch` is now denylisted from `bsd:a` MITM
- keep `bsd:a` tracing enabled for non-`qlaunch` clients while the
  `RegisterClient` path is analyzed further

This note is guidance from runtime traces, not a proven root cause.

## 2026-07-03: `bsd:s` autoboot trace and tracer refinement

An autobooted run that waited on Home, launched `sphaira`, started and aborted
an update, then rebooted cleanly produced the first stable `bsd:s` trace that
is useful for socket-sequence analysis.

Observed `bsd:s` client map in that run:

- the only active `bsd:s` client was `0x05446530ACA7E000`
- one session carried the real socket work
- a second short session only issued `StartMonitoring`
- no useful `bsd:u` traffic appeared
- `bsd:a` remained disabled for this run

Observed socket sequence:

- `RegisterClient`
- `StartMonitoring`
- listener setup on fd `0`
  - `Socket(AF_INET, SOCK_STREAM, 0)`
  - `Fcntl(F_SETFL, 0x800)`
  - `SetSockOpt(SOL_SOCKET, SO_REUSEADDR)`
  - `SetSockOpt(IPPROTO_TCP, TCP_NODELAY)`
  - `SetSockOpt(SOL_SOCKET, SO_KEEPALIVE)`
  - `Bind(0.0.0.0:5001)`
  - `Listen(5)`
- outbound connection pattern on later fds
  - `Socket(AF_INET, SOCK_STREAM, TCP)`
  - `Fcntl(F_GETFL)` then `Fcntl(F_SETFL, 0x800)`
  - `Connect(...) -> ret=-1 errno=115`
  - `GetSockOpt(SOL_SOCKET, SO_ERROR)`
  - `GetPeerName`
  - `GetSockName`
- later UDP-style helper socket:
  - `Socket(AF_INET, SOCK_DGRAM, 0)`
- later bind attempts on `0.0.0.0:28280`
- clean shutdown path:
  - `Shutdown(SHUT_RDWR)`
  - `Close`

Concrete remote endpoints recovered from the trace:

- `140.82.121.6:443`
- `138.199.37.232:443`

Concrete local state recovered from the trace:

- local IPv4 `<REDACTED_IPV4_LAB_SAMPLE>`
- ephemeral local ports including `61685`, `44968`, `40811`, `32532`, `32911`

Practical interpretation:

- the current passive MITM is transparent enough for at least one real
  homebrew `bsd:s` client under autoboot
- the trace already shows the expected nonblocking connect lifecycle around
  `Connect` plus `GetSockOpt(SO_ERROR)`
- the captured run still did not expose payload-bearing `Send` / `Recv`
  operations, so either:
  - this scenario did not drive the data path we expected
  - the actual transfer path uses a different service/client mix
  - or HTTPS payload I/O moved behind `ssl` after a connected socket
    descriptor handoff
  - or the interesting payload path is still outside the captured window

Tracer refinement made after this run:

- root `ipc_request` / `ipc_response` records now also carry a decoded
  `command_name`
- `ipc_decode_request` / `ipc_decode_response` now also carry:
  - `object_kind`
  - `command_name`
- `bsd_semantic` socket summaries were tightened to decode:
  - BSD-style `sockaddr` layouts using `sa_len` + `sa_family`
  - socket family / type / protocol names
  - `SOL_SOCKET` / TCP option names
  - `F_GETFL` / `F_SETFL`
  - `SHUT_RD` / `SHUT_WR` / `SHUT_RDWR`
  - `GetSockOpt` output values when the output buffer is small and scalar

Expected value of the next run:

- binds and connects should no longer show impossible raw families such as
  `512` or `518`
- `GetSockOpt(SO_ERROR)` should surface the returned scalar directly in the
  semantic record
- manual offline correlation against the `.bin` stream should be needed less
  often for routine `bsd:s` socket-state analysis

## 2026-07-06: `bsd:s` lifecycle instrumentation for repeated-client freezes

Latest `bsd:s`-only traces point away from a simple socket-command forwarding
bug and toward a session/domain lifecycle issue.

Observed shape from the failing `sphaira` sequence:

- first `sphaira` launch reaches `bsd:s`, opens two MITM sessions, performs
  normal `RegisterClient` / `StartMonitoring` / socket activity, and exits
- the later freeze happens before the second launch reaches `bsd:s` at all
- standalone `requester` traffic can still succeed in runs where the repeated
  `sphaira` launch later freezes

Working hypothesis:

- the first `bsd:s` client leaves behind poisoned lifecycle state
- the highest-value suspects remain:
  - cloned forward-handle ownership
  - domain-object teardown
  - monitoring child-object teardown after `StartMonitoring`

Tracer refinement added for this phase:

- per-domain-object state records:
  - creation path
  - parent object ID
  - creator command ID
  - creation timestamp
  - whether a `DispatchClose` was observed
- per-session domain snapshots at disconnect time
- passive-service destructor logging of the forward `Service` state
- a low-rate watchdog that emits session and domain snapshots every two seconds
  while MITM sessions are still active

Practical goal of the next runs:

- verify whether the first `sphaira` launch leaves any tracked `bsd:s` domain
  objects alive at disconnect
- compare the clean `requester` path against the repeated-client freeze path
- determine whether the sensitive state is rooted in the short
  `StartMonitoring` child session or in the main `RegisterClient` session

## 2026-07-07: `CloneCurrentObject` overtakes `StartMonitoring` as primary `bsd:s` suspect

The next static + runtime pass tightened the `bsd:s` picture enough to narrow
the repeated-client freeze further.

Runtime shape from the failing `sphaira` sequence:

- `bsd:s` accepts two MITM sessions from `sphaira`
- session `1` performs:
  - `RegisterClient`
  - repeated `CloneCurrentObject`
  - normal socket-facing activity (`Socket`, `Fcntl`, `SetSockOpt`, `Bind`,
    `Listen`, later another `Socket`)
- session `2` performs only:
  - `StartMonitoring`
- the two observed `CloneCurrentObject` calls on session `1` each return one
  move handle
- later freezes still tend to happen before a second launch reaches fresh
  `bsd:s` traffic at all

Static reversing in `bsdsockets_main` now lines up with that shape:

- `FUN_71000e41d0` is the `bsd:s` `StartMonitoring` wrapper
  - it sends plain root cmd `1`
  - it does not set up any returned child object or duplicate handle path
- `FUN_71000e43f4` and `FUN_71000e487c` are representative `bsd:s` wrapper
  methods that:
  - lazily obtain a duplicate handle through `FUN_71000d7900`
  - cache it in the wrapper object
  - then issue the real socket-facing command over that duplicate
- `FUN_71000d7900` is only a thin wrapper over `FUN_71000d8480`
- `FUN_71000d79a0` is the sibling helper that calls `FUN_71000d7d00`
- `FUN_71000d7d00` updates a caller-owned duplicate-handle slot
- `FUN_71000d8050` explicitly replaces previous stored handles and closes old
  ones via `FUN_71000c7c50`

That shifts the working hypothesis:

- `StartMonitoring` is still relevant as a secondary lifecycle signal, but it
  is no longer the best explanation for the freeze by itself
- the stronger suspect is now cloned forward-handle lifecycle on the main
  `bsd:s` session
  - duplicate-handle creation
  - duplicate-handle replacement
  - duplicate-handle teardown on wrapper destruction / client exit

Practical implication for the tracer:

- generic domain snapshots are no longer enough on their own
- the next useful instrumentation should track, per `bsd:s` session:
  - every successful `CloneCurrentObject` / `CloneCurrentObjectEx`
  - the returned move handle
  - the outstanding duplicate count at disconnect time
  - whether later control-path events still target a session that should have
    been fully torn down

## 2026-07-07: new requester/requester vs sphaira/sphaira evidence

Fresh runtime comparison tightened the clone hypothesis further.

`requester -> requester`:

- both requester runs completed cleanly
- under the `bsd:s`-only probe shape used for this run, the `bsd:s` trace never
  progressed into the clone-heavy session pattern seen with `sphaira`
- this means requester is currently not a reliable reproducer for the
  `bsd:s` repeated-launch wedge

`sphaira -> sphaira`:

- the first `sphaira` launch again accepted two `bsd:s` MITM sessions
- the main logical session (`session_id = 1`) issued two successful
  `CloneCurrentObject` calls
- at disconnect time, the tracer still saw two outstanding clone records on
  that logical session
- the same logical session later emitted three close callbacks with different
  server-side session handles
  - this matches the expected shape of one original accepted MITM session plus
    two clone-created MITM sessions
- the second `sphaira` launch froze before any fresh `AcceptMitmConnection`
  record appeared in the probe log

That runtime shape aligns with the passive MITM implementation in the vendored
Atmosphere-libs:

- `CloneCurrentObjectImpl()` in
  `sf_hipc_server_domain_session_manager.cpp` creates a fresh kernel session
  pair with `hipc::CreateSession()`
- for MITM sessions, it clones the upstream forward `Service` with
  `serviceClone()`
- it then registers the clone through `RegisterMitmSession(...)`
- importantly, the cloned session is registered with the same logical
  `m_mitm_session_id` used for monitor callbacks

Implication:

- the existing trace compression by logical `session_id` hides clone-created
  MITM sessions as siblings of the original root session
- the next targeted tracer needs to emit clone-registration events with the
  newly created server session handle and cloned forward handle so those later
  close callbacks can be matched to concrete clone registrations

## 2026-07-07: updated `sphaira -> exit -> sphaira` teardown evidence

Fresh run:

- `workspace/reports/nxrv/probe/probe-mitm-bsd.jsonl`
- `workspace/reports/nxrv/probe/net-probe.log`

Observed:

- only the first `sphaira` launch reaches `bsd:s`
- the run accepts exactly two root MITM sessions:
  - logical session `1`: main `bsd:s` root traffic
  - logical session `2`: short `StartMonitoring` side session
- logical session `1` issues two successful `CloneCurrentObject` calls
- later close order is:
  - clone session handle `688238`
  - clone session handle `786541`
  - root/monitor session handle `622705` for logical session `2`
  - root/main session handle `524403` for logical session `1`
- `PassiveMitmService` destructor logs appear for logical session `2` and then
  `1`
- no second-launch `ShouldMitm(bsd:s)` / `AcceptMitmConnection(bsd:s)` appears
  after the first launch has torn down

Current interpretation:

- with present visibility, the first `sphaira` launch does complete its MITM-side
  teardown
- the freeze therefore moved from “missing disconnect callback” to “post-teardown
  state still prevents a new `bsd:s` client from being created or acknowledged”
- the next tracer improvement is to log root accepted session handles explicitly,
  alongside clone-created session handles, so later disconnect/destructor events can
  be mapped without inferring which handle belonged to the original accepted MITM
  sessions

## 2026-07-09: source-level `sphaira` + libnx correlation for `bsd:s`

To reduce guesswork around the repeated-launch `bsd:s` wedge, the current
upstream `sphaira` source and upstream libnx socket runtime were reviewed as
guidance. This is not yet proof that the on-device `sphaira` binary matches the
same revision exactly, but the recovered startup shape aligns closely with the
live traces.

Source-level chain:

- `sphaira/source/main.cpp`
  - `userAppInit()` calls:
    - `socketInitialize(&socket_config)`
    - `nifmInitialize(NifmServiceType_User)`
  - both applet and application configs set:
    - `num_bsd_sessions = 3`
    - `bsd_service_type = BsdServiceType_Auto`
- `sphaira/source/app.cpp`
  - `App` initialization calls `curl::Init()`
- `sphaira/source/download.cpp`
  - `curl::Init()`:
    - runs `curl_global_init()`
    - creates one queue thread
    - creates `MAX_THREADS = 4` worker threads
    - creates one shared curl object with shared DNS / SSL-session /
      connection state
- `sphaira/source/ui/menus/main_menu.cpp`
  - `MainMenu::MainMenu()` immediately starts an async HTTPS request to GitHub
    releases metadata
- `sphaira/source/ui/menus/menu_base.cpp`
  - the UI polls `nifmGetInternetConnectionStatus()` and
    `nifmGetCurrentIpAddress()` about once per second for the status banner

libnx-side meaning of that startup:

- `nx/source/runtime/devices/socket.c`
  - `socketInitialize()` forwards `num_bsd_sessions` into `bsdInitialize()`
- `nx/source/services/bsd.c`
  - `_bsdInitialize()`:
    - opens root `bsd:*`
    - opens a second monitor `bsd:*`
    - issues `RegisterClient` on the root session
    - issues `StartMonitoring` on the monitor session
    - then calls `sessionmgrCreate(...)`
- `nx/source/sf/sessionmgr.c`
  - `sessionmgrCreate()` stores the root session in slot `0`
  - for every remaining slot, it issues `cmifCloneCurrentObject(root_session, ...)`

That directly explains the main runtime shape we keep seeing from `sphaira`:

- one main root `bsd:s` session
- one short `StartMonitoring` root `bsd:s` session
- two `CloneCurrentObject` calls on the main root session

For `num_bsd_sessions = 3`, those two clone calls are expected libnx behavior,
not an application-specific anomaly.

Updated interpretation:

- the current `sphaira` reproducer is valuable because it exercises the normal
  libnx BSD session pool shape plus immediate async network work
- the repeated-launch failure is now more likely inside passive MITM lifecycle
  compatibility with libnx session pooling than in Sphaira-specific socket code
- the next targeted tracer still needs to focus on:
  - accepted root-session ownership
  - clone-session registration / close ordering
  - whether post-teardown state blocks a later client before a fresh
    `AcceptMitmConnection(bsd:s)` occurs
