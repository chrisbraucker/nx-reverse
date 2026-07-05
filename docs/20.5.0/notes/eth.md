# 20.5.0 `eth_main`

## 2026-06-17: eth:nd driver-service path and OpenNetworkInterface

### Confirmed creator path

The top-level `eth:nd` creator path is now substantially clearer.

- `FUN_71000030e0`
  - appears to be the `eth:nd` service bootstrap / IPC server setup path
  - calls `FUN_7100003390(...)`
- `FUN_7100003390`
  - allocates a large backing state object (`0x8e8` bytes, plus internal substate)
  - then calls `FUN_7100017f50(...)`
- `FUN_7100017f50`
  - builds the smaller returned wrapper object through `FUN_71000180b0(...)`
  - returns a `Result`, with a default error of `0x40025` when setup cannot proceed
- `FUN_71000180b0`
  - allocates a `0x58`-byte object
  - calls `FUN_7100018288(...)`
- `FUN_7100018288`
  - installs the small-object vtable and callback hooks

Interpretation:

- `eth:nd` cmd `0` `CreateDriverService` is very likely:
  - `FUN_71000030e0` -> `FUN_7100003390` -> `FUN_7100017f50` -> `FUN_71000180b0` -> `FUN_7100018288`

This matches the on-device probe result where `CreateDriverService` succeeds consistently.

### Small object: returned driver-service wrapper

The small wrapper object's vtable starts at `0x71000512d8`, with the actual object vptr set to `+0x10`.

Function slots after the two RTTI/destructor entries:

- `0x7100018420`
- `0x710001850c`
- `0x710001854c`
- `0x710001856c`
- `0x7100018580`
- `0x71000185cc`
- `0x71000185dc`
- `0x71000185e4`
- `0x71000185f0`
- `0x71000188b0`

These are internal service methods on the returned object. They are not yet mapped one-to-one to public cmd IDs, but one is now strongly identified:

- `0x71000185f0` is the `OpenNetworkInterface` implementation.

### OpenNetworkInterface: exact recovered path

`FUN_71000185f0`:

- takes the service object plus a structured input descriptor
- parses the input through these accessors:
  - `FUN_7100013e48` -> `u16` at offset `0`
  - `FUN_7100013e28` -> 6-byte value spanning offsets `2..7`
  - `FUN_7100013e38` -> `u16` at offset `8`
  - `FUN_7100013e40` -> `u16` at offset `0xA`
  - `FUN_7100013e20` -> `u64` / pointer-like value at offset `0x10`
- allocates:
  - a `0x338`-byte helper object
  - a `0x30`-byte helper object
- then constructs the larger returned interface object by calling:
  - `FUN_7100018770(...)`
  - which allocates a `0xA0`-byte object
  - and initializes it via `FUN_7100018a68(...)`

Important returned error:

- the function seeds `w22` with `0x00048425`
- if no matching interface/helper object is found, it returns that code

This matches the on-device probe exactly:

- `ISfDriverService::OpenNetworkInterface(...) -> 0x00048425`

Current interpretation:

- `0x00048425` is the "no eligible interface matched this request" path inside the real `OpenNetworkInterface` implementation, not a bogus probe-side artifact.

Probable request-layout implication:

- the parsed request shape is at least `0x18` bytes long
- the earlier `0x10` "DeviceFilter" assumption was too small to describe the full internal parser contract
- however, the later `0x20` / `0x40` / `0x80` zero-filled probe buffers still reached the same `0x00048425` internal failure, so the current blocker is still more likely semantic matching than raw buffer length alone

### Large object: returned network-interface wrapper

The larger interface object's vtable starts at `0x71000513c0`, with the actual object vptr set to `+0x10`.

Constructor chain:

- `FUN_7100018770`
- `FUN_7100018a68`

`FUN_7100018a68` caches a larger internal method table and installs a callback/dispatch block rooted at:

- `FUN_7100019000`
- plus nearby stubs at:
  - `0x710001908c`
  - `0x71000190c4`
  - `0x71000190d4`
  - `0x71000190f0`
  - `0x710001910c`
  - `0x7100019124`
  - `0x710001913c`
  - `0x7100019154`

