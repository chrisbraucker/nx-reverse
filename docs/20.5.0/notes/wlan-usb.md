# 20.5.0 `wlan_main` and `usb_main`

## 2026-06-20: first-pass triage of `wlan_main` and `usb_main`

### `wlan_main`: same `anif::drv` object model on the live Wi-Fi medium

The initial `wlan_main` pass shows that it is not a thin radio helper. It contains the same
high-level `anif::drv` ladder already recovered in `eth_main`:

- `ISfDriverServiceCreator`
- `ISfDriverService`
- `ISfNetworkInterfaceService`
- `SfDriverServiceCreator`
- `SfDriverService`
- `SfNetworkInterfaceService`
- `DriverCore`
- `NetworkInterfaceCore`
- `NetworkInterfaceImpl`

Key recovered strings and tables:

- `0x7100125e6a` -> `nn.wlan.AnifHipcServer`
- `0x7100136324` -> service-descriptor blob beginning with `wlan\0`
- `0x710016ec80` -> `ISfDriverServiceCreator` type/interface metadata block
- `0x710016f060` -> `ISfNetworkInterfaceService` type/interface metadata block

Important startup path:

- `FUN_71000e9640`

That function:

- caches platform/service state in globals
- calls `(**(code **)(*DAT_710051f010 + 0x10))(DAT_710051f010,&DAT_7100136324)`
- configures `nn.wlan.SharedMemoryTx` and `nn.wlan.SharedMemoryRx`
- starts the `nn.wlan.AnifHipcServer` worker via `FUN_7100007760(...)`

This is the strongest static evidence so far that the same `anif` driver-service /
network-interface architecture exists on the active Wi-Fi medium, not just in the wired ethernet
stack.

Notable implication:

- even without a plain preserved `wlan:nd` string, `wlan_main` looks like a viable next reverse
  target for understanding the generic `anif` object model on a transport that is actually present
  during ordinary testing

## 2026-06-20: recovered `wlan_main` `ISfDriverService` public command table

The next `wlan_main` pass shows that its public driver-service layer is structurally very close to
the one already recovered in `eth_main`.

### Creator-side entrypoint for probing

The `ISfDriverServiceCreator` side is now good enough to use as a probe target.

Best current creator block:

- `0x710016ec80`

Useful entries in that block:

- `0x71000f1c24`
  - best current match for `CreateDriverService`
- `0x71000f1ca0`
  - teardown/cleanup path for the creator-owned backing state

`0x71000f1c24` forwards into:

- `FUN_71000f4060(...)`

That helper:

- requires `param_1[1] != 0`
- obtains an auxiliary object through `FUN_710011e400()`
- allocates a `0x90`-byte object
- initializes it via `FUN_71000f4328(...)`
- installs creator/service metadata rooted at:
  - `PTR_DAT_710016fc68 + 0x10`
  - then `PTR_DAT_710016fc70 + 0x10`
- returns the created object through the caller's out-object slot
- returns:
  - `0x10025` when prerequisites are missing
  - `0x40025` on allocation/setup failure

This is the strongest current local match for:

- service open -> `ISfDriverServiceCreator`
- cmd `0` -> `CreateDriverService`
- result -> returned `ISfDriverService` object

This lines up with the public `wlan:nd` description on Switchbrew:

- service: `wlan:nd`
- creator cmd `0`: `CreateDriverService`

### Public-facing table location

The best current candidate is the block at:

- `0x710016ed90`

That block points at:

- `0x71000f41cc`
- `0x71000f41d4`
- `0x71000a9e50`
- `0x71000a4bc4`
- `0x71000f422c`
- `0x71000f4260`
- `0x71000f4268`
- `0x71000f427c`
- `0x71000f4284`
- `0x71000f428c`
- `0x71000f4294`
- `0x71000f42a4`
- `0x71000f42b4`
- `0x71000f42cc`
- `0x71000f4320`

The first four entries are framework/lifecycle support:

- `0x71000f41cc`
  - thin helper that adjusts to `self + 0x80` and branches into a lock/helper path
- `0x71000f41d4`
  - paired helper that operates on the object at `self + 0x88` and dispatches via vtable slot
    `+0x18` with object offset `0x90`
- `0x71000a9e50`
  - returns zero
- `0x71000a4bc4`
  - returns zero

That leaves the same 11-entry public method run seen in `eth_main`.

### Command mapping

Best current mapping:

- `0x71000f422c` -> `OpenNetworkInterface`
- `0x71000f4260` -> `GetDriverInfo`
- `0x71000f4268` -> `GetNetworkInterfaceList`
- `0x71000f427c` -> `GetStateChangedEvent`
- `0x71000f4284` -> `GetNetworkInterfaceListUpdatedEvent`
- `0x71000f428c` -> `Ioctl`
- `0x71000f4294` -> `IoctlRead`
- `0x71000f42a4` -> `IoctlWrite`
- `0x71000f42b4` -> `IoctlReadWrite`
- `0x71000f42cc` -> `IoctlSetHandle`
- `0x71000f4320` -> `IoctlGetHandle`

