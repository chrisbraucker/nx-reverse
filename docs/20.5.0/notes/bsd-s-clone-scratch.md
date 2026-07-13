# 20.5.0 `bsd:s` Clone/Lifecycle Scratch Pad

This note is intentionally provisional. It tracks the current working set for
the repeated-client `bsd:s` freeze while the tracer and static reversing are
still moving.

## Current narrow hypothesis

The main `bsd:s` failure is more likely a duplicate-handle lifecycle bug than a
domain-child bug.

Why:

- runtime traces show `StartMonitoring` as a separate short root session with
  no returned object and no returned handle
- runtime traces show repeated `IHipcManager::CloneCurrentObject` on the main
  `bsd:s` session, each returning a move handle
- the failing `sphaira -> sphaira` run shows that same logical session later
  closing three times, which matches one original MITM session plus two clone
  registrations
- static reversing shows multiple `bsd:s` wrapper methods lazily cloning and
  caching duplicate handles before issuing their real commands
- static reversing also shows explicit duplicate replacement / close logic in
  the client wrapper layer
- the vendored Atmosphere MITM implementation confirms that
  `CloneCurrentObject` on a MITM domain session creates a fresh kernel session,
  clones the upstream forward `Service`, and registers the clone under the same
  logical monitor `session_id`
- upstream libnx `sessionmgrCreate()` clones the root BSD session once for each
  extra slot, so a client configured with `num_bsd_sessions = 3` should produce
  exactly two `CloneCurrentObject` calls before ordinary BSD traffic starts
- current upstream `sphaira` startup matches that shape exactly:
  it uses `socketInitialize(... num_bsd_sessions = 3, bsd_service_type = Auto)`
  and immediately starts async libcurl-backed network work from the main menu

## Static anchors

- `workspace/20.5.0/exefs/bsdsockets_main`
- `FUN_71000e41d0`
  - `StartMonitoring`
  - plain cmd `1`
  - no child-object setup observed
- `FUN_71000e35c0`
  - builds a `bsd:s` wrapper object
  - obtains an additional duplicate through `FUN_71000d79a0(..., 1)`
- `FUN_71000e43f4`
  - lazily clones a handle through `FUN_71000d7900`
- `FUN_71000e487c`
  - same lazy-clone pattern for another wrapper path
- `FUN_71000d7d00`
  - duplicate-handle update / replacement helper
- `FUN_71000d8050`
  - closes replaced handles
- upstream libnx source:
  - `nx/source/runtime/devices/socket.c`
    - `socketInitialize()` forwards `num_bsd_sessions` into `bsdInitialize()`
  - `nx/source/services/bsd.c`
    - `_bsdInitialize()` opens one root `bsd:*` session, opens a second
      monitor session, performs `RegisterClient`, then `StartMonitoring`, then
      builds a `SessionMgr`
  - `nx/source/sf/sessionmgr.c`
    - `sessionmgrCreate()` stores the root session in slot `0` and issues
      `cmifCloneCurrentObject(root_session, ...)` for every remaining slot
- upstream Sphaira source:
  - `sphaira/source/main.cpp`
    - `userAppInit()` calls `socketInitialize()` and `nifmInitialize()`
  - `sphaira/source/download.cpp`
    - `curl::Init()` creates one queue thread plus `MAX_THREADS = 4` worker
      threads and shares DNS / SSL-session / connection state through
      `curl_share`
  - `sphaira/source/ui/menus/main_menu.cpp`
    - `MainMenu::MainMenu()` immediately starts an async GitHub HTTPS request
  - `sphaira/source/ui/menus/menu_base.cpp`
    - the UI polls `nifmGetInternetConnectionStatus()` and
      `nifmGetCurrentIpAddress()` roughly once per second

## Runtime signatures to watch next

- first `sphaira` launch:
  - two `CloneCurrentObject` successes on the main `bsd:s` session
  - two root accepted MITM sessions total:
    - main session
    - short `StartMonitoring` side session
  - this now matches the libnx model directly:
    - one root BSD session
    - one monitor BSD session
    - two cloned root-session handles for a three-slot `SessionMgr`
- clone registration:
  - which server-side session handles were created for those clone returns
  - which cloned forward handles were attached to them
- disconnect:
  - whether that session reports outstanding cloned handles
  - whether the later close callbacks map 1:1 onto:
    - accepted root session handles
    - clone-created session handles
- freeze path:
  - whether any later manager-control activity still references the earlier
    session after the client should be gone
- whether the first launch fully tears down yet still prevents a second
  `AcceptMitmConnection(bsd:s)` from appearing
- reboot/autoboot path:
  - whether the same clone-lifecycle pattern appears during boot-sensitive
    clients like `olsc`

## Practical interpretation

This shifts the priority away from “what custom socket trick is Sphaira doing?”
and toward “does the passive `bsd:s` MITM remain compatible with libnx’s
pre-cloned BSD session pool and its later teardown?”

Important consequences:

- Sphaira itself is probably not the direct bug source here.
- The two `CloneCurrentObject` calls are expected behavior for
  `num_bsd_sessions = 3`, not an anomaly.
- The short `StartMonitoring` root session is also expected behavior from
  libnx, because `bsd.c` opens a dedicated monitor service handle and issues
  command `1` on it before the session manager is created.
- The remaining unknown is therefore in our MITM/session lifecycle:
  - accepted root-session ownership
  - clone-session ownership
  - wake/rearm behavior after teardown
  - interaction between those pieces and a second client launch

## Targeted tracer plan

1. Track successful `CloneCurrentObject` / `CloneCurrentObjectEx` returns per
   session.
2. Emit explicit clone-state records on issuance and at session disconnect.
3. Emit clone-registration records from the vendored Atmosphere MITM path with:
   - clone server session handle
   - returned client handle
   - cloned upstream forward handle
4. Emit root accepted-session records with:
   - accepted server session handle
   - attached upstream forward handle
5. Record outstanding clone count in manager-control events for clone commands.
6. Compare:
   - requester -> requester
   - sphaira -> requester
   - sphaira -> sphaira
7. If the freeze still occurs before new `bsd:s` activity, inspect whether the
   earlier session disconnect left clone state behind in the tracer.

## 2026-07-10 tracer update

The current probe now implements the session-lifecycle part of that plan:

- clone-return records from `IHipcManager::CloneCurrentObject*` remain tracked
  until the matching clone-created server session actually closes
- clone registration is correlated by matching the returned client move handle
  to the later `CloneRegistered` monitor event
- per-handle MITM session state is now logged separately from the logical
  `session_id`
- logical-session cleanup is deferred until the last tracked server handle for
  that `session_id` closes

This means the next `bsd:s` run should answer a narrower question:

- does the repeated-launch freeze still happen even when clone/session-handle
  teardown is represented accurately in the tracer, or
- was the earlier “stale clone” picture partly an artifact of collapsing
  multiple real server handles into one logical cleanup event?

## 2026-07-10 manager-state follow-up

The latest requester-only run strengthens one conclusion:

- the first `bsd:s` client can tear down to logical zero cleanly in the probe
  tracer
- disabling the probe immediately after that restores clean repeated launches
- therefore the remaining bug is still most likely in the *live MITM manager
  state*, not in requester itself and not in persistent system state

