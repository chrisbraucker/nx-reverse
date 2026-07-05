# 20.5.0 `pkg2` Inventory and First Kernel Pass

## 2026-06-21: first static inventory

Available decrypted package data:

- `workspace/20.5.0/pkg/main/pkg2/kernel.bin`
- `workspace/20.5.0/pkg/main/pkg2/ini/*.kip1`
- `workspace/20.5.0/pkg/safe/pkg2/kernel.bin`
- `workspace/20.5.0/pkg/safe/pkg2/ini/*.kip1`

Main `pkg2` KIPs present:

- `FS.kip1`
- `Loader.kip1`
- `NCM.kip1`
- `ProcessMana.kip1`
- `boot.kip1`
- `sm.kip1`
- `spl.kip1`

Safe `pkg2` KIPs present:

- `Bus.kip1`
- `FS.kip1`
- `Loader.kip1`
- `NCM.kip1`
- `PCV.kip1`
- `ProcessMana.kip1`
- `boot.kip1`
- `psc.kip1`
- `sm.kip1`
- `spl.kip1`

## Important first conclusion

There is no network-facing sysmodule in `pkg2` itself.

Notably absent from both `main` and `safe` `pkg2`:

- `nifm`
- `wlan`
- `eth`
- `bsdsockets`

That matters because it narrows the kernel/KIP question:

- if we want a lower-than-`wlan` insertion point, it is unlikely to be "another
  hidden networking KIP in `pkg2`"
- it is more likely to be:
  - a generic kernel object/memory/event boundary
  - a Mesosphere or custom-KIP patch
  - or a userland service boundary after all

## Raw kernel observations

### Main vs safe kernel

The kernels are different binaries:

- main `kernel.bin`
  - size: `2.8M`
  - SHA-256: `88b9e9af866696479403a8a860111e04c6621acad5547d0b8cb0f3a04097a72a`
- safe `kernel.bin`
  - size: `3.6M`
  - SHA-256: `4bfc8f59fe221afc0c9d23c45a10cdd527754e990dff8b9b8ac27365a2939e3a`

They are not just trivially padded copies. The first visible byte difference
appears well before EOF and the safe kernel is materially larger.

### Kernel format

Both kernels look like raw AArch64 binaries, not ELF objects:

- first word: `0x1401d400`
- objdump decodes that as:
  - `b 0x75000`

The first few decoded instructions are identical between `main` and `safe`:

- initial branch from offset `0x0` to `0x75000`
- a small low-address stub region around `0x800`

This is enough to justify importing `kernel.bin` into Ghidra as a raw
`AARCH64:LE:64` binary for the next pass.

### Early privileged register use

A quick disassembly scan of the main kernel shows immediate privileged-register
activity, including:

- `mrs ... daif`
- `msr daif, ...`
- `mrs ... ttbr0_el1`
- `mrs ... ttbr1_el1`
- `mrs ... tcr_el1`

This is low-level kernel bring-up / MMU / interrupt control territory, not
network-specific logic.

## KIP inventory observations

Shared between `main` and `safe` with identical hashes:

- `Loader.kip1`
- `sm.kip1`
- `spl.kip1`

Present in both but different:

- `FS.kip1`
- `NCM.kip1`
- `ProcessMana.kip1`
- `boot.kip1`

Safe-only extras:

- `Bus.kip1`
- `PCV.kip1`
- `psc.kip1`

Implication:

- `sm`, `Loader`, and `spl` are stable enough between `main` and `safe` that one
  reverse pass can likely cover both.
- `ProcessMana` remains worth prioritizing for process/service context.
- none of these names suggest a hidden network implementation path on their own.

## Current Ghidra entrypoints

The local `ghidra-cli` / Switch-loader setup is already useful for the KIPs:

- `sm.kip1` imports successfully as:
  - executable format: `Nintendo Switch Binary`
  - language: `AARCH64/little/64/v8A`
  - image base: `0x7100000000`
- `ProcessMana.kip1` imports the same way

But the current behavior is uneven:

- `kernel.bin` does not auto-import through the loader path
  - current result: `Import failed: No load spec found`
