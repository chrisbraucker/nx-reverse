# Reverse Roadmap

This roadmap tracks the current plan for deep Horizon networking integration on
firmware `20.5.0`. The goal is to keep runtime probing, static reversing, and
implementation work aligned so we can decide with evidence whether a native-ish
virtual uplink is feasible, or whether `bsd:*` MITM remains the practical path.

## Goal

Determine the lowest practical interception layer for WireGuard-backed traffic
steering on Horizon OS, with preference for:

1. a native or near-native interface/uplink path
2. a lower-level service boundary below `bsd:*`
3. `bsd:*` MITM only if the native path does not materialize

## Current State

- `bsdsockets_main`, `nifm_main`, `wlan_main`, `eth_main`, `usb_main`, and
  `pkg2/kernel.bin` are available locally.
- `bsd:nu` has been tied to the user-side `anif` object model.
- `wlan:nd` and `eth:nd` probing exists in `net-probe`.
- passive MITM for `nifm:u`, `nifm:s`, and `wlan:nd` now compiles in the main
  `sysmodule`; first on-device traces are still pending.
- `wlan:nd` connected-state probing has already shown that unsafe calls can
  crash the server process, so future runtime work must stay narrow and staged.
- `pkg2` is available, so kernel and core-KIP reversing can begin without
  additional firmware extraction.

Constraint:

- the current passive MITM implementation depends on temporary local changes in
  the vendored `Atmosphere-libs` subtree; use it for trace collection, but do
  not treat that fork as an acceptable long-term architecture

## Workstreams

### 1. Passive Service MITM

Purpose:
Observe how the rest of Horizon uses `nifm`, `wlan`, and later `bsd:*` without
changing behavior.

Action goals:

- Add passive MITM targets for `nifm` and `wlan`.
- Use [TRACE_SCHEMA.md](/workspaces/nx-reversing.git/docs/methods/TRACE_SCHEMA.md:1)
  as the baseline logging contract.
- Log command IDs, caller PID, handle flow, copied buffers, output handles,
  result codes, and timing.
- Forward all calls unchanged.
- Keep logs structured enough to diff between flight mode, idle wireless,
  associated wireless, and later wired ethernet.

Research goals:

- Identify which services applications and system components actually talk to
  during network bring-up.
- Determine whether `nifm` is mostly policy/lifecycle and whether `wlan` is the
  first useful uplink-facing boundary.
- Identify whether `wlan` traffic is mostly control-plane IPC or whether any
  packet-adjacent buffer flow is visible from public service calls.

Exit criteria:

- We can explain the common `nifm` session flow for "offline", "Wi-Fi enabled",
  and "Wi-Fi connected".
- We can explain the common `wlan` session flow without crashing the system.
- We know whether `wlan` public IPC is useful for anything beyond device state
  and lifecycle.

### 2. Runtime Probe Hardening

Purpose:
Keep `net-probe` useful as a focused runtime instrument without letting it turn
into a destructive fuzz target.

Action goals:

- Continue narrow command probes only when static analysis suggests exact
  signatures or ordering.
- Prefer read-only or event/state commands first.
- Gate risky commands behind explicit test modes and clear logging.
- Preserve per-scenario logs for:
  - flight mode
  - Wi-Fi enabled but unassociated
  - Wi-Fi connected
  - wired ethernet when hardware becomes available

Research goals:

- Correlate public `anif` commands with concrete state transitions.
- Confirm whether `wlan` / `eth` objects can ever satisfy the `bsd:nu::Assign`
  preconditions directly.
- Learn whether link precedence or uplink coexistence is externally visible.

Exit criteria:

- Probe coverage is sufficient to validate static hypotheses before attempting
  deeper integration work.
- Known crash cases are documented and avoided by default.

### 3. Kernel and Core-KIP Reversing

Purpose:
Determine whether the kernel or core KIPs expose a reusable lower-level
networking abstraction that could support a synthetic uplink or deeper patch.

Action goals:

- Import and analyze:
  - `pkg/main/pkg2/kernel.bin`
  - `pkg/main/pkg2/ini/sm.kip1`
  - `pkg/main/pkg2/ini/Loader.kip1`
  - `pkg/main/pkg2/ini/ProcessMana.kip1`
- Build a map of:
  - SVC usage relevant to IPC, memory, events, and device access
  - kernel object types used by networking sysmodules
  - shared-memory / transfer-memory / device-memory patterns
- Compare generic kernel structures against Mesosphere where that speeds up
  naming and orientation.