To narrow that further, the current probe now also logs the concrete
Stratosphere manager state:

- allocated MITM session slots
- allocated server/domain slots
- active multi-wait holder counts
- deferred multi-wait holder counts

These are emitted:

- after each successful `AcceptMitmConnection(...)`
- in every watchdog snapshot while tracked sessions exist

This should answer the next concrete question:

- after the first client exits, does the MITM manager actually return to:
  - one armed `bsd:s` MITM server holder
  - zero live session holders
  - zero deferred stale session holders

If not, the repeated-launch freeze is very likely a wait-set/session-pool leak
in the generic MITM plumbing rather than a higher-level `bsd:s` protocol issue.

## 2026-07-10 accept-path update

The latest tracer layer extends below the service-specific `bsd:s` hooks and
into the generic Atmosphere MITM accept pipeline:

- `ProcessForMitmServer` now emits begin/end events when a MITM port becomes
  signaled and `OnNeedsToAccept(...)` is entered
- `Server::AcknowledgeMitmSession(...)` now emits:
  - begin
  - forward-service allocation (`CreateForwardService`) becoming ready
  - end, including the acknowledged client identity and populated forward
    service state
- `AcceptMitmSessionImpl(...)` now emits:
  - begin
  - the accepted server session handle returned by `svc::AcceptSession`
  - final success after registration
- `RegisterMitmSessionImpl(...)` now emits begin/end with:
  - MITM session id
  - concrete server session handle
  - upstream forward handle
  - forward pointer-buffer size

This should answer the current open question much more directly:

- when the second Sphaira launch hangs before any higher-level `bsd:s`
  dispatch trace appears, do we still see the generic MITM accept path firing?
- if yes, where does it stop: SM acknowledgement, kernel `AcceptSession`, or
  session registration into the Stratosphere wait set?

## 2026-07-10 latest report readout

Latest analyzed capture:

- report root:
  - `workspace/reports/nxrv/probe/`
- active MITM configuration:
  - `nifm:u=0 nifm:s=0 bsd:u=0 bsd:s=1 bsd:a=0 ssl=0 ssl:s=0`
- user-observed scenario:
  - first `sphaira` launch succeeds
  - second `sphaira` launch hangs the system

What the trace confirms:

- only the first `sphaira` launch ever reaches the `bsd:s` MITM
- the first launch still matches the expected libnx topology:
  - two root accepted MITM sessions
    - main BSD client session
    - short `StartMonitoring` side session
  - two `CloneCurrentObject` returns on the main session
  - two clone-created server sessions registered under the main logical
    session
- after first-launch teardown:
  - both clone server-session handles report `close_observed`
  - both root server-session handles report `close_observed`
  - both logical root sessions report `client_disconnected`

## 2026-07-10 deferred-rearm tracer update

The next probe build adds one narrower accept-path event:

- `mitm_accept`
  - `operation = "mitm_server_wait"`
  - `phase = "promoted"`

This event fires when a deferred MITM server holder is observed in
`ServerManagerBase::LinkDeferred()` and is about to be moved back into the live
`m_multi_wait` set.

Current purpose:

- distinguish “the server loop never re-entered `LinkDeferred()` after the
  first launch” from
- “the `bsd:s` server was re-promoted into the wait set, but still never
  became selected for the second launch”

The event currently carries list counters in `detail_value*`:

- `detail_value0`: deferred-list total holder count before move
- `detail_value1`: active `m_multi_wait` total holder count before move
- `detail_value2`: deferred-list session-holder count before move
- `detail_value3`: active `m_multi_wait` session-holder count before move

What the trace does *not* show:

- no second-launch `mitm_accept` events at all
- no second-launch `ProcessMitmServer` begin/end
- no second-launch SM acknowledge for `bsd:s`

That matters because it pushes the failure below the existing service-specific
dispatch layer. The hang is happening before the next `bsd:s` connection even
enters the generic MITM accept pipeline.

Remaining anomaly in the first-launch teardown:

- root forward services emit `forward_service_state.phase = service_destructor`
  for handles `491634` and `589936`
- clone forward services for handles `753772` and `852074` do not emit a
  matching destructor record in this capture

That keeps a stale clone-forward-service / stale wait-registration hypothesis
alive:

- the server-side clone session handles *do* look closed
- the corresponding upstream cloned forward `Service` objects may still not be
  unwound symmetrically
- if those objects still occupy wait state or another lower-level registration,
  a later `bsd:s` client could block before `ProcessMitmServer(...)` is entered

Current best interpretation:

- this is no longer pointing at a bad `bsd:s` command forward
- it is more likely a lower-level lifecycle issue in the vendored Atmosphere
  MITM/session manager path
- the highest-value next instrumentation target is explicit destruction /
  unlinking of clone-created forward-service objects and any associated wait-set
  removal

## 2026-07-10 wait/destructor tracer extension

The next tracer pass is now wired into the vendored HIPC lifecycle below the
existing `bsd:s` dispatch hooks.

Added MITM-session events:

- `session_wait_registered`
- `session_wait_selected`
- `session_wait_finalized`
- `forward_service_destroy_begin`
- `forward_service_destroy_end`
- `session_destroy_begin`
- `session_destroy_end`

Added MITM-server accept-side events:

- `operation = "mitm_server_wait", phase = "queued"`
- `operation = "mitm_server_wait", phase = "selected"`

What this should answer on the next `sphaira -> exit -> sphaira` run:

- does the first `bsd:s` client actually leave the wait set before teardown?
- do clone-created sessions emit the same forward-service destruction signals as
  the root session?
- after first-launch teardown, does the `bsd:s` MITM server get re-queued and
  selected again before the second launch freezes?

This keeps the current working hypothesis narrow:

- if the second launch still never reaches `ProcessMitmServer(...)`, but we now
  see incomplete `session_wait_finalized` / `forward_service_destroy_*` coverage
  on the first launch, the defect is very likely in session-manager lifecycle
  handling rather than command forwarding.

## 2026-07-10 requester exit crash and manager-state follow-up

Latest `requester -> exit-error -> requester-hang` run:

- first requester launch completed all scenarios successfully
- requester bootstrap reached `main: exit`
- the app crash report is `User Break` in `hbl`, with requester frames
  symbolizing to libnx `_exit` / `__syscall_exit`
- second requester launch produced no requester log, matching the earlier
  observation that the hang happens before requester `main()` logging

`bsd:s` trace shape from the first requester launch:

- two root `bsd:s` sessions were accepted for requester
- two additional clone sessions were registered under the first root session
- logical tracker state eventually reached `active_session_count=0`
- outstanding clone count reached zero before the final root disconnect

Important limitation in that capture:

- manager resource/wait-set snapshots were only emitted while sessions were
  tracked
- the probe therefore missed the exact post-teardown state after
  `active_session_count=0`

New diagnostic patch:

- watchdog now emits six additional manager-state snapshots after tracked
  sessions drop from nonzero to zero
- this should show whether the manager returns to a clean state:
  one active `mitm_server` holder, zero active/deferred session holders, and no
  leaked session allocations

If the next run shows clean post-zero manager state but second launch still
hangs before accept, the likely failure point moves earlier than the HIPC server
manager wait set, probably in SM/MITM handoff state for `bsd:s`.

