# 20.5.0 `bsdsockets` / `eth` / `anif` Model

## 2026-06-15: bsdsockets/anif/eth model

### Scope

Verified against:

- `workspace/20.5.0/exefs/bsdsockets_main`
- `workspace/20.5.0/exefs/eth_main`
- `workspace/20.5.0/strings/bsdsockets.txt`
- `workspace/20.5.0/strings/eth.txt`

SwitchBrew references used while interpreting the binaries:

- `Sockets services`: `bsd:nu` is `nn::anif::detail::ISfUserServiceCreator`
- `Ethernet services`: `eth:nd` is `nn::anif::detail::ISfDriverServiceCreator`

### bsdsockets_main: confirmed

- Plain service strings present in the binary:
  - `bsd:u`
  - `bsd:a`
  - `bsd:s`
- No plain `bsd:nu` string was found in `bsdsockets_main`.
- The earlier plain `Assign` hit was a false lead:
  - the string is `Assign random ip_id values`
  - this is not evidence of `ISfUserService::Assign`

Confirmed registration paths:

- The block at `0x710006fdb8` registers:
  - `bsd:u` via `FUN_71000d9944(..., 0, 0xf, ...)`
  - `bsd:a` via `FUN_71000d9944(..., 1, 0x17, ...)`
- `FUN_7100075df4` registers:
  - `bsd:s` via `FUN_71000d9944(..., 0, 0x7e, ...)`

Registration helper chain:

- `FUN_71000d9944`
  - thin wrapper over `FUN_71000d9994`
- `FUN_71000d9994`
  - allocates/attaches a server object after service-port creation
- `FUN_71000dd180`
  - dispatches service-port creation
  - falls back to a generic path through `FUN_71000dd5d0(...)`
- `FUN_71000dd0b0`
  - optional singleton matcher used by `FUN_71000dd180`
  - still not tied to `bsd:nu`

### bsdsockets_main: anif-related types and activity

Confirmed strings/types:

- `nn.socket.AnifWorker`
- `nn.socket.AnifIngress`
- `nn.socket.AnifEgress`
- `N2nn4anif6detail21ISfUserServiceCreatorE`
- `N2nn4anif6detail14ISfUserServiceE`
- `N2nn4anif6detail26ISfNetworkInterfaceServiceE`
- `N2nn4anif6detail34ISfAssignedNetworkInterfaceServiceE`

Confirmed behavior:

- `FUN_710006d0a0` starts the `nn.socket.AnifWorker` thread.
- Other nearby startup paths reference `nn.socket.AnifIngress` and `nn.socket.AnifEgress`.

Interpretation:

- `bsdsockets_main` definitely contains active `anif` user-side machinery.
- Even without a plain `bsd:nu` string, the binary is consistent with hosting the
  `ISfUserServiceCreator` / `ISfUserService` side of the documented `bsd:nu` IPC surface.

### eth_main: confirmed

Confirmed driver-side types:

- `N2nn4anif6detail23ISfDriverServiceCreatorE`
- `N2nn4anif6detail16ISfDriverServiceE`
- `N2nn4anif6detail26ISfNetworkInterfaceServiceE`
- `N2nn4anif3drv6detail22SfDriverServiceCreatorE`
- `N2nn4anif3drv6detail15SfDriverServiceE`
- `N2nn4anif3drv6detail25SfNetworkInterfaceServiceE`
- `N2nn4anif3drv6detail10DriverCoreE`
- `N2nn4anif3drv6detail20NetworkInterfaceCoreE`
- `N2nn4anif3drv6detail20NetworkInterfaceImplE`

Confirmed runtime behavior:

- `FUN_710000c800` starts:
  - `nn.eth.Ingress`
  - `nn.eth.Egress`
  - `nn.eth.Events`
- `FUN_710000d990` starts:
  - `nn.eth.Ingress`
  - `nn.eth.Egress`
  - `nn.eth.Events`
- `FUN_7100012e9c` starts:
  - `nn.eth.Ingress`
  - `nn.eth.Egress`
  - `nn.eth.Events`

Interpretation:

- `eth_main` is not just a thin IPC shim.
- It owns concrete driver/network-interface objects and active worker threads for the
  ethernet-side data path.

### Current model

This is the best-supported interpretation so far:

1. `eth_main` owns the concrete `ISfNetworkInterfaceService` implementation.
2. `bsdsockets_main` owns the `ISfUserServiceCreator` / `ISfUserService` side
   documented as `bsd:nu`.