Research goals:

- Determine whether Horizon kernel networking logic is substantive or whether
  the kernel mostly supplies generic plumbing used by userland sysmodules.
- Identify the lowest boundary where packets or frame-like buffers might become
  visible.
- Determine whether a custom KIP or Mesosphere patch could host a synthetic
  network device without re-implementing the entire socket semantics in
  userspace.

Exit criteria:

- We can answer whether there is a realistic kernel-adjacent insertion point
  below `wlan` / `eth`.
- We can explain what a custom KIP would need to emulate or patch.

### 4. Dynamic Debugging

Purpose:
Bridge the gap between static reversing and implementation by watching selected
 sysmodules at runtime.

Action goals:

- Set up repeatable debug sessions against:
  - `nifm`
  - `wlan`
  - `eth` when wired hardware is available
  - `bsdsockets` only after lower layers are understood
- Trace selected SVC and IPC activity around:
  - `svcSendSyncRequest`
  - `svcReplyAndReceive`
  - shared-memory creation and mapping
  - transfer-memory creation and mapping
  - synchronization waits and events

Research goals:

- Recover handle and memory-object topology used during network bring-up.
- Identify whether packet-adjacent queues or mapped buffers exist at the
  `wlan` / `eth` boundary.
- Correlate runtime object flow with static reverse findings from `pkg2` and
  the extracted sysmodules.

Exit criteria:

- Static hypotheses about the uplink boundary are confirmed or rejected with
  runtime evidence.
- We know whether kernel instrumentation is necessary.

### 5. `bsd:*` MITM Fallback and Measurement

Purpose:
Keep the practical fallback moving in parallel without committing to it too
early.

Action goals:

- After `nifm` / `wlan` passive MITM is stable, add passive MITM coverage for
  `bsd:u` and `bsd:s`.
- Record socket lifecycle, endpoint data, command mix, and result timing.
- Measure how much of the application-visible surface would need to be
  virtualized for policy routing through WireGuard.

Research goals:

- Determine whether `AllowedIPs`-style steering can be implemented with modest
  socket virtualization or whether full userspace TCP/IP emulation would be
  required.
- Compare complexity and performance expectations against the kernel/uplink
  path.

Exit criteria:

- We have a grounded estimate of how expensive `bsd:*` MITM would be in code,
  CPU, and RAM.
- We can decide whether the native path is still worth chasing.

## Sequence

### Phase 1: Stabilize observation

1. Keep the current `net-probe` usable and document unsafe calls.
2. Add passive MITM for `nifm`.
3. Add passive MITM for `wlan`.
4. Collect traces across the known Wi-Fi scenarios.
5. Update notes with observed call graphs and state transitions.
6. After the first useful trace set is captured, remove or isolate the local
   `Atmosphere-libs` fork and preserve only the minimum required observation
   layer.

Decision gate:
If `wlan` remains purely control-plane, prioritize `pkg2` and lower-boundary
analysis over more public `wlan` probing.

### Phase 2: Build the lower-boundary model

1. Reverse `pkg2/kernel.bin` for IPC, memory, event, and device primitives.
2. Reverse `sm.kip1`, `Loader.kip1`, and `ProcessMana.kip1` for service and
   process context.
3. Correlate `pkg2` findings with `wlan_main`, `eth_main`, and `nifm_main`.
4. Prepare targeted runtime debugging sessions based on those findings.

Decision gate:
If a reusable lower-level queue, object, or memory contract is visible, begin
designing a synthetic uplink or custom KIP proof of concept.

### Phase 3: Decide the integration path

1. If the lower boundary looks viable:
   - design a native-ish uplink proof of concept
   - define the minimum kernel or KIP patch surface
   - keep `bsd:*` as a diagnostic fallback only
2. If the lower boundary does not look viable:
   - add passive `bsd:*` MITM
   - measure the socket surface
   - decide whether a selective proxy/virtualization layer is acceptable

## Artifacts To Maintain

- `docs/20.5.0/notes/`
  - keep per-service notes split by topic
- `reports/`
  - preserve runtime logs, fatal reports, and scenario labels
- `net-probe/`
  - keep probe code narrow, explicitly mode-driven, and well-logged

## Near-Term Next Steps

1. Deploy the compiled passive MITM for `nifm` and `wlan`.
2. Collect and classify traces for the three known Wi-Fi states.
3. Continue the `pkg2/kernel.bin` checklist in parallel.
4. Fold trace results back into the next narrow probe changes.