### Why this mapping fits

#### `OpenNetworkInterface`

`0x71000f422c` forwards into:

- `FUN_71000f44d0(...)`

That helper:

- calls `FUN_71000f2460(...)`
- allocates a `0x68`-byte returned service object
- initializes it through `FUN_71000f4d28(...)`
- installs `PTR_DAT_710016fc58 + 0x10`, then `PTR_LAB_710016efb0`
- writes the new object back through the caller's out-object slot

`FUN_71000f2460(...)` is the decisive part:

- it walks the active-interface bitset
- serializes each candidate interface with `FUN_71000f3cd0(...)`
- compares the first `0x10` bytes of the serialized candidate against the caller-supplied filter
- returns `0x48425` when no candidate matches

So this is the same high-level contract already seen in `eth_main`: `OpenNetworkInterface` is a
real filter-based open path over a set of already-registered interfaces.

#### `GetDriverInfo`

`0x71000f4260` branches into:

- `FUN_71000f4650(...)`

That helper:

- calls `FUN_71000f2740(...)`
- writes one returned `u64` into the caller's out location

`FUN_71000f2740(...)` packs bytes from backing-state offsets `+8..+0xf` into a compact 8-byte
value, which is a direct fit for `GetDriverInfo`.

#### `GetNetworkInterfaceList`

`0x71000f4268` branches into:

- `FUN_71000f467c(...)`

The decompiler under-types that wrapper, but the surrounding disassembly shows it loading a buffer
pointer/count pair from one incoming parameter block and forwarding a separate out-count slot.

The helper it reaches:

- calls `FUN_71000f2830(...)`
- writes the returned emitted-entry count back to caller memory

`FUN_71000f2830(...)`:

- walks the active-interface bitset
- serializes each interface through `FUN_71000f3cd0(...)`
- copies each serialized `0xb0`-byte record to the caller buffer
- returns the number of emitted entries

This is a direct fit for `GetNetworkInterfaceList`.

#### Event getters

`0x71000f427c` and `0x71000f4284` branch into:

- `FUN_71000f46a8(...)`
- `FUN_71000f46ec(...)`

Those simply export handles from:

- `self + 0x18`
- `self + 0x48`

respectively. That matches the documented two-event shape.

#### Ioctl family

The remaining six entries all route through the same backend:

- `FUN_71000f2028(...)`

That backend:

- returns `0x10025` if the object is not initialized
- dispatches via the callback at state offset `+200`
- returns `0x20225` if no callback is installed

The wrapper shapes line up with the documented ioctl family:

- `0x71000f428c` -> `FUN_71000f4730(...)`
  - no buffer pair
- `0x71000f4294` -> `FUN_71000f47c0(...)`
  - second buffer pair only
- `0x71000f42a4` -> `FUN_71000f4874(...)`
  - first buffer pair only
- `0x71000f42b4` -> `FUN_71000f4928(...)`
  - two buffer pairs
- `0x71000f42cc` -> `FUN_71000f49dc(...)`
  - incoming handle carrier
- `0x71000f4320` -> `FUN_71000f4a7c(...)`
  - extracts an outgoing handle carrier after the ioctl callback

### Practical implication

This is the strongest evidence so far that `wlan_main` exposes essentially the same
`ISfDriverService` surface as `eth_main`, but on a medium that is already live during ordinary
testing.

Two useful consequences follow:

1. `wlan_main` can be used to continue recovering the generic `anif` driver-service contract
   without waiting for a physical USB ethernet adapter.
2. The repeated `OpenNetworkInterface` pattern strengthens the current hypothesis that both
   `wlan_main` and `eth_main` sit on top of an existing registered-interface set rather than
   inventing arbitrary interfaces on demand.
3. A WLAN-only `net-probe` variant now has a concrete minimal path:
   - open `wlan:nd`
   - call creator cmd `0` `CreateDriverService`
   - on the returned object, call:
     - cmd `128` `GetDriverInfo`
     - cmd `129` `GetNetworkInterfaceList`
     - cmd `130` `GetStateChangedEvent`
     - cmd `131` `GetNetworkInterfaceListUpdatedEvent`
     - optionally cmd `0` `OpenNetworkInterface`

The command IDs in that last list are supported by Switchbrew's `wlan:nd` documentation, and the
local wrapper semantics match that documented ordering closely enough to treat it as the current
best probe surface.

## 2026-06-20: visible `wlan_main` network-interface layer is compact and ioctl-oriented

The first obvious `ISfNetworkInterfaceService`-related block in `wlan_main` does not look like the
rich 20+ entry wrapper table recovered in `eth_main`.

### Visible metadata block

The current candidate block is:

- `0x710016f060`

It points at a compact 6-entry run:

- `0x71000f4c8c`
- `0x71000f4c94`
- `0x71000f4ca4`
- `0x71000f4cb4`
- `0x71000f4ccc`
- `0x71000f4d20`

The adjacent type metadata still says `ISfNetworkInterfaceService`, so this is not a random
unrelated object. But the visible command surface is much narrower than the one already pinned in
`eth_main`.