This is consistent with the documented `ISfNetworkInterfaceService` side of the interface contract.

### Driver-service metadata methods: still unresolved

The earlier probe showed:

- `GetDriverInfo`
- `GetNetworkInterfaceList`
- `GetStateChangedEvent`
- `GetNetworkInterfaceListUpdatedEvent`

all immediately produce `0x0000F601` (`KernelError_ConnectionClosed`) on a fresh driver-service session.

Static reversing so far suggests:

- our current libnx-side calling contract for cmd `128..131` is still wrong, or
- those public commands are routed through a separate wrapper/dispatcher layer not yet pinned down from the small object alone

Evidence:

- the small returned object's visible methods do not cleanly line up with simple "out buffer only" wrappers for cmd `128..131`
- `FUN_71000185f0` is conclusively `OpenNetworkInterface`
- but the exact wrappers for `GetDriverInfo` / `GetNetworkInterfaceList` are still not pinned to specific service methods

### Practical takeaway

What is now verified:

1. `CreateDriverService` is real and reachable from the custom sysmodule.
2. `OpenNetworkInterface` is a real implemented path in `eth_main`, not just a documented stub.
3. `OpenNetworkInterface` internally constructs the larger `ISfNetworkInterfaceService` object.
4. The stable `0x00048425` result is a meaningful internal failure from the real open path.

What remains open:

1. exact cmd `128..131` contracts on the returned driver-service object
2. whether a real USB ethernet adapter changes the `0x00048425` result
3. whether it is more effective to keep pushing `eth:nd` or pivot toward synthetic `ISfNetworkInterfaceService` / `bsd:u` MITM paths

## 2026-06-17: interface-descriptor split and recovered network-interface command layer

### Important correction

The richer CMIF-facing descriptor block recovered around `0x7100050f98` is best interpreted as
the `ISfNetworkInterfaceService` command layer, not the `ISfDriverService` command layer.

Reasoning:

- the associated type/data blocks immediately adjacent to it are:
  - `0x7100051080` -> `ISfNetworkInterfaceService` type metadata
  - `0x71000510e0` -> `ISfNetworkInterfaceService` interface/object metadata
  - `0x71000510f8` -> object-factory / command-descriptor block that `0x7100050f98` points into
- the helper bodies reached from the stubs at `0x71000171d8` and later clearly operate on
  network-interface state such as shared memory, queue state, MAC/MTU-style setters, and
  communication state transitions
- several of those helper bodies match the documented `ISfNetworkInterfaceService` surface far
  better than the documented `ISfDriverService` surface

This means the previous assumption that the block at `0x7100050f98` might describe
`ISfDriverService` was too broad.

### Recovered command-wrapper layer

The block starting at `0x7100050f98` contains a dense wrapper/stub table that forwards into
concrete helper functions beginning at `0x71000175fc`.

Recovered wrapper targets include:

- `0x71000175fc`
- `0x7100017618`
- `0x7100017634`
- `0x710001766c`
- `0x7100017674`
- `0x710001767c`
- `0x71000176c0`
- `0x7100017704`
- `0x71000177f4`
- `0x71000178e4`
- `0x71000178f0`
- `0x71000178fc`
- `0x7100017908`
- `0x7100017910`
- `0x7100017918`
- `0x7100017924`
- `0x7100017930`
- `0x71000179cc`
- `0x7100017a8c`
- `0x7100017b4c`
- `0x7100017c0c`
- `0x7100017cb8`

These wrappers consistently dereference `*(param_1 + 8)` as the backing state and are therefore
working on the larger network-interface object, not on the earlier small driver-service wrapper.

### Semantics recovered from helper bodies

The following helpers are now characterized well enough to be useful:

- `FUN_71000155c0`
  - transitions interface state from "configured but not active" into a more active state
  - initializes both lower sub-objects rooted at `+0x148` and `+0x210`
  - returns `0x10425` when the state preconditions are not met
- `FUN_7100015470`
  - tears down the same lower sub-objects and clears the active flag
