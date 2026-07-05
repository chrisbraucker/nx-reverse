# Local Workspace

This directory is intentionally local-only.

Use it for all state that is useful during reversing for data that everyone needs to source themselves.

- extracted firmware binaries and strings
- decrypted package content and keys
- Ghidra projects and user config
- raw traces, crash dumps, and private notes
- disposable caches created by local toolchain setup

Suggested layout:

```text
workspace/
  $FIRMWARE_VERSION/
    exefs/
    modules/
    pkg/
    strings/
    keys/
  ghidra/
    projects/
  config/
  cache/
```