### What the 6 visible entries do

Best current mapping:

- `0x71000f4c8c` -> no-buffer callback/ioctl entry
- `0x71000f4c94` -> second-buffer-pair callback/ioctl entry
- `0x71000f4ca4` -> first-buffer-pair callback/ioctl entry
- `0x71000f4cb4` -> two-buffer-pair callback/ioctl entry
- `0x71000f4ccc` -> handle-input callback/ioctl entry
- `0x71000f4d20` -> handle-output callback/ioctl entry

Recovered helper targets:

- `0x71000f4c8c` -> `FUN_71000f5350(...)`
- `0x71000f4c94` -> `FUN_71000f53ec(...)`
- `0x71000f4ca4` -> `FUN_71000f54ac(...)`
- `0x71000f4cb4` -> `FUN_71000f556c(...)`
- `0x71000f4ccc` -> `FUN_71000f562c(...)`
- `0x71000f4d20` -> `FUN_71000f56d8(...)`

Those all funnel into:

- `FUN_71000f3454(...)`

That backend:

- requires state byte `+0xac` to be initialized
- dispatches through a callback at offset `+0x110`
- passes callback context from offset `+0x140`
- returns `0x10025` if uninitialized
- returns `0x20225` if no callback is installed

The wrapper family chooses callback mode `2` or `3` depending on the flag at:

- `self + 0x48`

So the currently visible `wlan_main` network-interface object behaves more like a compact
callback/ioctl adapter than like the richer explicit method surface already recovered in `eth_main`.

### Supporting details

The constructor path used by `OpenNetworkInterface`:

- allocates a `0x68`-byte object in `FUN_71000f44d0(...)`
- initializes it through `FUN_71000f4d28(...)`

`FUN_71000f4d28(...)`:

- installs object metadata from the `0x710016f000` region
- stores the opened interface pair
- records a mode bit at object offset `+0x48`
- calls `FUN_71000f3b4c(...)`

`FUN_71000f3b4c(...)`:

- allocates a slot in a bitset-backed table
- stores the per-interface object pointer into that table
- returns `0x40625` when no slot is available

`FUN_71000f3cd0(...)` still serializes a full `0xb0`-byte interface record, just like the
driver-service list/open logic. So the compact visible surface is not because the underlying data is
small; it is because the exposed object layer currently visible in `wlan_main` is narrower.

### Current interpretation

The safest reading right now is:

1. `wlan_main` definitely has the same top-level `anif` driver-service model as `eth_main`.
2. The first clearly visible returned network-interface object in `wlan_main` exposes a compact
   callback/ioctl-oriented layer.
3. A second, richer network-interface wrapper table like the one in `eth_main` has not yet been
   found in the obvious metadata path.

That means the next `wlan_main` reverse target, if we continue deeper, should be one of:

- find whether a second richer `ISfNetworkInterfaceService` wrapper table exists elsewhere
- trace the callback target installed at `+0x110` / context `+0x140`
- compare this compact layer against how `bsdsockets_main` expects assigned interfaces to behave

### `usb_main`: explicit physical-interface inventory layer under `usb:hs`

The first `usb_main` pass is much more consistent with a real physical-inventory backend than with
an abstract virtual-interface API.

Confirmed public service registration:

- `FUN_710003544c`
  - registers `usb:hs`
  - registers `usb:hs:a`
  - starts two `nn.usb.HsIpcServer` workers

Separate PM/observer service path:

- `FUN_710005c3cc`
  - registers `usb:pm`
  - registers `usb:obsv`
  - starts `nn.usb.PmIpcServer`

Most useful recovered inventory path:

- `FUN_710003e570`

That function:

- iterates 8 fixed-size device slots at `param_2 + n*0x228`
- checks whether each slot has a non-null object at offset `+0x210`
- allocates a `0xb10`-byte object named `DeviceManager::ManagedInterface`
- initializes it and stores the resulting object pointer back at slot offset `+0x220`

This is strong evidence that `usb_main` owns a concrete managed-interface inventory layer for
physical USB interfaces. It fits the working hypothesis that `eth_main` does not synthesize
arbitrary devices itself; instead, it likely consumes already-discovered USB interface inventory
from lower layers like `usb:hs`.

Practical implication:

- if `eth:nd::OpenNetworkInterface(...)` only succeeds when a real USB ethernet interface has
  already been discovered, `usb_main` is the most likely upstream module to explain where those
  candidate records originate and how they are observed

### Priority update after this triage

Current reverse priority should be:

1. `wlan_main`, to recover the same `anif` driver-service command surface on a live medium
2. `usb_main`, to understand the physical USB interface discovery / observation path that likely
   feeds `eth_main`
3. `netConnect_main`, only for the proxy fallback path

This does not change the current `eth` expectation: the next high-value runtime probe is still on
a genuinely wired device. But it does add a second strong static path:

- `wlan_main` can likely teach us more about the generic `anif` service contract without waiting for
  a USB NIC