- `ghidra-cli analyze` on imported KIPs currently leaves suspiciously thin
  metadata behind
  - function count still reports as `1`
  - program `analyzed` state does not become trustworthy

Practical takeaway:

- KIP1 import is working well enough to begin labeled manual reversing
- `kernel.bin` needs a raw-binary import path in Ghidra
- for now, GUI/manual analysis remains more trustworthy than relying on the CLI
  analysis status for KIPs

## Current interpretation for the VPN effort

The first `pkg2` pass shifts the kernel hypothesis from "find the networking
stack in package2" to "find the generic kernel primitives that userland network
sysmodules rely on".

That means the kernel-focused checklist should now prioritize:

1. `kernel.bin`
   - exception/SVC dispatch
   - IPC and handle object flow
   - shared-memory / transfer-memory support
   - event/wait primitives
2. `sm.kip1`
   - service registration and MITM context
3. `ProcessMana.kip1`
   - process metadata and permission context that may matter for service access
4. `Loader.kip1`
   - process/module launch context only where it helps map the above

## Next static step

Import and label:

- `pkg/main/pkg2/kernel.bin`
- `pkg/main/pkg2/ini/sm.kip1`
- `pkg/main/pkg2/ini/ProcessMana.kip1`
- `pkg/main/pkg2/ini/Loader.kip1`

Then recover:

- a first exception-vector / SVC table map from `kernel.bin`
- the service-manager object model in `sm.kip1`
- the process capability / context structures in `ProcessMana.kip1`

That is the lowest-friction path to determine whether a synthetic uplink could
be attached below the current `wlan` / `eth` userland services.

## 2026-06-23: `sm.kip1` service-manager pass

A first real `sm.kip1` pass is now captured separately in
[sm.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/sm.md:1).

Most important current result:

- the registration path candidate at `0x71000020b0` hardcodes `0x815` before
  scanning the main service table at `0x7100019000`

That strongly supports the current runtime interpretation that
`RegisterMitmServer(nifm:u) failed rc=0x00000815` is a genuine `sm`
service-table registration result rather than a higher-level tracing artifact.

## 2026-06-28: first raw `kernel.bin` exception map

The raw-binary path is now useful enough to record the first concrete kernel
control-flow anchors even without a clean Ghidra import.

Tools used:

- `aarch64-none-elf-objdump -D -b binary -m aarch64`
- focused windows around the early branch target and later exception-heavy
  regions

### Early boot / bring-up anchor

The first word still branches from offset `0x0` to `0x75000`.

The block at `0x75000` is now clearly a privileged bring-up path, not just a
branch landing pad:

- masks interrupts with `msr daifset, #0xf`
- checks `CurrentEL`
- invalidates the instruction cache
- calls helper code at `0x7535c` and `0x7521c`
- later writes:
  - `TTBR0_EL1`
  - `TTBR1_EL1`
  - `TCR_EL1`
  - `MAIR_EL1`
- finally installs `SCTLR_EL1` and branches into the next stage

Practical conclusion:

- the `0x75000` region is the main EL1/MMU/cache bring-up path for this kernel
- it is generic platform-init code, not a hidden network stack

### Vector-base installation anchors

Two useful `VBAR_EL1` install sites are now visible.

At `0x64a10`:

- loads a vector base pointer from data near `0x78000`
- writes it to `VBAR_EL1`
- clears `TPIDR_EL1`
- clears `CNTV_CVAL_EL0`
- returns

At `0x2ba000`:

- sets up a temporary stack and calls helper code
- clears `TPIDR_EL1`
- clears `CNTV_CVAL_EL0`
- installs `VBAR_EL1 = 0x2ba800`
- performs address translation with `AT S1E1R` / `PAR_EL1`
- then branches through the translated target

Current interpretation:

- `0x64a10` looks like a reusable EL1 "install current vector state" helper
- `0x2ba000` looks more like a later-stage relocation / trampoline path that
  temporarily points EL1 at a local vector table while transferring control

### First sync-exception split

The block at `0x62424` is now the first clear sync-exception gate worth
tracking.

