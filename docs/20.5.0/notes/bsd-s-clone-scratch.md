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