## 2026-07-11 registration failure hardening

A later run produced a different failure mode:

- `RegisterMitmServer(bsd:s)` returned `0x815`
- no `bsd:s` MITM server became active
- requester then stalled during `socketInitialize()`
- the fatal context identified `0100000000000004` (`sm`) aborting, not the
  requester or a live probe-side dispatch path

This is distinct from the normal repeated-launch `bsd:s` lifecycle hang. It is a
startup/registration failure, and the probe must not continue as though it owns a
valid MITM endpoint.

Probe-side hardening now treats any requested MITM registration failure as fatal
to probe startup:

- log `HasMitm(service)` immediately before each registration attempt
- log `HasMitm(service)` immediately after success or failure
- if any enabled target fails registration, tear down any partial manager state
  and return failure to `main`
- on this failure path, do not run residual `UninstallMitm` cleanup, because the
  service may already be owned by another MITM
- `main` then shuts down the logger and exits instead of leaving a half-active
  probe in the system

For a later WireGuard sysmodule, this should become a startup invariant rather
than probe-only behavior: MITM registration must be all-or-nothing, with explicit
pre/post `HasMitm` evidence and no best-effort continuation after a failed
critical service registration.

## 2026-07-11 post-teardown watchdog correction

The latest `requester -> wait -> requester` run with only `bsd:s` enabled
tightened the repeated-launch failure again:

- first requester launch completed and cleaned up from the application's point
  of view
- the `bsd:s` trace ended with final `Close` activity and tracker state dropping
  to zero sessions
- no second requester bootstrap log appeared
- no second `ShouldMitm(bsd:s)` / accept event appeared

One important diagnostic caveat was found in the probe itself:

- the watchdog called `GetDebugMultiWaitState()` after teardown
- that accessor takes the server manager selection mutex
- the server manager intentionally holds that same mutex while blocked in
  `WaitAny()`
- therefore a post-teardown watchdog snapshot can block when the manager is
  idle, making the diagnostic path unsafe

Patch applied:

- watchdog snapshots now use resource-only manager state plus `HasMitm(...)`
  checks
- wait-list snapshots remain available only for synchronous points where they do
  not contend with the sleeping manager thread
- vendored Atmosphere-libs now emits lower-level MITM session events:
  `process_for_session_begin`, `process_for_session_end`, and
  `session_native_handle_closed`

The next run should answer a narrower lifecycle question:

- does every first-launch session emit `process_for_session_end` and
  `session_native_handle_closed` after its final close?
- after tracked sessions drop to zero, does `HasMitm(bsd:s)` remain true while
  resource counts return to their expected baseline?
- if both are true and the second launch still hangs before a new accept, the
  failure point is likely outside command forwarding and outside normal
  `ServerSession` destruction, probably in the SM/MITM handoff or service
  connection path for `bsd:s`.

## 2026-07-11 Atmosphere `sm` MITM handoff check

Pinned Atmosphere source was checked out at:

- `workspace/repos/Atmosphere`
- commit `de9b02007bbfc62f2f9ee2abf4a96bc337f5f86a`

The relevant upstream implementation detail is in
`stratosphere/sm/source/impl/sm_service_manager.cpp`:

- `GetMitmServiceHandleImpl(...)` calls the MITM query service
- when `ShouldMitm` returns true, `sm` creates a forward session, connects to
  the MITM port, then sets `mitm_info->waiting_ack = true`
- later `GetService` calls for the same service return
  `tipc::ResultRequestDeferred()` while `waiting_ack` remains true
- `AcknowledgeMitmSession(...)` clears `waiting_ack` and transfers the forward
  session handle to the MITM process

This matters for the pre-`main()` second-launch hang: application bootstrap can
block in service acquisition before requester logging starts if a prior or
current `bsd:s` handoff leaves `waiting_ack` uncleared. This does not require a
bad BSD command dispatch; it can happen entirely in the SM/MITM handshake.

Probe changes for the next run:

- `Server::AcknowledgeMitmSession(...)` now returns `Result` instead of
  aborting on failure
- existing `server_acknowledge` accept trace phases now bracket the actual SM
  acknowledge call and include the result
- `bsd:s` `ShouldMitm` has a temporary fail-closed liveness gate:
  - returning true increments a local pending count
  - successful accept/register promotes pending to active
  - service destruction drops active
  - while pending or active is nonzero, later `bsd:s` `ShouldMitm` calls return
    false
- the gate records decisions through the query trace ring, not file-backed
  `ShouldMitm` logging

Expected interpretation:

- if the second requester launch now succeeds and the query trace shows
  `should_mitm_policy_decision` with reason
  `BsdSystemSessionOutstanding`, the repeated-launch hang is probably caused by
  a stale or long-lived first `bsd:s` MITM session preventing a safe second
  handoff
- if the second launch still hangs and no second `ShouldMitm` appears, `sm` is
  probably deferring before the query call, which points directly at an uncleared
  `waiting_ack`
- if the trace shows `server_acknowledge` begin without end, the MITM accept
  thread is blocked inside the SM acknowledge call
- if `server_acknowledge` ends successfully but no
  `accept_mitm_session` / `register_mitm_session` completion follows, the fault
  is in probe-side accept/session registration after SM has already cleared
  `waiting_ack`

## 2026-07-11 MITM domain bookkeeping correction

The latest run showed another important mismatch with stock Atmosphere:

- after `ConvertCurrentObjectToDomain`, local domain snapshots stayed empty
  (`active_domain_count=0`)
- the vendored MITM path had intentionally stayed "passive" by converting the
  upstream forward service to domain mode but not registering the current MITM
  service object in the local domain table
- stock Atmosphere instead preserves the upstream domain object id, reserves
  that exact id locally, and registers the current MITM service object under it

That local registration is part of the MITM contract, not optional tracing
state. The MITM object still forwards commands it does not implement, but
manager operations, clones, and teardown now see a coherent local domain shape.

Patch applied:

- restore `ReserveSpecificIds(...)` for the upstream root object id
- restore `RegisterObject(...)` for the current MITM service object
- keep the existing aggressive domain conversion tracing

Expected next-run signature:

- after `ConvertCurrentObjectToDomain`, domain snapshots should show the local
  root object registered instead of an empty domain table
- the first requester run should still produce the expected two
  `CloneCurrentObject` calls for `num_bsd_sessions = 3`
- if the second requester launch still hangs before any new `ShouldMitm(bsd:s)`
  event, the passive-domain shortcut was not the whole cause and the remaining
  investigation should move back to SM handoff / service acquisition state

## 2026-07-11 post-domain test and forwarder launch path

The post-domain-bookkeeping run still black-screened on the second requester
launch. Important observations from that run:

- first requester launch completed normally and logged cleanup/exit
- all first-launch `bsd:s` MITM sessions and clone forward sessions reached
  service/session destruction and native handle close traces
- probe watchdog stayed alive after requester exit
- `HasMitm(bsd:s)` remained true with resource counts back at the expected
  idle baseline
- the apparent `ConvertCurrentObjectToDomain` entry in the JSONL was only
  manager command-id lookup for BSD command `0`; requester did not actually
  exercise domain conversion in this path
