# 20.5.0 `net-probe`

## 2026-06-18: current `net-probe` state and next expected signal

The current `net-probe` build was updated to match the recovered `ISfDriverService` contracts:

- `GetDriverInfo` now uses a plain `u64` out value
- `GetNetworkInterfaceList` now uses:
  - one output buffer
  - one separate out `u32` entry count
- the two event getters remain handle-returning calls
- `OpenNetworkInterface` still runs on a separate fresh driver-service session

This means future `eth` probe results should now be interpreted as follows:

- A successful `GetDriverInfo` should return a stable nonzero `u64` without immediately closing the
  driver session.
- A successful `GetNetworkInterfaceList` should return an emitted entry count and one or more
  `0xb0`-byte serialized interface records in the output buffer.
- On a device with a real wired adapter/dock path present, `OpenNetworkInterface` may stop
  returning `0x00048425`; if it still returns `0x00048425`, that strongly suggests no eligible
  ethernet interface matched the supplied filter.
- The two driver event getters should return valid handles without poisoning the fresh session used
  for metadata probing.

Because all known `eth` behavior so far is consistent with physical ethernet inventory, the next
high-value run is on a device that is actually connected via wired ethernet, not another Wi-Fi-only
probe.

## 2026-06-20: dual `eth:nd` and `wlan:nd` probe path

`net-probe` now walks both driver-service creators sequentially:

- `eth:nd`
- `wlan:nd`

For each service it now:

- opens the service creator
- creates a fresh `ISfDriverService` for metadata probing
- logs:
  - `GetDriverInfo`
  - `GetNetworkInterfaceList`
  - `GetStateChangedEvent`
  - `GetNetworkInterfaceListUpdatedEvent`
- if `GetNetworkInterfaceList` emits any `0xb0` interface records, creates a second fresh
  `ISfDriverService` and tries `OpenNetworkInterface` using the first `0x10` bytes of each emitted
  record as the filter key
- if `OpenNetworkInterface` succeeds and `bsd:nu` is available, attempts
  `CreateUserService -> Assign -> AddSession`

If a service reports no interface records, the probe logs that explicitly and skips record-based
open attempts for that service instead of continuing to blind zero-filled guesses.

## 2026-06-20: three-run `eth:nd` / `wlan:nd` results and WLAN crash note

The first dual-driver runs were collected in three states:

- flight mode enabled
- flight mode disabled, no Wi-Fi association
- Wi-Fi connected

Observed behavior:

- `eth:nd`
  - `GetDriverInfo` succeeds
  - `GetNetworkInterfaceList` succeeds but emits `0` records in all three runs
  - both event getters succeed
  - there is still no evidence of a usable non-USB interface on the test device
- `wlan:nd`
  - `GetDriverInfo` succeeds
  - `GetNetworkInterfaceList` emits exactly one `0xb0` interface record in all three runs
  - in the first two runs, `OpenNetworkInterface` on the first `0x10` bytes of that record
    succeeds and returns a live `ISfNetworkInterfaceService`
  - in those same two runs, `bsd:nu::Assign` rejects the returned WLAN interface with `0x00010425`
  - with Wi-Fi actively connected, the same `OpenNetworkInterface` attempt no longer succeeds and
    instead fails with `0x0000F601` (`KernelError_ConnectionClosed`)

The connected Wi-Fi run also coincided with a crash in:

- `wlan.autoge` (`Program ID 0100000000000016`)

The fatal report PC lands in a raw SVC wrapper and the visible stack is already in the fatal
reporting path, so it does not preserve the original failing WLAN routine. However, the crash is
still strongly correlated with the connected-state `wlan:nd::OpenNetworkInterface` probe, because:

- `net-probe` itself exits cleanly
- the crash only appears on the connected Wi-Fi run
- the fatal report TLS still contains the same `SFCI` command context and serialized WLAN interface
  record bytes seen in the probe log

Practical conclusion:

- offline/disconnected WLAN probing is useful and currently stable enough to continue
- connected-state WLAN `OpenNetworkInterface` probing is currently risky and should be avoided until
  the contract is better understood

## 2026-06-20: guard connected Wi-Fi before `wlan:nd::OpenNetworkInterface`

`net-probe` now queries `nifmGetInternetConnectionStatus(...)` up front.

If `nifm` reports:

- connection type = Wi-Fi
- connection status = `Connected`

then the probe:

- still runs the safe `wlan:nd` metadata path
- but skips the risky `wlan:nd::OpenNetworkInterface` call and logs the reason

This keeps the useful `wlan:nd` inventory and event probing available while avoiding the connected
state that previously triggered `0x0000F601` and a `wlan.autoge` fatal report.

## 2026-06-20: extend offline WLAN probe with direct interface-method calls

The next probe revision keeps the connected-Wi-Fi guard above, but deepens the offline/disconnected
`wlan:nd` branch once `OpenNetworkInterface` succeeds.

On the returned `ISfNetworkInterfaceService` it now attempts:

- `GetNetworkInterfaceInfo`
- `GetStateChangedEvent`
- `Duplicate`
- `GetNetworkInterfaceInfo` on the duplicate
- `BringUp`
- `GetNetworkInterfaceInfo` again
- `BringDown`
- `GetNetworkInterfaceInfo` again
- `bsd:nu::Assign` as before

For each `GetNetworkInterfaceInfo` call the probe logs a preview of the full `0xb0` descriptor and
also logs `memcmp` deltas against the original descriptor snapshot.