- `FUN_710001579c`
  - validates configured buffer sizes at `+0xa4` / `+0xa8`
  - lazily allocates lower resources when the interface is in the right state
- `FUN_7100015940`
  - moves the interface into a "communication started" state
  - requires both lower channel/resource blocks to be present
- `FUN_71000162b0`
  - serializes a `0xb0`-byte structure out of the backing state
  - reached by wrapper `FUN_710001767c`
- `FUN_7100015b0c`
  - reached by wrapper `FUN_71000178e4`
  - accepts a masked `u48`-style value, very likely MAC-address related
- `FUN_7100015c4c`
  - reached by wrapper `FUN_71000178f0`
  - accepts a `u16`
- `FUN_7100015d0c`
  - reached by wrapper `FUN_71000178fc`
  - accepts a `u16`
- `FUN_7100015dcc`
  - reached by wrapper `FUN_7100017908`
  - no extra arguments
- `FUN_7100015e80`
  - reached by wrapper `FUN_7100017910`
  - no extra arguments
- `FUN_7100015fb4`
  - reached by wrapper `FUN_7100017918`
  - accepts a `u32`
- `FUN_7100016070`
  - reached by wrapper `FUN_7100017924`
  - accepts a `u32`
- `FUN_7100015a34`
  - central ioctl-style backend
  - reached through wrappers:
    - `FUN_7100017930`
    - `FUN_71000179cc`
    - `FUN_7100017a8c`
    - `FUN_7100017b4c`
    - `FUN_7100017c0c`
    - `FUN_7100017cb8`

### Practical interpretation

This is enough to say that:

1. the returned larger object really does implement a rich network-interface command surface
2. that surface includes communication-state control, exported info blobs, and ioctl-style
   escape hatches
3. the remaining unresolved part is still the smaller `ISfDriverService` wrapper, especially the
   exact public contracts for cmd `128..131`

The immediate reverse target remains the same:

- recover the smaller driver-service wrapper's public CMIF command layer
- then correlate that layer to the documented `GetDriverInfo`, `GetNetworkInterfaceList`, and
  event getters

## 2026-06-17: recovered `ISfDriverService` public command table

### Public-facing table location

The smaller public `ISfDriverService` CMIF-facing handler table is best represented by the block
starting at `0x7100050d80`.

This block points at the following handler run:

- `0x71000167ac`
- `0x71000167b4`
- `0x71000141f4`
- `0x71000141fc`
- `0x710001680c`
- `0x7100016840`
- `0x7100016848`
- `0x710001685c`
- `0x7100016864`
- `0x710001686c`
- `0x7100016874`
- `0x7100016884`
- `0x7100016894`
- `0x71000168ac`
- `0x7100016900`

The first four entries are framework/lifecycle support:

- `0x71000167ac`
  - lock helper on `+0x80`
- `0x71000167b4`
  - paired unlock/release helper
- `0x71000141f4`
  - returns zero
- `0x71000141fc`
  - returns zero

The remaining eleven entries line up cleanly with the documented `ISfDriverService` method count.

### Command mapping

Best current mapping:

- `0x710001680c` -> `OpenNetworkInterface`
- `0x7100016840` -> `GetDriverInfo`
- `0x7100016848` -> `GetNetworkInterfaceList`
- `0x710001685c` -> `GetStateChangedEvent`
- `0x7100016864` -> `GetNetworkInterfaceListUpdatedEvent`
- `0x710001686c` -> `Ioctl`
- `0x7100016874` -> `IoctlRead`
- `0x7100016884` -> `IoctlWrite`
- `0x7100016894` -> `IoctlReadWrite`
- `0x71000168ac` -> `IoctlSetHandle`
- `0x7100016900` -> `IoctlGetHandle`

### Why this mapping fits

#### `OpenNetworkInterface`

`0x710001680c`:

- accepts an out-object slot plus an input buffer descriptor pair
- forwards into `FUN_7100016ab0(...)`

`FUN_7100016ab0(...)`:

- calls `FUN_7100014a40(...)` to search/match an existing interface by the supplied filter
- allocates a `0x68`-byte service object on success
- initializes it via `FUN_7100017308(...)`
- installs `&PTR_LAB_7100050fa0`
- stores the new object into the caller-supplied out slot