- no second requester log, second `ShouldMitm(bsd:s)`, or second MITM accept
  appeared

That makes stale BSD command forwarding, clone forwarding, and local domain
bookkeeping less likely as the immediate cause of the second-launch hang.

Two changes were made for the next diagnostic run:

- remove the temporary global "only one active `bsd:s` MITM session" gate; that
  gate created a mixed topology where the first requester process had one
  MITMed BSD session and one direct BSD monitor session
- add requester `__appInit` startup tracing by mirroring libnx default init and
  logging each stage:
  - early stages are emitted through `svcOutputDebugString`
  - once `fsdevMountSdmc()` succeeds, the trace is replayed to
    `sdmc:/nxrv/requester/requester-startup.log`
  - normal `main()` bootstrap logging remains unchanged

Sphaira forwarder notes:

- Sphaira installs forwarders by generating program/control/meta NCAs around
  its `hbl` payload, not by launching the NRO directly
- the generated program romfs contains `/nextArgv` and `/nextNroPath`
- on launch, `hbl` reads those romfs entries, opens the target NRO from SD,
  maps it with `svcMapProcessCodeMemory`, emits loader `svcBreak`
  notifications, then jumps to the NRO entrypoint through
  `nroEntrypointTrampoline`
- after the NRO returns, the trampoline resets the stack and branches back to
  `loadNro()` to unmap/reload or exit through applet self-exit

This means "no requester startup log on second launch" is ambiguous when using
the home-screen forwarder. The second launch may be stalling in:

- NS/loader before the Sphaira `hbl` title starts
- Sphaira `hbl` before it jumps to the requester NRO
- requester libnx startup before SD is mounted
- requester `main()` after SD is mounted

Next test interpretation:

- if `requester-startup.log` appears on the second launch, the failure point is
  inside requester/libnx startup or later; the last startup phase names the next
  target
- if neither `requester-startup.log` nor `requester-bootstrap.log` appears, but
  probe still shows no second `ShouldMitm(bsd:s)`, prioritize the forwarder/HBL
  and NS launch path over BSD command forwarding
- compare one run launched through the installed home-screen forwarder with one
  run launched directly from Sphaira/homebrew menu, if possible; a forwarder-only
  failure would implicate Sphaira `hbl` reload/exit state or NS application
  title launch state

## 2026-07-11: Sphaira launcher denylist test

Running the requester through Sphaira without the probe active succeeds, while
running the same Sphaira -> requester path with the probe active crashes before
any requester-side log is created. The fatal report names the Sphaira wrapper
process, not the requester forwarder, and the probe only observes successful
`bsd:s` activity from Sphaira before teardown.

For the next diagnostic build, the local Sphaira wrapper program is temporarily
included in the `bsd:s` denylist. This keeps Sphaira's launcher/update socket
traffic on the native service path while leaving the requester/forwarder path
eligible for MITM.

Expected interpretation:

- if Sphaira can launch requester with the probe active, the immediate trigger
  is Sphaira/HBL's own MITMed `bsd:s` use rather than requester traffic
- if it still crashes before requester startup logging, the failure is likely in
  the HBL/NRO handoff or app launch lifecycle while the probe is registered,
  not in Sphaira's socket command forwarding

Follow-up run:

- launch Sphaira first
- start the probe while Sphaira is already running
- launch requester from inside Sphaira

In this order, requester starts, completes all network scenarios, and logs normal
cleanup/exit. Sphaira then crashes as control returns from the requester NRO.

Important interpretation:

- requester logs prove the NRO reached libnx startup, `main()`, socket init, all
  request scenarios, cleanup, and exit
- the fatal report still names the Sphaira wrapper process and `hbl`, not a
  separate requester program
- every `bsd:s` IPC record for requester traffic is attributed to the Sphaira
  wrapper program, because Sphaira/HBL jumps into the requester NRO inside the
  same process
- all MITMed BSD sessions and clone sessions are destroyed and native handles
  are closed before the fatal report

This means the denylist test will intentionally bypass `bsd:s` MITM for any NRO
launched from inside Sphaira. It remains useful because it isolates whether
Sphaira/HBL survives the requester return path when its process never receives a
MITMed BSD session. To test requester traffic under its own program identity,
use the installed HOME requester forwarder instead.

Source correlation for the exit crash:

- Sphaira `hbl/source/trampoline.s` returns to `loadNro()` after the NRO
  entrypoint returns
- `hbl/source/main.c` checks whether the launched NRO requested another load;
  otherwise it exits through `selfExit()`
- when an NRO was mapped, `loadNro()` first emits `PreUnloadDll`, then calls
  `svcUnmapProcessCodeMemory` for `.text`, `.rodata`, and `.data + .bss`, then
  emits `PostUnloadDll`
- the observed fatal is still in `hbl`, after requester logs normal exit, with
  an `InvalidMemoryState`-class result

The installed HBL binary does not exactly match the locally built source by
address, so this is a source-level correlation rather than a line-accurate
symbolization. If the Sphaira denylist avoids the crash, instrumenting this HBL
return/unmap path becomes the next useful step.

## 2026-07-12: HOME requester forwarder denylist test

The Sphaira denylist test confirms that Sphaira's own `bsd:s` path can be kept
off the MITM path, but the installed HOME requester forwarder still hangs the
system on its second launch while the probe is active. The first HOME requester
run reaches the requester, completes its network scenarios, and drains all
observed MITM BSD sessions and clone sessions. The second HOME launch does not
emit requester startup logs and does not produce another observed `bsd:s`
`ShouldMitm` query or MITM accept.

For the next diagnostic build, the installed requester forwarder program
`0x05720820ABC97000` is temporarily included in the `bsd:s` denylist.

Expected interpretation:

- if HOME requester can launch repeatedly while this program is denylisted, the
  trigger is state left by the first launch's MITMed `bsd:s` interaction even
  though the visible service sessions drained cleanly
- if HOME requester still hangs on second launch while this program is
  denylisted, the active registered MITM service or another pre-requester launch
  dependency is perturbing the forwarder/app launch path before requester opens
  `bsd:s`

The build-config override is named `AllowBsdSystemRequesterForwarder`; it should
stay `false` for this isolation run and only be flipped for a targeted
transparency check.

Follow-up result:

- with the HOME requester forwarder denylisted, three consecutive requester
  launches completed successfully with the probe active
- all observed `ShouldMitm(bsd:s)` decisions for program
  `0x05720820ABC97000` returned denylist policy code 3
- there were no `AcceptMitmConnection(bsd:s)` records for the forwarder
- the requester reached `__appInit`, `main()`, socket init, all configured
  network scenarios, cleanup, and exit on every launch

This rules out the registered `bsd:s` MITM presence alone as the second-launch
trigger. The bad state is introduced only when the requester forwarder process
actually receives a MITMed `bsd:s` session.

## 2026-07-12: requester workload narrowing build

The next build re-enables requester-forwarder `bsd:s` MITM by setting
`AllowBsdSystemRequesterForwarder = true`. To avoid immediately repeating the
full failing workload, the requester now gates initialization and each scenario
behind booleans in `requester/src/config.hpp`.

Current matrix default:

- `EnableSocketInitialize = true`
- `EnableNifmInitialize = false`
- `EnableSslInitialize = false`
- `EnableCurlInitialize = false`
- all `EnableScenario*` values are `false`

This produces a socket-initialize-and-exit requester run while still exercising
the requester forwarder's MITMed `bsd:s` session lifetime. If two or three
consecutive launches survive, widen exactly one step for the next binary:

- DNS only: enable `EnableScenarioDnsResolve`
- NIFM snapshot only: enable `EnableNifmInitialize` and
  `EnableScenarioEnvironmentSnapshot`
- TCP connect/close: enable `EnableScenarioPlainTcpConnect`
- TCP idle lifetime: enable `EnableScenarioIdleTcpHold`
- UDP: enable `EnableScenarioUdpEcho`
- manual HTTPS path: enable `EnableSslInitialize` and
  `EnableScenarioHttpsGet`
- libcurl path: enable `EnableCurlInitialize` and one curl scenario

New lifecycle trace signals:

- `monitor.session` lines in `net-probe.log` mirror `bsd:s` session events such
  as accepted, clone registered, close observed, wait finalized, forward-service
  destroy, session destroy, and native handle close
- `monitor.forward` lines mirror forward-service handle state at accept and
  service destruction phases
- `PassiveMitmService destructor begin/end` marks destruction of the MITM
  service object itself

For the next run, the important comparison is whether the first launch reaches
final close/destructor/native-handle-close for every requester `bsd:s` session
before the second launch is attempted.

## 2026-07-12: requester forwarder bsd:s ordinal split

Disabling requester `socketInitialize()` allows repeated launches, while a
socket-only run with `num_bsd_sessions = 1` still hangs on the second HOME
launch. That removes the clone/session-pool path from the first suspect set:
the failing first run only needs libnx's two root `bsd:s` sessions.

The probe now has a temporary requester-forwarder-specific split mode in
`net-probe/src/build_config.hpp`:

- `RequesterForwarderBsdMitmMode::FirstOnly`: MITM only the first per-PID
  `bsd:s` query for program `0x05720820ABC97000`
- `RequesterForwarderBsdMitmMode::SecondOnly`: MITM only the second per-PID
  `bsd:s` query
- `RequesterForwarderBsdMitmMode::Both`: current fully MITMed failing case
- `RequesterForwarderBsdMitmMode::None`: equivalent to requester-forwarder
  denylist, known stable

The initial split-test build uses `FirstOnly`. Based on prior libnx traces, the
first `bsd:s` session is expected to carry `RegisterClient`, while the second is
expected to carry `StartMonitoring`. If this build survives repeated launches,
`StartMonitoring` through the MITM path becomes the stronger suspect. If it
still hangs, the transfer-memory/client-registration path is sufficient to
poison the next launch.

The ordinal decision is recorded through the in-memory SM query trace:

- `holder_tag` is the requester-forwarder `bsd:s` ordinal for the PID
- `detail_code` is the policy reason
- `detail_value3` is the configured ordinal split mode

The file-backed `ShouldMitm*` logging remains disabled.

Follow-up result:

- `FirstOnly` still hangs on the second HOME requester launch
- the split policy worked as intended: ordinal 1 was MITMed and ordinal 2 was
  denied for the requester-forwarder PID
- the only proxied BSD command from the requester forwarder was root command 0,
  `RegisterClient`
- no `StartMonitoring` command passed through the MITM in that build
- the first run still showed clean visible MITM session teardown before the
  second launch attempt

This narrowed the poisoned state to a MITMed `bsd:s::RegisterClient` path. At
that point, the working hypothesis was that Atmosphere's generic MITM
forwarder PID rewrite was unsafe for this specific command.

Atmosphere forwards MITMed requests that carry `send_pid` by replacing the
client PID word with `0xFFFE000000000000 | (original_pid & 0x0000FFFFFFFFFFFF)`.
The initial hypothesis treated that tag as the value delivered to
`bsdsockets`. Later source review showed that interpretation was wrong:
Mesosphere recognizes the `0xFFFE` marker, strips it, and copies the low PID
bits to the destination. An untagged PID sent by the probe is instead replaced
with the probe process PID by the kernel.

The next diagnostic build preserves the original PID only when forwarding root
command 0 on `bsd:s`. All other `send_pid` forwards keep Atmosphere's generic
MITM tag behavior. Forward-request traces now record:

- whether the request carried `send_pid`
- whether a root CMIF command ID was decoded
- the decoded root command ID
- the PID rewrite mode (`0` none, `1` Atmosphere tag, `2` preserved BSD
  RegisterClient PID)
- the PID word before preprocessing
- the PID word after preprocessing

Expected interpretation:

- if repeated requester launches now survive, the generic MITM PID tag is the
  failing ingredient for `bsd:s::RegisterClient`
- if the second launch still hangs, the trigger is deeper in the fact that
  `RegisterClient` was proxied at all, most likely around transfer-memory
  ownership, handle translation, or BSD's per-client resource registration

Correction after the first preserved-PID test attempt:

- the diagnostic did not actually preserve the PID, because the temporary root
  CMIF command decoder read `CmifInHeader` at the unaligned HIPC raw-data
  pointer
- the BSD trace independently decoded the same request as root command 0,
  `RegisterClient`, but the forwarding path logged `has_root_cmd=0` and
  `rewrite_mode=1`
- the forwarder now uses the same aligned raw-data read pattern as Atmosphere's
  manager request tracing before deciding whether to preserve the PID

The corrected preserved-PID test should show:

- `has_root_cmd=1`
- `rewrite_mode=2`
- `pid_before` equal to the real requester-forwarder PID
- `pid_after` still equal to the real requester-forwarder PID

Follow-up result, reinterpreted after reviewing Mesosphere PID handling:

- the corrected diagnostic preserved the requester's PID word only in the
  probe's outgoing TLS message
- because that word was untagged, Mesosphere replaced it with the sending
  probe's PID before delivering the request to `bsdsockets`
- the second HOME requester launch still stalled before requester startup
- this run did not rule out PID ownership; it tested probe-owned registration
  rather than requester-owned registration

The next suspect is request copy-handle lifetime in the generic MITM forwarder.
Libnx sends `bsd:s::RegisterClient` with `send_pid=true` and one copy handle:
the BSD transfer memory. The probe receives a duplicated handle for that
transfer memory, then forwards the saved HIPC message to the real `bsd:s`
service. Kernel copy-handle forwarding gives the real service its own duplicate
but does not consume the probe's duplicate. Atmosphere's forward path already
parses response copy handles and schedules those for closure, but it did not
close incoming request copy handles after forwarding.

That leak fits the observations:

- requester-forwarder denylisting is stable, because no transfer-memory handle
  is duplicated into the probe
- stopping/restarting the probe after one requester run is stable, because
  process teardown closes any leaked duplicate handles
- a long-lived probe with a proxied `RegisterClient` can keep the first
  requester's BSD transfer memory alive after the requester exits, poisoning
  the next launch even though probe-visible sessions have drained

The forwarder now snapshots incoming request copy handles before the TLS buffer
is reused, sends the request to the real service, then closes those probe-owned
copy handles immediately after `SendSyncRequest` returns. It currently closes
only request copy handles; move handles have transfer semantics and should be
handled separately if a later trace shows they matter. Forward traces now record
request copy/move counts, tracked copy-handle count, closed copy-handle count,
and the first four copied handle values.

