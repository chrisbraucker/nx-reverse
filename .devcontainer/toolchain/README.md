# Host Toolchain Setup

This is the shortest reproducible path for the `aarch64` setup we used.

## Devcontainer

The repo now ships its own `.devcontainer/` based on the `wireguard-nx` baseline, but with the reversing toolchain baked into the image.

Included in the container image:

- everything from the original `wireguard-nx` devcontainer baseline
- Go `1.26.5`
- `jq`
- Ghidra `12.1.2`
- Temurin JDK `21.0.12+8`
- `ghidra-cli` built from source
- Adubbz Switch loader patched for Ghidra `12`
- `linux_arm_64` decompiler natives

Expected workflow:

```bash
cd /workspaces/<repo-dir>
```

Then reopen in the devcontainer rooted at `nx-reversing.git`.

Inside the devcontainer, the reversing toolchain is already installed.
Do not rerun `.devcontainer/toolchain/install.sh` there unless you are intentionally rebuilding the tool payload.

Container layout:

- tool binaries live under `/opt/toolchain`
- Go lives at `/usr/local/go` and is available on `PATH`
- devkitA64 tools, including `aarch64-none-elf-gdb`, are available on `PATH`
- project-local persistent state lives under `workspace/`
- Ghidra projects live under `workspace/ghidra/projects`
- Ghidra and `ghidra-cli` config are persisted under `workspace/config/`

## Install

From the repo root:

```bash
export WORKSPACE_FOLDER="$PWD"
sudo bash .devcontainer/toolchain/install.sh
bash .devcontainer/toolchain/init-workspace.sh
source workspace/toolchain-env.sh
```

If `/opt/toolchain` is not desirable or not writable, override the prefix:

```bash
NX_REVERSE_TOOLCHAIN_PREFIX="$PWD/workspace/toolchain" \
bash .devcontainer/toolchain/install.sh
bash .devcontainer/toolchain/init-workspace.sh
source workspace/toolchain-env.sh
```

The default install shape is:

- `/opt/toolchain/ghidra`
- `/opt/toolchain/jdk`
- `/opt/toolchain/cargo`
- `/opt/toolchain/rustup`
- `/opt/toolchain/bin`

Persistent workspace state stays in-repo under `workspace/` and is initialized by `.devcontainer/toolchain/init-workspace.sh` or the devcontainer `postStart` hook:

- `workspace/ghidra/projects`
- `workspace/config`
- `workspace/toolchain-env.sh`

Installed components:

- Go `1.26.5`
- Ghidra `12.1.2`
- Temurin JDK `21.0.12+8`
- Rust `1.97.1`
- `ghidra-cli` from source
- Adubbz Switch loader, built against local Ghidra
- `linux_arm_64` native `decompile` and `sleigh` binaries under Ghidra's `build/os/` tree

It also applies the local compatibility patches in [ghidra-cli-g12.patch](/workspaces/nx-reversing.git/.devcontainer/toolchain/patches/ghidra-cli-g12.patch:1) and [ghidra-switch-loader-g12.patch](/workspaces/nx-reversing.git/.devcontainer/toolchain/patches/ghidra-switch-loader-g12.patch:1).

The devkitA64 base image, Go archive, Temurin archive, Ghidra archive, Rust bootstrapper, Rust toolchain version, and source revisions are pinned by the container recipe.

The image builds the Ghidra toolchain in a dedicated stage.
Only changes to `toolchain/install.sh` or `toolchain/patches/` invalidate that expensive stage.
Go is installed afterwards in the final development stage.

## Use

Use the wrappers from the installed toolchain `bin/` directory, not the raw cargo binary.
The generated `workspace/toolchain-env.sh` file exports `JAVA_HOME`, `CARGO_HOME`, `RUSTUP_HOME`, `XDG_CONFIG_HOME`, and the required `PATH` entries.

Quick import:

```bash
ghidra-cli import "$WORKSPACE_FOLDER"/workspace/20.5.0/exefs/bsdsockets_main \
  --project switch-test --program bsdsockets_main
```

List saved programs:

```bash
ghidra-cli program list --project switch-test
```

Analyze a saved program:

```bash
ghidra-cli analyze --project switch-test --program bsdsockets_main
ghidra-cli function list --project switch-test --program bsdsockets_main --limit 5
ghidra-cli function decompile 7100000100 --project switch-test --program bsdsockets_main
```

## Known Limits

- Official Ghidra `12.1.2` on Linux `aarch64` does not ship the native decompiler binary here.
  The setup script compensates by building the `Decompiler` module's `linux_arm_64` natives locally.
- The `ghidra-cli` patch deliberately starts a project-scoped bridge and imports/opens programs over TCP.
  This avoids the Ghidra 12 issues we hit with hidden script paths and unreliable `-process` / `-import` startup behavior.
- `ghidra-cli analyze` is patched to stop the bridge, run `analyzeHeadless -process <program>`, then reopen the program through the bridge.
  That path is deliberate; bridge-side analysis was unreliable on this host.
- The native build is intentionally scoped to `Ghidra/Features/Decompiler` with `buildNatives_linux_arm_64`.
  That is enough for working decompilation on this host without paying for a full cross-module native build.