This is a direct fit for `OpenNetworkInterface`.

`FUN_7100014a40(...)` also materially strengthens the earlier interpretation of the stable
on-device `0x00048425`:

- it iterates the registered interface set
- serializes each interface candidate through `FUN_71000162b0(...)`
- compares the first `0x10` bytes of that serialized blob against the caller-supplied filter
- returns `0x48425` when no candidate matches

So `0x00048425` is now very clearly the "no matching interface found" path in the real driver
implementation.

#### `GetDriverInfo`

`0x7100016840` forwards into `FUN_7100016c30(...)`, which:

- calls `FUN_7100014d20(...)`
- writes one returned `u64` into the caller-provided out location

`FUN_7100014d20(...)` packs bytes from offsets `+8..+0xf` of the backing state into an 8-byte
return value. This fits a compact driver-info identifier much better than any of the other public
driver-service methods.

#### `GetNetworkInterfaceList`

`0x7100016848` forwards into `FUN_7100016c5c(...)`, which:

- passes an output buffer pointer and element-count/capacity into `FUN_7100014e10(...)`
- writes the returned count back through a separate out scalar

`FUN_7100014e10(...)`:

- walks the active-interface bitset
- serializes each interface via `FUN_71000162b0(...)`
- copies each serialized record into the caller buffer via `thunk_FUN_7100034600(...)`
- returns the number of emitted entries

This is a direct fit for `GetNetworkInterfaceList`.

#### Event getters

`0x710001685c` and `0x7100016864` forward into:

- `FUN_7100016c88(...)`
- `FUN_7100016ccc(...)`

These simply export handles from:

- `param_1 + 0x18`
- `param_1 + 0x48`

respectively, through `thunk_FUN_710001cdf0(...)`.

That is the expected shape for:

- `GetStateChangedEvent`
- `GetNetworkInterfaceListUpdatedEvent`

The exact ordering between those two names is still an inference, but it matches the documented
ordering and the presence of two separate event-holder fields.

#### Ioctl family

The remaining six entries map cleanly onto the documented ioctl family:

- `0x710001686c` -> `FUN_7100016d10(...)`
  - no buffer arguments
- `0x7100016874` -> `FUN_7100016da0(...)`
  - one "second-pair" buffer
- `0x7100016884` -> `FUN_7100016e54(...)`
  - one "first-pair" buffer
- `0x7100016894` -> `FUN_7100016f08(...)`
  - two buffers
- `0x71000168ac` -> `FUN_7100016fbc(...)`
  - handle-style transfer
- `0x7100016900` -> `FUN_710001705c(...)`
  - out-handle export

The pattern is the same as the richer `ISfNetworkInterfaceService` ioctl family:

- no-buffer variant
- read variant
- write variant
- read/write variant
- set-handle variant
- get-handle variant

### Callback backend used by the ioctl family

The driver-service ioctl family ultimately funnels into:

- `FUN_7100014608(...)`

That backend:

- requires the state at `+0xf` to be initialized
- then invokes a callback stored at backing-state offset `+0xc8`
- passing the service object stored at `+0xd0` and the ioctl selector

For the small driver-service object, `FUN_7100014ccc(...)` installs:

- callback `FUN_7100018958` at `+0xc0`
- callback stub `LAB_710001893c` at `+0xc8`
- the small driver-service object pointer at `+0xd0`

This explains why the ioctl family is implemented as a generic adapter layer rather than as fully
distinct handwritten methods.

### Practical implication for probing

This changes the probe strategy in a useful way:

- `OpenNetworkInterface` is already being called with the correct high-level IPC shape
- `GetNetworkInterfaceList` should use:
  - one output buffer for an array of `0xb0`-byte serialized entries
  - one separate out scalar for the returned entry count
- `GetDriverInfo` should use:
  - one simple `u64` out value, not a bulk output buffer
- the two event getters should request moved/copy handles, not buffers

So the earlier `GetDriverInfo` / `GetNetworkInterfaceList` probe shape was indeed wrong.