Follow-up result:

- closing the probe-owned incoming transfer-memory copy handle did not fix the
  second-launch stall
- the trace confirmed the handle-close path ran for `RegisterClient`
- the toxic condition is therefore not a simple leaked duplicate transfer-memory
  handle in the probe process

The requester-forwarder ordinal split then tested the opposite side of libnx's
BSD bootstrap. With `RequesterForwarderBsdMitmMode::SecondOnly`, the first
`bsd:s` open for each requester PID was denied and the second was MITMed. Three
HOME requester launches completed successfully. The accepted MITM sessions were
all root command 1, `StartMonitoring`, with `send_pid=true` and no copy handles,
and each session disconnected and destroyed cleanly.

This moves the boundary to:

- `bsd:s::StartMonitoring` through the generic MITM path is stable for this
  requester case
- registered `bsd:s` MITM presence is stable when requester-forwarder traffic is
  denied
- accepting and tearing down a short requester `bsd:s` MITM session is stable
- `bsd:s::RegisterClient` through the MITM path is the remaining toxic surface

The next diagnostic intentionally blocks command 0 before it reaches the real
`bsdsockets` service. The probe still accepts the first requester-forwarder
`bsd:s` session and decodes `RegisterClient`, but `ForwardRequest` now has a
temporary BSD-specific mode:

- `BsdRegisterClientForwardMode::AtmosphereTaggedForward`: use Atmosphere's
  normal tagged-PID forwarding for command 0
- `BsdRegisterClientForwardMode::BlockWithCmifError`: close any probe-owned
  incoming request copy handles, synthesize a CMIF error response, and do not
  send the request to the forward session

The current build uses `BlockWithCmifError`, so requester `socketInitialize()`
is expected to fail. The useful result is whether repeated launches remain
possible after command 0 was MITMed but not registered with `bsdsockets`.

Expected interpretation:

- if repeated launches survive, the poison occurs inside or after the real
  `bsdsockets` `RegisterClient` registration reached through the MITM forward
  session
- if repeated launches still hang, the poison occurs earlier: accepting the
  command-0 MITM session, receiving the transfer-memory copy handle, or
  returning a failed CMIF response is sufficient

Upstream Atmosphere-libs implication:

- the previous domain bookkeeping and request-copy-handle cleanup remain
  plausible upstreamable hardening, but neither is yet proven as the BSD fix
- a BSD-specific `RegisterClient` workaround is not upstreamable as-is until the
  failing ingredient is isolated more precisely

## 2026-07-12 `RegisterClient` spoof diagnostic

The blocked-`RegisterClient` run was a useful positive result:

- the requester reached `main()` on repeated launches
- the probe accepted and decoded `bsd:s::RegisterClient` on repeated launches
- the probe received and closed the copied transfer-memory handle
- `socketInitialize()` failed with the intentionally synthesized
  `sf::hipc::ResultInvalidCmifRequest()`
- the device did not enter the previous second-launch black-screen stall

This rules out the earliest MITM mechanics as the sole poison source:

- MITM registration with SM is not enough
- accepting a requester `bsd:s` MITM session is not enough
- receiving the command-0 copied transfer-memory handle is not enough, assuming
  it is closed by the probe
- returning a synthetic CMIF error for command 0 is not enough

The remaining toxic boundary is therefore either forwarding `RegisterClient` to
the real `bsdsockets` service through the MITM forward session, or real
`bsdsockets` state created by that forwarded registration.

The current diagnostic moves one step closer to normal client behavior without
touching real `bsdsockets` registration state:

- `BsdRegisterClientForwardMode::SpoofSuccess` does not call
  `svc::SendSyncRequest()` for `bsd:s::RegisterClient`
- it closes probe-owned incoming request copy handles
- it synthesizes a successful CMIF response with the normal libnx output shape:
  one `u64` assigned BSD client PID
- it uses the original request PID as that assigned PID
- trace output marks this with
  `forward_bsd_register_client_mode=2` and
  `forward_bsd_register_client_spoofed=true`

Expected interpretation for the next run:

- if repeated requester launches still survive, then the prior stall requires
  real `bsdsockets` to see the MITM-forwarded `RegisterClient`
- if the first launch proceeds to `StartMonitoring` and fails there, the next
  step is a second spoof for command 1 so libnx can build its BSD session pool
  without real service state
- if the second launch stalls again despite spoofed command 0, the failure is
  tied to giving libnx a successful registration response and letting it
  continue, not to real `bsdsockets` state

## 2026-07-12 BSD input-buffer diagnostic

The spoofed-`RegisterClient` run allowed repeated requester launches, and a
plain UDP socket open/close also survived. The next crash boundary was the first
BSD operation that carries a HIPC input buffer: libnx `setsockopt()` dispatches
`bsd:s` command 21 (`SetSockOpt`) with `HipcAutoSelect | In` buffer metadata.

The observed stack-overflow fatal happened before the probe logged command 21,
which means the failure is probably in userspace/IPC marshalling or MITM server
buffer setup rather than inside the passive command forwarding code. This is
also consistent with command 2 (`Socket`) and command 26 (`Close`) surviving:
both are ordinary in-raw/out-raw commands without client input buffers.

Current diagnostic build changes:

- requester logs every `setsockopt()` boundary with fd, level, option, optval
  pointer, optlen, return code, and errno
- requester splits UDP option tests into:
  - `SO_REUSEADDR` scalar `int`
  - `SO_RCVTIMEO` `timeval`
  - `SO_SNDTIMEO` `timeval`
- probe has `AdvertiseZeroPointerBufferForRequesterBsd=true`, making
  `IHipcManager::QueryPointerBufferSize` report 0 to requester-forwarder
  `bsd:s` MITM sessions while retaining real 0x1000 server-side storage
- next requester default profile is intentionally minimal: socket init, UDP
  socket open/close, then `SO_REUSEADDR`; DNS and UDP echo are disabled

Expected interpretation:

- if `SO_REUSEADDR` now reaches the probe or succeeds, the crash is tied to the
  MITM pointer/static-buffer path selected by `HipcAutoSelect`
- if `SO_REUSEADDR` still crashes before command 21 is visible, any input
  buffer command is toxic even without an advertised pointer buffer
- if scalar `SO_REUSEADDR` succeeds but a timeout option crashes later, the
  issue is likely payload-shape or ABI-specific rather than generic buffer
  routing

Follow-up from the first zero-pointer-buffer build:

- setting the MITM server manager's real `PointerBufferSize` to zero is invalid
  for a MITM forward session whose upstream service advertises 0x1000 bytes
- the probe fatal decoded to Atmosphere-libs
  `ServerSessionManager::RegisterMitmSession()` during MITM accept, before
  requester `socketInitialize()` completed and before command 21 could be
  tested
- the failing invariant is that local MITM session storage must be at least as
  large as `forward_service->pointer_buffer_size`; for `bsd:s` that value was
  observed as 4096

The corrected diagnostic keeps real MITM session pointer storage at 0x1000 and
only lies to the requester-forwarder client from
`IHipcManager::QueryPointerBufferSize` for `bsd:s` MITM sessions. This preserves
Atmosphere's internal forward-session invariant while still testing whether
libnx chooses a different `HipcAutoSelect` buffer route for `SetSockOpt`.

