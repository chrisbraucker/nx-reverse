# nx-reverse

A reversing setup to improve understanding of the IPC and network stack of NX.

Workspace split out of `wireguard-nx` for NX reversing and probe
development.
The end goal is to add wireguard-backed VPN functionality to NX that is able to intercept as many traffic paths as possible.
So far we have identified bsd:* for socket and unencrypted traffic like HTTP, and ssl:* for TLS-encrypted TCP traffic.
The browser applet for some reason seems to not use that path, but this could be due to the denylist preventing some core services like qlaunch from being MITM'd.

Current contents:

- `docs/`: notes, plans, methods, and setup docs.
- `net-probe/`: sysmodule-based probe and MITM tracer.
- `tools/`: helper scripts for syncing logs and decoding crashes.
- `traces/`: runtime-trace material.
- `.devcontainer/`: build-time reversing environment, kept compatible with the
  original `wireguard-nx` libnx/devkitPro runtime, including the baked-in
  toolchain installer under `.devcontainer/toolchain/`.

Notes:

- `net-probe/lib/Atmosphere-libs` is intended to remain a submodule against
  `git@github.com:chrisbraucker/Atmosphere-libs.git`.
- `workspace/` carries local-only firmware inputs, Ghidra projects, config,
  caches, and other persistent reversing state.
- generated build artifacts and local firmware inputs are intentionally kept
  out of the publishable tree.
