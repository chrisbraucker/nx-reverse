# `ghidra-cli` Best Practices On This Host

This is the shortest set of habits that avoided the recurring `ghidra-cli`
failures on the ephemeral Linux `aarch64` setup.

## Environment

- use the wrappers installed by `.devcontainer/toolchain/install.sh`
- prefer the wrapper on `PATH`, not the raw cargo binary
- if the command is unexpectedly missing from `PATH`, re-source the shell:

```bash
source workspace/toolchain-env.sh
which ghidra-cli
which analyzeHeadless
```

The wrappers carry the required `JAVA_HOME`, `XDG_CONFIG_HOME`, and the
installed toolchain paths.

## Operational rules

- Prefer serialized `ghidra-cli` access per project/program.
- Do not fire multiple bridge-backed queries against the same project in
  parallel.
- After import, always verify the actual saved program name with
  `ghidra-cli program list --project <name>`.

Reason:

- the CLI bridge has been reliable enough for single-step use, but not robust
  enough to treat as a parallel query service
- `import --program <alias>` may still save the program under the original file
  basename instead of the requested alias

## Known import quirk

For `sm.kip1`, the command:

```bash
ghidra-cli import workspace/20.5.0/pkg/main/pkg2/ini/sm.kip1 \
  --project sm-test --program sm_20_5_0
```

imported the file but then errored because the saved program name was still
`sm.kip1`.

Practical rule:

1. import
2. run `ghidra-cli program list --project <project>`
3. use the discovered program name for every later command

## KIP-specific limitation

KIP1 import through the Switch loader works, but headless analysis is still not
fully trustworthy here.

Symptoms seen on this host:

- import succeeds
- memory map looks correct
- strings can be queried
- `ghidra-cli analyze` reports success
- function metadata still stays effectively empty, for example `function_count:
  1`
- direct disassembly requests may fail with "address may be data or unanalyzed
  code"

## Reliable fallback for KIPs

When a KIP imports but does not analyze into usable code units:

1. inspect section layout with:

```bash
ghidra-cli memory map --project <project> --program <program>
```

2. extract strings with:

```bash
ghidra-cli strings list --project <project> --program <program>
```

3. parse and decompress the KIP sections directly
4. disassemble the decompressed RX image with `aarch64-none-elf-objdump`

That fallback was the reliable path for `sm.kip1`.

## When to use the GUI

Prefer the GUI when one of these is true:

- the CLI keeps reporting analyzed state without meaningful function recovery
- you need renaming, structure work, or xref chasing
- you need to confirm whether the loader created code units correctly

Use the CLI first for:

- imports
- strings
- memory maps
- quick decompile calls on already well-behaved NSO binaries

Use the GUI or raw disassembly fallback for:

- KIP1s that import cleanly but do not analyze cleanly

## Short recipe

```bash
ghidra-cli import <binary> --project <project>
ghidra-cli program list --project <project>
ghidra-cli memory map --project <project> --program <actual-name>
ghidra-cli strings list --project <project> --program <actual-name>
ghidra-cli analyze --project <project> --program <actual-name>
```

If analysis still looks implausibly thin, stop trusting the CLI analysis result
and switch to manual KIP decompression plus `objdump`.