Recovered shape:

- reads `ESR_EL1`
- branches on exception class (`ESR_EL1 >> 26`)
- has fast paths for a handful of classes
- otherwise falls back to a full register-save path and helper call at
  `0x66e80`

Important branch targets:

- `EC = 0x11` -> `0x63cd4`
- `EC = 0x15` -> `0x63954`
- `EC = 0x07` -> `0x626f8`
- `EC = 0x20` / `0x24`
  - special handling for `ISS & 0x3f == 0x30`
  - performs targeted `TLBI`
  - otherwise falls through to the heavier save/dispatch path

The exact Arm architectural labels are not hard-confirmed in this note, but
the `0x15` branch is the strongest current AArch64 SVC-path candidate.

### SVC-dispatch candidate

The path at `0x63954` now looks like the most useful first SVC-dispatch anchor.

Recovered behavior:

- saves general context and EL1 return state
- reads `ESR_EL1 & 0xff`
- bounds-checks it against `0xc0`
- consults a bitset-like permission/filter blob via stack-relative table logic
- indexes a function table rooted near `0x77950`
- temporarily unmasks interrupts with `msr daifclr, #0x2`
- calls the selected handler
- restores context and returns with `eret`

There is a sibling-looking path at `0x63cd4` with the same broad structure but
a different dispatch table rooted near `0x77350`.

Practical conclusion:

- the current kernel evidence still points toward generic syscall/exception
  plumbing, not a dedicated networking subsystem in `pkg2`
- this is still valuable, because it identifies where deeper runtime tracing or
  Mesosphere comparison should start if we later need to understand:
  - IPC object flow
  - memory mapping primitives
  - event/wait behavior

## 2026-06-28: Mesosphere-guided SVC naming

The vendored Atmosphere tree in this repo is now enough to tighten the naming
of the first two kernel exception branches without claiming an exact
address-for-address source match.

Relevant local source:

- `sysmodule/lib/Atmosphere-libs/libmesosphere/source/arch/arm64/svc/kern_svc_handlers_asm.s`

That file has two explicit entry paths:

- `ams::kern::arch::arm64::SvcHandler64`
- `ams::kern::arch::arm64::SvcHandler32`

Both have the same broad structure already recovered from raw `kernel.bin`:

- read `ESR_EL1`
- extract the low SVC number byte
- bounds-check against the SVC table size
- check per-thread SVC permission bits
- load from an SVC handler table
- clear interrupt mask bits with `msr daifclr, #2`
- call the resolved handler
- loop through DPC handling before returning
- fall back into a generic exception path when the SVC is invalid

That makes the earlier raw branch labels materially stronger:

- from the sync-exception split at `0x62424`
  - `EC = 0x15 -> 0x63954`
    - best label: 64-bit SVC handler path
  - `EC = 0x11 -> 0x63cd4`
    - best label: 32-bit compatibility SVC handler path

This is still guidance, not hard proof:

- the Nintendo kernel at 20.5.0 is not being claimed to be source-identical to
  vendored Mesosphere
- the claim is only that the recovered control-flow shape and the exception
  class split line up closely enough that these names are now the best working
  labels

Practical impact for the `pkg2` checklist:

- `0x63954` is now the best first anchor for later syscall-side tracing or hook
  experiments against 64-bit userland
- `0x63cd4` is the sibling worth keeping for 32-bit compatibility paths, but
  is probably secondary for current homebrew/sysmodule work

## 2026-06-28: `ProcessMana.kip1` first orientation

`ProcessMana.kip1` imports cleanly into the local `pkg2-test` project, but the
first CLI pass suggests it will not yield much through strings alone.

Observed so far:

- memory map is sane
  - `.text` at `0x7100000000`
  - `.rodata.1` present
  - normal `.data` / `.bss` layout present
- string surface is nearly empty from the CLI
  - only `pm.nss` is immediately surfaced

Practical conclusion:

- `ProcessMana.kip1` should be treated as a function/disassembly-driven target,
  not a strings-driven one