Follow-up from the corrected zero-pointer-buffer build:

- the requester reached `main()` and completed `socketInitialize()` with
  spoofed `RegisterClient`
- the requester completed the minimal UDP socket open/close scenario
- the requester then entered the `SO_REUSEADDR` scenario and stopped before it
  could log a `setsockopt()` result
- `probe-mitm-bsd.jsonl` contains the requester `bsd:s` session and the
  `IHipcManager::QueryPointerBufferSize` response with
  `response_pointer_buffer_size=0`
- the fatal report resolves inside the probe process, with `PC` in
  `_svfprintf_r`, `LR` in `snprintf`, and `SP == FAR`
- the fatal stack contains JSON fragments for `command_id=21`, which means the
  probe had reached the command-21 trace formatting path even though the JSONL
  line did not make it to disk

This changes the interpretation of the previous crash. The zero-pointer-buffer
advertisement did enough to let the first input-buffer BSD command reach the
probe. The observed fatal is now most likely probe diagnostic stack exhaustion,
not proof that `SetSockOpt` forwarding itself is toxic.

Current mitigation:

- the `wgnx-mitm` thread stack is raised from 16 KiB to 64 KiB
- this is intended to keep nested trace formatting alive long enough to log the
  `ipc_request`/`ipc_response` pair for command 21
- larger trace records are still queued through the existing logger path and may
  be truncated at the logger's queue-line limit; that is acceptable for the next
  run because the key question is whether command 21 reaches forwarding and
  returns

Expected interpretation for the next run:

- if the probe survives and logs command 21, then the prior failure was a probe
  trace-stack artifact and the next decision should be based on the actual
  `SetSockOpt` response/result
- if the probe still fatals in `snprintf` or adjacent trace formatting with
  `SP == FAR`, move the largest MITM trace scratch buffers off the `wgnx-mitm`
  stack before changing BSD behavior again
- if the device stalls without a probe fatal and without command 21 response
  logging, resume triage at the BSD input-buffer forwarding path

## 2026-07-13 `bsd:s` ownership requirement and upstream-registration test

The `bsd:u` success cases do not remove the need for `bsd:s` MITM. libnx uses
the same operational socket command table after initialization for `bsd:u` and
`bsd:s`, and homebrew defaults to `bsd:u`, but official/system-capability
clients can choose or require `bsd:s`. From packet capture alone this
distinction is mostly invisible: UDP/TCP packets do not carry a `bsd:u` versus
`bsd:s` marker. The productive distinction is the Horizon-side BSD client and
fd table created by `RegisterClient`.

Implication for the VPN work:

- packet capture can identify endpoint/protocol/timing
- MITM trace is required to attribute traffic to `bsd:u` or `bsd:s`
- socket fds are scoped to the BSD client/session state that created them
- a transparent VPN fallback cannot assume all application/system traffic will
  enter through `bsd:u`
- `bsd:s` MITM therefore has to work, even if the eventual transport policy is
  identical for `bsd:u` and `bsd:s` packets

The current stable diagnostic is still spoofed downstream
`bsd:s::RegisterClient`: command 0 is not forwarded to the MITM forward session,
incoming copied handles are closed, and libnx receives a synthetic success
response. This avoids the repeated-launch black-screen stall but leaves the
normal upstream forward session unregistered. The latest UDP `SendTo` failure
with `errno=90` is consistent with forwarding data-plane commands to a BSD
session that never received a successful upstream `RegisterClient`.

New experiment:

- `BsdRegisterClientForwardMode::SpoofSuccessWithProbeOwnedUpstream` keeps the
  known-stable downstream spoof behavior for requester-forwarder `bsd:s`
- on intercepted command 0, the probe sends the same command-0 registration
  payload over the MITM-provided forward service from
  `AcknowledgeMitmSession`
- that forwarded copy is rewritten to the probe process PID instead of the
  requester PID
- if upstream registration succeeds, later commands on the same MITM session are
  sent through that now-registered forward service
- trace output records:
  - `forward_bsd_register_client_mode=3`
  - `forward_bsd_probe_upstream_register_attempted`
  - `forward_bsd_probe_upstream_register_result`
  - `forward_bsd_probe_upstream_register_cmif_result`
  - `forwarded_to_bsd_probe_upstream`
  - `forward_target_handle`

Important limitation:

- this first experiment deliberately borrows the incoming command-0 transfer
  memory handle instead of allocating a full probe-owned transfer-memory pool
- if it improves `Socket`/`SendTo`, the next implementation step should replace
  the borrowed transfer memory with explicit probe-owned BSD socket memory and
  document that as the real architecture
- if repeated launches regress, the toxic boundary is narrower: a real upstream
  `RegisterClient` on the MITM-provided forward service, even with the probe
  PID and downstream spoofing, is enough to poison the lifecycle

Latest mode-3 run:

- three intercepted requester-forwarder launches reached `main`, completed
  socket cleanup, and exited without the old second-launch black-screen stall
- the accidental fourth requester run happened after probe disable, so it is
  not evidence that mode 3 fixed UDP forwarding
- every intercepted mode-3 `RegisterClient` attempted the probe-owned direct
  upstream registration, but the dispatch result was `0x0000F601`
  (`KernelError_ConnectionClosed`)
- because upstream registration failed, later `Socket`/`SendTo` calls remained
  on the unregistered MITM forward handle and still produced `SendTo ret=-1`
  / `errno=90`
- the trace also showed the downstream spoof response assigning a tagged
  `0xFFFE...` PID, which means mode 3 was still entering Atmosphere's generic
  PID-rewrite path before synthesizing the downstream response

Follow-up implementation cleanup:

- treat `SpoofSuccessWithProbeOwnedUpstream` like `SpoofSuccess` during
  downstream preprocessing, so the requester receives its original PID in the
  synthetic `RegisterClient` success response
- preserve a separate copy of the original command-0 request for the upstream
  experiment, then rewrite only that copy to the probe PID immediately before
  sending it to the MITM-provided forward service
- keep closing the incoming copy handles after the upstream attempt, since the
  downstream request is still spoofed and not forwarded

Second mode-3 refinement:

- the direct `sm::GetServiceHandle("bsd:s")` upstream was removed from mode 3
- the probe now sends the probe-PID `RegisterClient` attempt on the existing
  MITM forward service
- on success, the stored probe-upstream service is an alias of that same forward
  service, so later data-plane calls keep using the service handle that BSD
  itself associated with command 0

Expected next-run signatures:

- `request_pid_rewrite_mode=4` on intercepted `bsd:s::RegisterClient`
- `assigned_pid` in the semantic response should match the requester process
  PID, not a `0xFFFE...` tagged PID
- if upstream registration still returns `0x0000F601`, the failure is not caused
  by opening the wrong service through SM; it is caused by replaying command 0
  from this low-level MITM path
- if upstream registration succeeds, later `Socket`/`SendTo` should show
  `forwarded_to_bsd_probe_upstream=true` and `forward_target_handle` equal to
  the original forward handle
