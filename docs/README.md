# Documentation

This directory documents the public continuation surface for `net-probe`.
The contracts describe only service targets that the probe intercepts or calls directly.

## Start Here

- [`contracts/README.md`](contracts/README.md) explains the contract data and its JSON Schemas.
- [`contracts/targets.json`](contracts/targets.json) lists every current target and its access mode.
- [`contracts/trace-format.md`](contracts/trace-format.md) describes the versioned MITM trace format.
- [`services/listener-lifecycle.md`](services/listener-lifecycle.md) defines the listener-lifecycle rules shared by passive targets.
- [`services/`](services/) explains the recovered service flows and their operational constraints.

The JSON files under `contracts/services/` are the field-level contract records.
Their companion Markdown files describe state, lifecycle, and confidence without duplicating field tables.

All contracts apply to Horizon firmware `20.5.0` unless the file says otherwise.