3. `ISfUserService::Assign` likely binds a user-side wrapper to a service-session
   handle for `ISfNetworkInterfaceService`, matching the SwitchBrew description.

This matches the public docs:

- `bsd:nu` / `ISfUserServiceCreator` -> `CreateUserService` -> `ISfUserService`
- `ISfUserService::Assign(handle)` takes an `ISfNetworkInterfaceService` session handle
- `eth:nd` / `ISfDriverServiceCreator` -> `CreateDriverService` -> `ISfDriverService`
- `ISfDriverService::OpenNetworkInterface(...)` returns `ISfNetworkInterfaceService`

### Still unproven

- Exact `bsd:nu` registration path in `bsdsockets_main`
  - no plain `bsd:nu` string has been found
- Exact `CreateUserService` dispatcher in `bsdsockets_main`
- Exact `CreateDriverService` / `OpenNetworkInterface` dispatcher in `eth_main`
- Exact handoff point where an `eth`-owned `ISfNetworkInterfaceService` session is
  passed into the `bsdsockets` user-side path

### Next reverse targets

- In `bsdsockets_main`:
  - locate the `ISfUserServiceCreator` dispatcher
  - locate the `ISfUserService::Assign` handler
- In `eth_main`:
  - locate the `ISfDriverServiceCreator` dispatcher
  - locate the `ISfDriverService::OpenNetworkInterface` handler
- Then compare the interface boundary to confirm the concrete session flow.

## 2026-06-20: recovered `bsd:nu` public object tables and `Assign` gate

The `bsdsockets_main` `anif` user-side path is now concrete enough to map the public
`bsd:nu -> ISfUserService -> ISfAssignedNetworkInterfaceService` ladder.

### Recovered metadata blocks

The mangled type strings are referenced by the following metadata blocks:

- `0x7100171e10`
  - `N2nn4anif6detail21ISfUserServiceCreatorE`
- `0x7100171f40`
  - `N2nn4anif6detail14ISfUserServiceE`
- `0x7100172310`
  - `N2nn4anif6detail34ISfAssignedNetworkInterfaceServiceE`

The associated command-wrapper runs are:

- `ISfUserService` block rooted at `0x7100171f10`
  - `0x710007a678`
  - `0x710007a6a4`
  - `0x710007a6ac`
- `ISfAssignedNetworkInterfaceService` block rooted at `0x71001722d0`
  - `0x710007cd8c`
  - `0x710007cd94`
  - `0x710006ce10`
  - `0x710006ce18`
  - `0x710007cdec`

### `ISfUserService` command mapping

This now lines up with the public Switchbrew description for `bsd:nu`:

- `0x710007a678` -> `Assign`
- `0x710007a6a4` -> `GetUserInfo`
- `0x710007a6ac` -> `GetStateChangedEvent`

Supporting details:

- `0x710007abcc` is the `GetUserInfo` helper reached by the second stub
  - it packs bytes from the user-service state at offsets `+8..+0xf` into one returned `u64`
- `0x710007abfc` is the `GetStateChangedEvent` helper reached by the third stub
  - it returns the handle stored in the event object rooted at `self + 0x18`

### `Assign`: exact recovered path

`0x710007a678` is a thin wrapper over:

- `FUN_710007a810(...)`

That helper:

- allocates a small request-wrapper object (`0x28` bytes)
- allocates a larger per-assignment state object (`0x1e0` bytes)
- initializes the larger state with `FUN_710007cdf4(...)`
- queues the request wrapper into that larger state through `FUN_710007d0a4(...)`
- finalizes the larger state via `FUN_710007d170(...)`
- then performs the actual assignment gate through `FUN_710007ee20(...)`
- on success, allocates and returns the `ISfAssignedNetworkInterfaceService` object (`0x38` bytes)
  rooted at `PTR_LAB_71001722e0`

So `Assign` is not a tiny direct validator. It builds a real user-side state object, attaches the
incoming interface handle to it, and only then admits the interface into the assigned set.

### Exact source of `0x00010425`

The earlier runtime `Assign -> 0x00010425` result is now statically explained.

Inside:

- `FUN_710007ee20(...)`

the first meaningful gate is:

- it calls `FUN_710007dd30(&local_100, *param_2)`
- that helper obtains an object from the queued handle set and invokes vtable slot `+0x58`
  on the underlying interface object to serialize a full `0xb0`-byte descriptor into the local
  buffer