- if `SendTo` still returns `errno=90` after successful upstream registration,
  then `bsd:s` likely requires the rest of libnx's registration sequence
  (`StartMonitoring` or related monitor state) before data-plane commands are
  usable on that handle

## 2026-07-13 raw requester BSD lifecycle ladder

The mode-3 result does not by itself distinguish an Atmosphere forwarding bug
from a lifecycle mismatch introduced by the probe. Mode 3 registers the MITM
forward service under the probe PID, returns a synthetic client ID downstream,
and lets the requester's real `StartMonitoring` call bypass the intercepted
root. That is intentionally useful as a data-plane experiment, but it is not a
faithful libnx lifecycle and cannot identify the first native operation that
poisons a later launch.

The requester now contains a raw `bsd:s` lifecycle scenario which does not call
`socketInitialize()`. It mirrors the relevant IPC structures and ordering from
the pinned libnx `bsd.c` implementation while exposing each added lifecycle
phase as a boolean in `requester/src/config.hpp`:

- `ManualBsdOpenMonitorSession`
- `ManualBsdCreateTransferMemory`
- `ManualBsdRegisterClient`
- `ManualBsdStartMonitoring`
- `ManualBsdCloneRootSession`

Compile-time assertions reject combinations where monitoring lacks a monitor
session or returned client ID, cloning lacks registration, or the raw harness
and `socketInitialize()` would run together.

The initial test configuration is deliberately minimal:

- applet exit locking disabled
- `socketInitialize()` and all ordinary network scenarios disabled
- open one `bsd:s` root session
- create transfer memory using the same size calculation and socket-buffer
  configuration used by libnx
- dispatch command 0 `RegisterClient` with the real requester PID descriptor
- capture the real returned BSD client ID
- skip the monitor session, command 1, cloning, sockets, and traffic
- send and record the root CMIF close request
- record the kernel handle-close result
- close transfer memory

The probe is configured for requester-forwarder `FirstOnly`, and the
Atmosphere-libs RegisterClient forwarding mode defaults to mode 0
(Atmosphere-tagged forwarding) for this test. Mode 3 remains available through
`WGNX_BSD_REGISTER_CLIENT_FORWARD_MODE=3`, but should not be used for the raw
lifecycle ladder.

Run the initial binary at least twice through the same forwarder while one probe
instance remains active. Interpret the result as follows:

- if the second launch stalls, successful `RegisterClient` followed by closure
  of the registered MITM forward session is the minimal known trigger; neither
  `StartMonitoring`, cloning, socket creation, nor traffic is required
- if repeated launches succeed, enable `ManualBsdOpenMonitorSession` and
  `ManualBsdStartMonitoring` together and repeat using the real command-0
  client ID
- if that succeeds, enable `ManualBsdCloneRootSession` and repeat
- only after those phases survive should socket creation and data-plane commands
  be reintroduced

The critical requester log markers are
`manual_bsd phase=register_client_complete`,
`manual_bsd phase=cleanup_root_close_request_complete`, and
`manual_bsd phase=cleanup_root_close_handle_complete`. A successful first run
followed by a second launch that never reaches requester startup logging would
tie the failure to server-side state retained or mishandled after that recorded
teardown, rather than to requester code executed after `main()`.

## 2026-07-13 raw lifecycle result and PID ownership correction

The minimal raw lifecycle reproduced the second-launch stall. The first
requester run completed all configured work:

- opened one `bsd:s` root session
- created the 67,534,848-byte transfer memory
- completed command 0 `RegisterClient`
- skipped monitoring, cloning, sockets, and traffic
- received `0x0000F601` from the explicit CMIF close request, then successfully
  closed the kernel session handle and transfer memory

The probe also observed a clean visible lifecycle: one accepted session, one
forwarded command 0, client disconnect, session finalization, forward-service
destruction, native-session close, and resource counters returning to zero.
No requester startup marker and no new `ShouldMitm` query appeared for the
second launch. The four retrieved ERPT reports did not contain a new fatal
attributable to the probe or `bsdsockets`.

Reviewing the exact Atmosphere 1.9.5 source at commit
`de9b02007bbfc62f2f9ee2abf4a96bc337f5f86a` exposed a test-definition error:

- stock `sf_hipc_server_session_manager.cpp` tags every forwarded `send_pid`
  value as `0xFFFE000000000000 | original_pid`
- Mesosphere `kern_k_server_session.cpp` recognizes that marker, strips it,
  and delivers the original requester PID to the destination process
- for an untagged value, Mesosphere ignores the supplied word and delivers the
  sending process's PID instead
- the fork's former mode 0 special-cased `bsd:s::RegisterClient` by leaving the
  PID untagged, so real `bsdsockets` received the long-lived probe PID

That behavior provides a concrete lifecycle mechanism for the stall. BSD can
retain the first requester's client registration and copied transfer memory as
state owned by the still-running probe. Closing the requester session and the
probe's visible forward-service objects does not represent death of the PID
under which BSD registered that client. Restarting the probe clears that owner,
which is consistent with the earlier restart result.

Mode 0 now exactly follows Atmosphere's tagged-PID forwarding semantics. The
legacy untagged rewrite trace value 2 remains reserved so existing logs keep
their numeric meaning, but no current forwarding mode emits it.

For the next run, keep the requester in the same minimal raw configuration and
launch it at least three times while one probe instance remains active. The
first command-0 forward should show:

- `forward_bsd_register_client_mode=0`
- `forward_pid_rewrite_mode=1`
- `forward_pid_before_preprocess` equal to the requester PID
- `forward_pid_after_preprocess` equal to `0xFFFE000000000000 | requester_pid`

If repeated launches now succeed, incorrect probe-owned BSD registration was
the minimal root cause. If the second launch still stalls, the next split must
keep tagged PID ownership and vary only explicit CMIF session close versus
requester process disconnect.

## 2026-07-13 guarded UDP SendTo mutation result

The requester-only `bsd:s` SendTo mutation ladder now has successful positive
and negative-path validation. The probe matches command 11 only when the
requester forwarder sends an IPv4 sockaddr for the exact configured original
endpoint. It copies that sockaddr into probe-owned storage and asks the
Atmosphere-libs forwarder to replace send-static descriptor 1; it never writes
to requester-owned memory.

With port rewriting enabled, three consecutive requester launches sent UDP to
the harness on port 29001 even though each requester still specified port
29000. All three echo responses succeeded, every replacement trace reported
`substitution_applied=true`, and session/domain counts returned to zero.

The negative-path run left the port-29001 listener disabled. Three consecutive
requester launches each sent 18 bytes, reached the requester's bounded
five-second poll timeout, cleaned up normally, and remained relaunchable. The
probe again returned to zero active sessions and domains after each run. This
confirms that both successful forwarding and an absent rewritten destination
survive repeated lifecycle teardown.

The next mutation mode, `RewriteIpv4`, rewrites both bytes 4-7 of the copied
IPv4 sockaddr and its destination port. The expected original endpoint and the
replacement endpoint are configured independently in `build_config.hpp`.
Keeping the original-endpoint match unchanged prevents already rewritten or
unrelated SendTo calls from entering the mutation path. The trace event names
the mode `rewrite_ipv4_and_port` and records both endpoints so the descriptor
replacement can be correlated with the spare host's receive log.