- the next useful pass there is to locate process creation / capability /
  service-access enforcement logic directly, not to mine more string tables

## 2026-06-28: `ProcessMana.kip1` uncompressed service surface

A raw KIP pass with `hactool` is now enough to turn `ProcessMana` into a much
more concrete target, even without deep function recovery yet.

Command used:

```bash
hactool -t kip1 \
  --json=.codex-tmp/processmana/ProcessMana.json \
  --uncompressed=.codex-tmp/processmana/ProcessMana_uncompressed.kip1 \
  workspace/20.5.0/pkg/main/pkg2/ini/ProcessMana.kip1
```

Recovered KIP header from the uncompressed image:

- title ID
  - `0x0100000000000003`
- name
  - `ProcessMana`
- process category
  - `Kernel Built-In`
- main thread priority
  - `49`
- default core
  - `3`
- handle table size
  - `128`

Recovered section layout:

- `.text`
  - address `0x00000000`
  - size `0x20924`
- `.rodata`
  - address `0x00021000`
  - size `0x8fe8`
- `.rwdata`
  - address `0x0002a000`
  - size `0x2108`
- `.bss`
  - address `0x0002d000`
  - size `0xd000`

The declared SVC set is also informative. `ProcessMana` is not a tiny passive
helper. It has enough privilege to:

- create and start threads
- connect to named ports and issue sync IPC
- create and accept sessions
- create events
- start and terminate processes
- query process info
- create and tune resource limits

That alone makes it a strong candidate for later service-access and
process-lifecycle reversing.

### Useful RO strings from the uncompressed image

After extracting the uncompressed RX/RO/RW blobs from the KIP container, the RO
surface yields a much better orientation set than the imported Ghidra strings
did.

Service and dependency strings:

- `pm.nss`
- `pm:shell`
- `pm:dmnt`
- `pm:bm`
- `pm:info`
- `ldr:pm`
- `fsp-pr`
- `sm:m`
- `spl:`

Thread and process-tracking strings:

- `MainThread`
- `nn.ProcessMana.MainThread`
- `nn.pm.ProcessTrack`
- `RegisterProgram`

Practical interpretation:

- `ProcessMana` is not just abstract "kernel process logic"
- it is a concrete userspace PM hub that:
  - hosts the public `pm:*` services
  - coordinates with loader through `ldr:pm`
  - touches program-registration or content paths through `fsp-pr`
  - talks to the service manager through `sm:m`
  - depends on secure-platform services through `spl:`

This is useful for the longer MITM / capability path because it narrows where
service access and launch policy are likely to be stitched together:

- `sm.kip1`
  - service registration and MITM tables
- `ProcessMana.kip1`
  - process launch, tracking, and PM-side policy glue
- `Loader.kip1`
  - executable loading and program bring-up

Current limitation:

- this is still orientation, not deep control-flow recovery
- no strong function labels are being claimed from the KIP import itself yet

### KIP orientation update

The KIP import status is now:

- `sm.kip1`
  - imported
  - section map usable
- `ProcessMana.kip1`
  - imported
  - section map usable
  - only obvious exported string so far: `"pm.nss"`
- `Loader.kip1`
  - imported
  - section map usable
  - only obvious exported string so far: `"ldr.nss"`

CLI analysis on these KIPs is still too thin to trust for automated function
recovery, but the imports are sufficient for:

- memory-map inspection
- string extraction
- targeted manual disassembly windows

## Current next kernel step

With the exception anchors now identified, the next useful kernel/KIP pass
should be:

1. compare the `0x63954` / `0x63cd4` dispatch paths against Mesosphere or other
   public kernel references for naming only
2. inspect `ProcessMana.kip1` specifically for process capability / context
   structures that govern service access
3. continue `sm.kip1` only where it clarifies MITM registration state

## 2026-06-28: `Loader.kip1` and `ProcessMana.kip1` now split cleanly by role

The next pass below userland is now concrete enough to stop talking about
`Loader` and `ProcessMana` as generic "core KIPs".

### `Loader.kip1`: process image construction and NPDM-facing code path

`Loader.kip1` uncompresses cleanly with `hactool`:

```bash
hactool -t kip1 \
  --json=.codex-tmp/loader/Loader.json \
  --uncompressed=.codex-tmp/loader/Loader_uncompressed.kip1 \
  workspace/20.5.0/pkg/main/pkg2/ini/Loader.kip1
```

Recovered header / capability facts:

- title ID
  - `0x0100000000000001`
- name
  - `Loader`
- process category
  - `Kernel Built-In`
- main thread priority
  - `49`
- default core
  - `3`
- handle table size
  - `128`

The important part is the SVC set.

`Loader` has:

- `svcCreateProcess`
- `svcMapProcessMemory`
- `svcUnmapProcessMemory`
- `svcSetProcessMemoryPermission`

But it does not have the PM-side lifecycle calls such as:

- `svcStartProcess`
- `svcTerminateProcess`
- resource-limit management

That split already points to `Loader` as the image-construction side of program
bring-up, not the supervisor.

Useful string anchors from the uncompressed image:

- `ldr.nss`
- `ldr:pm`
- `ldr:shel`
- `ldr:dmnt`
- `fsp-ldr`
- `nn.Loader.MainThread`
- `MountCode`
- `OpenFile`
- `code:/main`
- `code:/main.npdm`
- `code:/sdk`
- `code:/subsdk%d`
- `code:/rtld`

Practical interpretation:

- `Loader` exposes the expected `ldr:*` service family
- it talks to `fsp-ldr`, not directly to any network service
- it mounts code filesystems and explicitly opens `code:/main.npdm`
- it also carries the usual RTLD / subsdk / relocation plumbing for Nintendo's
  userspace image format

For the networking effort, the important conclusion is not "Loader does
networking". It is:

- if a later KIP-level strategy needs to alter service-access or capability
  policy for a networking sysmodule, `Loader` is the first realistic place to
  look because it is the KIP that visibly touches NPDM-backed program images
  and owns `svcCreateProcess`

### `ProcessMana.kip1`: lifecycle supervision, not image construction

`ProcessMana` now looks complementary rather than overlapping.

Key public-facing strings already recovered from the uncompressed image:

- `RegisterProgram`
- `nn.pm.ProcessTrack`
- `pm:shell`
- `pm:dmnt`
- `pm:bm`
- `pm:info`
- `fsp-pr`
- `ldr:pm`
- `sm:m`

Its SVC set differs from `Loader` in the exact way the service names suggest.

`ProcessMana` has:

- `svcStartProcess`
- `svcTerminateProcess`
- `svcGetProcessInfo`
- `svcCreateResourceLimit`
- `svcSetResourceLimitLimitValue`
- `svcGetResourceLimitLimitValue`
- `svcGetResourceLimitCurrentValue`

But it does not have:

- `svcCreateProcess`
- `svcMapProcessMemory`
- `svcSetProcessMemoryPermission`

Practical interpretation:

- `ProcessMana` supervises process lifecycle after construction
- `Loader` constructs the process image and likely materializes NPDM-derived
  policy and capabilities
- `ProcessMana` then tracks, starts, limits, and terminates the resulting
  process while coordinating with:
  - `ldr:pm`
  - `fsp-pr`
  - `sm:m`

### Why this matters for Horizon networking

This KIP split narrows the realistic kernel-adjacent intervention points.

If the future goal becomes:

- change service access for a networking sysmodule
- widen capability policy
- or understand how a modified KIP could bless a deeper networking hook

then the current best order is:

1. `sm.kip1`
   - service registration and MITM/service-table behavior
2. `Loader.kip1`
   - program image construction and NPDM/capability ingestion
3. `ProcessMana.kip1`
   - process lifecycle, limits, and PM-side policy glue

This also strengthens a negative conclusion:

- none of these KIPs show signs of hiding a separate network stack
- what they do show is the policy and launch machinery around userland
  networking sysmodules

That is useful because it tells us where a custom-KIP strategy would have to
patch the platform:

- not by finding a secret `nifm`/`wlan` implementation in `pkg2`
- but by changing service policy, process policy, or generic kernel plumbing
