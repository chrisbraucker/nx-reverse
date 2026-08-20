# Contract Format

The files in this directory are machine-readable descriptions of the Horizon service interactions that `net-probe` intercepts or calls directly.
They are not general Horizon API references.

`targets.json` uses [`schema/target-registry.schema.json`](schema/target-registry.schema.json).
Each file under `services/` uses [`schema/service-contract.schema.json`](schema/service-contract.schema.json).
JSONL records described by [`trace-format.md`](trace-format.md) use [`schema/trace-record.schema.json`](schema/trace-record.schema.json).

## Validation

Use any JSON Schema Draft 2020-12 validator with the referenced schema file.
The repository intentionally does not bundle a validator dependency.

## Reading A Contract

`schema_version` versions this documentation format.
`$schema` identifies the JSON Schema that validates the file.
`id` is a stable contract identifier and must not be inferred from a filename.

`firmware` gives the exact Horizon version and confidence for each statement.
`targets` identifies the service names and whether `net-probe` passively intercepts or directly calls them.
`objects` describes object paths as observed by the tracer.

An operation `id` is its raw CMIF command ID.
`request` and `response` list only fields that the tracer or a controlled experiment has decoded.
An omitted field, buffer, handle, or command is unknown rather than absent on Horizon.

`workflows` document observed or controlled interaction sequences.
Their `kind` distinguishes a passive forwarding rule, an observed client pattern, and a diagnostic procedure.
An observed sequence is not a universal service requirement unless its confidence says `confirmed`.

`constraints` are safety or ownership rules that callers and MITMs must preserve.
`provenance` identifies public code or the kind of evidence supporting a statement without disclosing raw firmware material or private reports.

## Field Types

Scalar fields use `u8`, `u16`, `u32`, `u64`, `s8`, `s16`, `s32`, or `s64`.
Other types are `bool`, `ipv4`, `uuid`, `sockaddr`, `handle`, `handle[]`, `buffer`, and `opaque`.

`handle` means a Horizon handle carried by CMIF or HIPC.
`buffer` means an IPC descriptor whose bytes have a separate direction and size contract.
`opaque` means the record is preserved but not semantically decoded.

## Bindings And Sessions

A workflow step can bind a response value with `bind` and pass it later through `arguments`.
For example, BSD `RegisterClient` binds `client_id`, which the observed monitoring session passes to `StartMonitoring`.

`sessions` names the roles that must remain distinct during a workflow.
The `data` and `monitor` BSD sessions are separate root sessions in the observed libnx shape.
They must not be merged merely because both use the same service name.