Expectation for a successful run:

- flight mode or disconnected Wi-Fi should remain the preferred environment
- the probe should now tell us whether the public WLAN interface object itself can promote the
  descriptor state that `bsd:nu::Assign` currently rejects with `0x00010425`
- if `BringUp` / `BringDown` are only thin lifecycle wrappers, the descriptor will likely remain
  unchanged and the result codes will make that explicit

## 2026-06-20: isolate WLAN interface experiments onto fresh sessions

The previous offline WLAN deepening pass still had one important ambiguity:

- `GetNetworkInterfaceInfo` was attempted first on the newly opened interface
- if that command shape or command ID is wrong for the returned WLAN object, it may close the
  session immediately
- that would make all later `0x0000F601` results artifacts of the first bad call rather than
  independent evidence

To remove that ambiguity, `net-probe` now keeps two layers of WLAN probing:

- one baseline multi-call pass on a single opened interface, matching the old behavior
- a second pass where each higher-risk experiment runs on a freshly reopened interface session

The fresh-session experiment set is currently:

- `bring-up-start-communication-then-assign`
- `bring-up-start-communication-only`
- `bring-up-then-assign`
- `assign-only`
- `ioctl-only` with selector `0`
- `ioctl-get-handle-only` with selector `0`
- `get-info-only`
- `duplicate-only`

The ordering is now intentional:

- the state-transition experiments run first on clean fresh sessions
- `StopCommunication` and `BringDown` are attempted as cleanup after the start/communication path
- the more suspicious `GetNetworkInterfaceInfo` / `Duplicate` calls run last, since they are still
  the strongest candidates for "wrong command shape closes the session"

Expected value from the next run:

- if only the baseline multi-call pass collapses into `0x0000F601`, but one or more isolated
  experiments return distinct results, then the first invalid command was poisoning the session and
  the WLAN object is still worth pursuing
- if every isolated experiment also fails the same way on a fresh session, then the returned
  `wlan:nd::OpenNetworkInterface` object is much less likely to be a directly usable path for
  `bsd:nu::Assign`

## 2026-06-20: latest fresh-session interpretation changed

The latest fresh-session logs showed:

- `assign-only`
  - `bsd:nu::Assign -> 0x00010425`
- `ioctl-only`
  - `0x00020225`
- `ioctl-get-handle-only`
  - `0x00020225`
- `get-info-only`
  - `0x0000F601`
- `duplicate-only`
  - `0x0000F601`
- `bring-up-*`
  - `0x00010425` on the command we had labeled `BringUp`

The important correction is interpretive, not just numeric:

- these results no longer support "WLAN bring-up failed"
- they are more consistent with "the opened WLAN object does not match the richer interface-method
  contract we assumed from `eth_main`"

Supporting static reverse in `bsdsockets_main` now shows:

- `Assign` reads the descriptor through a local proxy object that forwards to remote vtable slot
  `+0x58`
- that proxy exposes a larger forwarded method table than the first visible WLAN-opened object
- `bsdsockets_main` also has an internal `nn.socket.AnifWorker` path that constructs this richer
  proxy class itself

So the current WLAN probe should be read as:

- `Assign -> 0x00010425` is still a real semantic admission failure on descriptor byte `0xa8`
- `0x00010425` on our current `BringUp` label is not evidence that we found the real WLAN
  bring-up method
- `0x0000F601` on `GetNetworkInterfaceInfo` / `Duplicate` remains strong evidence that at least
  some current command IDs or shapes are wrong for the returned WLAN object

Practical implication:

- another blind probe pass against the same assumed WLAN interface table is lower value now
- the next useful runtime change should wait until the `bsdsockets_main` proxy surface, or a
  safer bridge into it, is understood more exactly

## 2026-06-20: next targeted probe shape

Static reverse now gives a much tighter probe target for the candidate interface object consumed by
`bsd:nu::Assign`.

The assign preflight is:

1. candidate-interface command `0x82`
2. candidate-interface command `0x83`
3. candidate-interface command `5`
4. candidate-interface command `0x80`
5. require descriptor byte `0xa8 == 1`
6. use command `6` as rollback/cleanup on failure paths

Recovered behavior behind those commands:

- command `5`
  - simple scalar IPC command with no structured output
- command `6`
  - simple scalar IPC command with no structured output
- command `0x80`
  - copies out a `0xb0` descriptor blob
- command `0x81`
  - returns a packed `u32 + valid-byte` style result
- commands `0x82` and `0x83`
  - return multi-part configuration records that are immediately parsed into local state by
    `FUN_7100079b1c(...)` / `FUN_7100079b88(...)` and
    `FUN_710007993c(...)` / `FUN_71000799a8(...)`

So the next runtime probe should stop trying broad guessed method families and instead:

- open a fresh interface session
- issue exactly `0x82`
- issue exactly `0x83`
- issue exactly `5`
- issue exactly `0x80`
- log all raw return codes and, for `0x80`, the descriptor byte at offset `0xa8`
- if any step fails after `5`, attempt cleanup with `6`

Expected value:

- if `0x82` or `0x83` already fails, then the candidate object likely is not the same family that
  `Assign` expects
- if `0x82` and `0x83` succeed but `5` fails, then the object family may match while the runtime
  state is still wrong
- if `5` succeeds and `0x80` still yields `0xa8 != 1`, then we are very close and need the final
  state transition that toggles descriptor admission
