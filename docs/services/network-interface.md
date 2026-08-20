# Network-Interface Probe Procedures

The field-level contract is [`../contracts/services/network-interface.json`](../contracts/services/network-interface.json).
Read [`../contracts/README.md`](../contracts/README.md) before using its workflows.

## Direct Probe

Create a driver service through `eth:nd` or `wlan:nd` and use a returned interface record to open one candidate interface.
Create a separate `bsd:nu` user service only when the candidate is ready for assignment.

Use a fresh candidate interface session for every targeted attempt.
Do not reuse a partially exercised candidate after a failed preflight.

## Assign Preflight

Call candidate-interface commands `0x82`, `0x83`, `5`, and `0x80` in that order.
Command `0x80` returns the 176-byte descriptor consumed by `bsd:nu::Assign`.
Byte `0xa8` must equal `1` before assignment can succeed on firmware `20.5.0`.

Call command `6` on the failed preflight path.
Do not broaden connected-WLAN calls while the service behavior remains unproven.

A failed assignment can report a valid handle in an ineligible interface state.
