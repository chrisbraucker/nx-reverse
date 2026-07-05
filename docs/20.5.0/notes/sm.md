# 20.5.0 `sm.kip1` Notes

## Scope

This note covers the first static pass over:

- `workspace/20.5.0/pkg/main/pkg2/ini/sm.kip1`

The immediate goal was to understand the service-manager side of Atmosphere MITM
registration, because recent probe crashes implicated title ID
`0100000000000004` and a failed `RegisterMitmServer(nifm:u)` path.

## Practical status

`ghidra-cli` imports `sm.kip1` through the Switch loader, but CLI-side analysis
is still too thin to trust on its own:

- import works
- image base resolves to `0x7100000000`
- `analyze` still reports effectively no function recovery

For this pass, the reliable workflow was:

1. parse the KIP header directly
2. decompress RX/RO/RW manually
3. disassemble RX with `aarch64-none-elf-objdump`
4. use the recovered strings and callsites to identify service-manager helpers

Scratch artifacts from this pass live in:

- `workspace/20.5.0/query/sm.rx.bin`
- `workspace/20.5.0/query/sm.ro.bin`
- `workspace/20.5.0/query/sm.rx.dis`

## KIP layout

Header values recovered from `sm.kip1`:

- program ID: `0x0100000000000004`
- module name: `sm`
- version: `0x1`
- priority: `27`
- ideal core: `3`
- flags: `0x7f`
- stack size: `0x1000`

Section layout:

- RX
  - address: `0x00000000`
  - size: `0xA994`
  - compressed size: `0x6D70`
- RO
  - address: `0x0000B000`
  - size: `0x2DE4`
  - compressed size: `0x1724`
- RW
  - address: `0x0000E000`
  - size: `0x6F40`
  - compressed size: `0x0EBC`
- BSS
  - address: `0x00015000`
  - size: `0x10000`

All three mapped sections are compressed in the KIP image.

## Strings worth keeping in mind

Useful RO strings recovered from the decompressed image:

- `sm.nss`
- `sm:m`
- `nn.sm.MainThread`
- `nn.sm.DispatcherThread`

These line up with the expected service-manager bootstrap and manager/user split.

## Atmosphere MITM command context

The passive MITM probe currently reaches `sm` through Atmosphere-added TIPC
commands, not purely through stock Nintendo `sm:` behavior.

From the vendored `libstratosphere` command definitions:

- `65000` `AtmosphereRegisterProcess`
- `65001` `AtmosphereUnregisterProcess`
- `65003` `AtmosphereAcknowledgeMitmSession`
- `65004` `AtmosphereHasMitm`
- `65005` `AtmosphereWaitMitm`
- `65006` `AtmosphereDeclareFutureMitm`
- `65007` `AtmosphereClearFutureMitm`
- `65100` `AtmosphereGetServiceRecord`
- `65101` `AtmosphereListServiceRecords`

For the current probe failures, the important point is that stock `sm`
registration logic still appears to own the backing service-name tables and the
duplicate/ownership error paths.

## Candidate functions

The addresses below are from the RX image mapped at base `0x7100000000`.

### `0x7100000a80`

Bootstrap helper that sets up at least two service-facing objects.

Observed behavior:

- initializes objects rooted off a global state block
- references the strings:
  - `sm:`
  - `sm:m`
- calls helper paths that look consistent with service registration/open setup

Current interpretation:

- early initialization for the public `sm:` and manager-facing `sm:m` services

### `0x71000013f0`

Main bootstrap / thread-launch path.

Observed behavior:

- references `nn.sm.MainThread`
- calls `0x7100000a80`
- later references `nn.sm.DispatcherThread`

Current interpretation:

- main-thread startup that initializes the service-manager state and spawns the
  dispatcher thread

### `0x7100001610`

Strong candidate for a stock service-table insertion helper.

Observed behavior:

- validates the 8-byte service name byte-by-byte
- scans a table at `0x7100019000`
- table length looks like `0x180` entries
- entry size looks like `0x18`
- stores service name, owner/context pointer, metadata, and an active byte

Notable return codes visible in this helper family:

- `0xA15`
- `0xC15`

Current interpretation:

- helper used by higher-level registration logic to install or validate
  service-table records

### `0x71000020b0`

Best current candidate for stock `RegisterService`.

Important callsite shape:

- `x0 = out handle ptr`
- `x1 = caller/session/process context`
- `x2 = service name`
- `w3 = max sessions`
- `w4 = is_light`

That matches the public `sm:` registration shape closely.

Observed behavior:

- validates the 8-byte service name
- in one path seeds `w0 = 0x815`
- scans the service table at `0x7100019000`
  - `0x180` entries
  - `0x18` bytes each
- in another path seeds `w0 = 0x415`
- scans a second table at `0x710001b410`
  - `0x45` entries
  - `0x218` bytes each
- one branch calls helper `0x7100001ac0`
- failure paths also surface:
  - `0x1015`
  - `0xC15`

Most important sub-block:

- around `0x71000021fc`
  - `w0` is initialized to `0x815`
  - the code walks the main service table and compares service names

Current interpretation:

- this is the duplicate-registration / already-present path that explains the
  runtime `RegisterMitmServer(nifm:u) failed rc=0x00000815` result

This does not yet prove whether the duplicate owner is:

- Nintendo `nifm`
- a prior probe instance
- or an Atmosphere MITM bookkeeping artifact

But it does confirm that `0x815` is a service-table registration result from
inside `sm`, not an unrelated higher-layer wrapper code.

### `0x7100002370`

Best current candidate for stock `UnregisterService`.

Important callsite shape:

- `x0 = caller/session/process context`
- `x1 = service name`

Observed behavior:

- validates the 8-byte service name
- checks the manager table at `0x710001b410`
- checks the service table at `0x7100019000`
- on match, clears and tears down the entry

Visible failure codes:

- `0x415`
- `0xE15`
- `0x1015`
- `0xC15`

Current interpretation:

- normal service removal / cleanup path

## What this means for the probe work

The current `sm` pass gives us one firm conclusion already:

- `0x815` is not speculative anymore; it is directly embedded in the
  service-registration search path that scans the main service table

That is enough to tighten the next runtime checks:

1. confirm whether `nifm:u` is already present in the stock service table when
   the probe tries to MITM-register
2. confirm whether a stale MITM/future-MITM state survives probe shutdown
3. distinguish normal duplicate-registration from ownership/manager-table
   failures in the `0x415` / `0x1015` family

## Next static step

The next useful `sm` work items are:

1. label the dispatcher command table around the `0x710000107c` call tree
2. recover the helper at `0x7100001ac0`
3. map the exact structure layout of:
   - the main table at `0x7100019000`
   - the manager/aux table at `0x710001b410`
4. line up those paths with Atmosphere's MITM registration flow to see where
   future-MITM or MITM-owner state is layered on top of stock records