- `FUN_710007ee20(...)` then checks the first byte of the last qword in that descriptor, which is
  byte offset `0xa8`

If that byte is not equal to `1`, `FUN_710007ee20(...)` returns:

- `0x10425`

This matches the earlier WLAN reverse hypothesis exactly:

- the `Assign` gate is keyed off serialized descriptor byte `0xa8`
- our runtime `wlan:nd` handles are still presenting a descriptor whose `0xa8` byte is not in the
  accepted state for `bsd:nu`

### Other `Assign` outcomes visible in the code

Also inside `FUN_710007ee20(...)`:

- `0x10225`
  - user-side assignment state not initialized (`*(char *)(param_1 + 0xf) != 1`)
- `0x4c425`
  - duplicate assignment attempt: an existing assigned entry already matches the first `0x10`
    bytes of the serialized descriptor
- `0x44425`
  - no free assignment slot remains

### `ISfAssignedNetworkInterfaceService`

The returned assigned-interface object exposes a minimal public surface.

Best current mapping:

- `0x710007cd8c`
  - reference-count / framework helper
- `0x710007cd94`
  - framework helper that forwards into the assigned-state object
- `0x710007cdec` -> `AddSession`

`0x710007cdec` branches into:

- `FUN_710007e624(...)`

That helper:

- allocates another small request-wrapper object (`0x28` bytes)
- records the incoming handle/input state into that wrapper
- queues it into `param_1[2]` via `FUN_710007d0a4(...)`

So `AddSession` reuses the same queueing primitive used during `Assign`, but against the already
returned assigned-interface service state.

### 2026-06-20: internal `bsdsockets_main` network-interface proxy behind `Assign`

The static picture is now stronger than "some object eventually provides a `0xb0` descriptor".
`bsdsockets_main` builds and owns a richer local proxy class for network-interface objects, and
that proxy is what exposes the forwarded method table consumed by `Assign`.

Recovered constructor chain:

- `FUN_710006d0a0(...)`
  - initializes a global `nn.socket.AnifWorker`
  - obtains an internal source object through the `FUN_710007a05*` / `FUN_710007a07*` ladder
  - calls `FUN_710006d650(&DAT_71001e7000, ...)`
- `FUN_710006d650(...)`
  - allocates a large `0x5a0` state object
  - initializes it with `FUN_710007a09c(...)` and `FUN_7100079678(...)`
  - then calls `FUN_710007e700(...)` with mode byte `2`
- `FUN_710007e700(...)`
  - allocates a `0x110` inner state object
  - allocates a `0x30` helper object rooted at `PTR_FUN_7100172400`
  - calls `FUN_710007e860(...)`
- `FUN_710007e860(...)`
  - allocates a `0x60` public proxy object
  - initializes it through `FUN_710007f544(...)`
- `FUN_710007f544(...)`
  - installs vtable `PTR_DAT_7100178bd0 + 0x10`
  - stores two transferred object pairs plus four mode-derived values from:
    - `FUN_710007a07c(...)`
    - `FUN_710007a084(...)`
    - `FUN_710007a08c(...)`
    - `FUN_710007a094(...)`
  - registers callbacks through:
    - `FUN_710007f3fc(..., FUN_710007f9c4, LAB_710007fa10, self)`
    - `(...)->vtable[0x30/8](..., LAB_710007fa98, self)`

There is also a smaller `0x30` wrapper path:

- `FUN_710007f8d0(...)`
  - allocates a `0x30` proxy object
  - transfers an incoming object pair into it
  - initializes it via `FUN_710007fab4(...)`
  - stores a callback/handle value from `param_1 + 0x48`
- `FUN_710007fab4(...)`
  - installs metadata rooted at `PTR_DAT_7100178bd8 + 0x10`
  - stores the underlying object pointer at `this + 8`
  - clears `this + 0x18` / `this + 0x20`
  - registers callback `FUN_710007fc7c` via `FUN_710007db90(...)`

The public-facing wrapper table around `0x710007fb18 .. 0x710007fc78` now looks like:

- local wrapper -> remote vtable slot `+0x58`
  - `FUN_710007dd30(...)`
  - serialize `0xb0` descriptor used by `Assign`
- local wrapper -> remote vtable slot `+0x60`
  - `FUN_710007dba0(...)`
  - returns packed `u32 + byte`
- local wrapper -> remote vtable slots `+0x78`, `+0x80`, `+0x88`, `+0x90`, `+0x98`
  - `FUN_710007d4a0(...)`
  - `FUN_710007d600(...)`
  - `FUN_710007d760(...)`
  - `FUN_710007d8c0(...)`
  - `FUN_710007da20(...)`
- local wrapper -> remote vtable slots `+0xb0`, `+0xb8`, `+0xc0`, `+0xc8`
  - `FUN_710007ded0(...)`
  - `FUN_710007e030(...)`
  - `FUN_710007e1b0(...)`
  - `FUN_710007e330(...)`

The forwarding helpers all share the same shape:

- borrow one object from a local four-entry rotating pool at `+0x10/+0x18/+0x20/+0x28`
- invoke the remote method
- either return the borrowed object to the pool or release it

This means `bsdsockets_main` does maintain a richer local proxy surface for assigned interfaces,
but that does not by itself prove the caller must already provide a private local object class.
The `Assign` input path also builds a separate thin request wrapper that forwards concrete IPC
commands onto the caller-supplied interface object.

## 2026-06-20: `Assign` input wrapper command surface

The object queued by `FUN_710007a810(...)` is a small `0x28` wrapper rooted at
`PTR_LAB_7100172098` / `PTR_LAB_7100172190`.

Its vtable entries used by `FUN_710007d170(...)` and `FUN_710007ee20(...)` are now recoverable:

- slot `+0x48` -> `0x710007adb0`
  - forwards IPC command `5`
  - no structured output
- slot `+0x50` -> `0x710007adbc`
  - forwards IPC command `6`
  - no structured output
- slot `+0x58` -> `0x710007adc8`
  - forwards IPC command `0x80`
  - copies out a `0xb0` descriptor blob
- slot `+0x60` -> `0x710007add4`
  - forwards IPC command `0x81`
  - returns a packed `u32 + valid-byte` style result
- slot `+0x68` -> `0x710007ade0`
  - forwards IPC command `0x82`
- slot `+0x70` -> `0x710007adec`
  - forwards IPC command `0x83`

The generic wrappers behind those slots are:

- `FUN_710007b34c(...)`
  - scalar command sender used for command IDs `1..6`
- `FUN_710007b450(...)`
  - descriptor reader used for command `0x80`
- `FUN_710007b554(...)`
  - small packed-out reader used for command `0x81`
- `0x710007b670...`
  - larger multi-out reader used for commands `0x82` and `0x83`

So the `Assign` preflight sequence is now concrete:

1. call candidate-interface command `0x82`
2. call candidate-interface command `0x83`
3. call candidate-interface command `5`
4. serialize descriptor through candidate-interface command `0x80`
5. require descriptor byte `0xa8 == 1`
6. on failure paths, call candidate-interface command `6`

The intermediate state object stores the outputs of `0x82` and `0x83` in two `0xc8`-byte records:

- `FUN_7100079b1c(...)` / `FUN_7100079b88(...)`
- `FUN_710007993c(...)` / `FUN_71000799a8(...)`

Those helpers look like address/config record builders rather than arbitrary opaque blobs:

- `FUN_7100079b1c(...)` and `FUN_710007993c(...)`
  - each require two non-zero scalar values and set a `present` byte at `+0xc0`
- `FUN_7100079b88(...)` and `FUN_71000799a8(...)`
  - each require a non-zero handle/pointer aligned to `0x1000`
  - build a larger record from that backing region

This makes `0x82` / `0x83` the highest-value runtime probe targets, because they are the first
commands `Assign` uses before the final descriptor admission gate.

### Practical conclusion

This materially strengthens the current runtime interpretation:

1. `bsd:nu::Assign` is definitely the real command path we are calling from `net-probe`.
2. The rejection is not generic handle invalidity; it is a specific semantic gate on serialized
   interface-descriptor byte `0xa8`.
3. The current `net-probe` WLAN method labels should not be trusted yet, but we no longer have to
   guess blindly: the preflight command sequence is `0x82 -> 0x83 -> 5 -> 0x80`.
4. The main open question is now narrower:
   - does `wlan:nd::OpenNetworkInterface` return an object with this exact IPC surface but the
     wrong runtime state
   - or are we still probing the wrong object family entirely
5. The next runtime step should therefore be targeted, not broad:
   - replay the exact `Assign` preflight commands directly on a fresh candidate interface session
   - record which of `0x82`, `0x83`, `5`, `0x80` first diverges from the expected success path
