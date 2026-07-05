# Passive MITM Trace Schema

This document defines the logging schema for passive service MITM work, starting
with `nifm` and `wlan`. The immediate goal is to capture enough structure to:

- reconstruct call graphs and object lifetimes
- correlate state transitions across services
- correlate MITM traces with later `pkg2` reversing and runtime debugging
- avoid re-running expensive or risky probes because earlier logs were too thin

The first target format is line-oriented JSON (`jsonl`), one event per line.
Human-readable side logs can be added later, but the authoritative trace should
stay machine-parseable.

For payload-bearing IPC buffers, the probe may additionally emit append-only
binary streams per service family:

- `probe-mitm-nifm.bin`
- `probe-mitm-bsd.bin`
- `probe-mitm-ssl.bin`

These streams are not a replacement for the JSONL traces. They exist to preserve
exact request/response buffer bytes while keeping filesystem I/O amortized
through the same deferred flush path as the text logs.

`ipc_buffer` records may describe either a HIPC map-alias buffer or a
pointer/static descriptor path used by CMIF auto-select buffers.

## Design Rules

1. Passive only.
   - Forward calls unchanged.
   - Do not mutate payloads, handles, command IDs, or timing intentionally.
2. Structure over volume.
   - Prefer compact, comparable fields over full raw dumps by default.
3. Correlate everything.
   - Every line must be joinable by run, process, session, object, and request.
4. Keep packet-adjacent data bounded.
   - Log hashes, lengths, and short previews before logging whole buffers.
5. Make kernel follow-up possible.
   - Include enough timing and handle metadata to line up with later SVC tracing.

## File Layout

Recommended trace placement:

- `reports/mitm/<date>/<scenario>/<service>.jsonl`
- `reports/mitm/<date>/<scenario>/meta.json`
- `reports/mitm/<date>/<scenario>/<service>.bin`

Where:

- `<date>` is UTC, for example `2026-06-21T18-45-00Z`
- `<scenario>` is one of:
  - `flight-mode`
  - `wifi-idle`
  - `wifi-connected`
  - `ethernet-connected`
  - `mixed-uplink`

`meta.json` should describe the scenario once per run:

- firmware version
- probe/sysmodule build ID
- title IDs or process IDs of the MITM processes
- network state snapshot from `nifm`
- whether Wi-Fi, ethernet, dock, or USB NIC were physically present

## Event Types

All records must include `"event"` with one of the following values.

### Lifecycle

- `trace_start`
- `trace_end`
- `mitm_registered`
- `client_connected`
- `client_disconnected`
- `object_created`
- `object_destroyed`
- `warning`
- `error`

### IPC

- `ipc_request`
- `ipc_response`
- `ipc_decode_request`
- `ipc_decode_response`
- `ipc_buffer`
- `ipc_handle`
- `mitm_dispatch`
- `domain_trace`
- `sm_mitm`
- `nifm_semantic`
- `bsd_semantic`
- `ssl_semantic`

### Optional future correlation

- `svc_marker`
- `debug_marker`

These last two are reserved so later runtime debugging can insert matching
timestamps or sequence IDs without changing the trace shape.

## Common Fields

Every record should carry these fields:

```json
{
  "schema_version": 1,
  "run_id": "2026-06-21T18-45-00Z-wifi-idle",
  "scenario": "wifi-idle",
  "event": "ipc_request",
  "ts_monotonic_ns": 1234567890123,
  "ts_utc": "2026-06-21T18:45:12.345678901Z",
  "service": "wlan:nd",
  "client_pid": 123,
  "client_program_id": "0x0100000000000016",
  "server_program_id": "0x0100000000000016",
  "thread_id": 0,
  "session_id": 4,
  "object_id": 9,
  "request_id": 17
}
```

Field meaning:

- `schema_version`
  - bump only on incompatible format changes
- `run_id`
  - stable ID for one trace run
- `scenario`
  - run classification
- `ts_monotonic_ns`
  - primary ordering source
- `ts_utc`
  - human-facing time for report comparison
- `service`
  - public service name such as `nifm:u`, `nifm:s`, `wlan:nd`
- `client_pid`
  - numeric Horizon PID if available
- `client_program_id`
  - title/program ID if recoverable
- `server_program_id`
  - sysmodule program ID serving the request
- `thread_id`
  - if cheaply available; otherwise `0`
- `session_id`
  - stable per-client-session ID inside the MITM
- `object_id`
  - stable MITM-local ID for the currently addressed object
- `request_id`
  - monotonically increasing request sequence within the run

## Object Model

`session_id` and `object_id` must be stable and explicit.

### `session_id`

Represents one client connection to one MITM service endpoint.

Examples:

- one app opens `nifm:u`
- `bsdsockets_main` opens `wlan:nd`
- a returned child object remains tied to the originating session

### `object_id`

Represents one IPC object view inside the MITM.

Suggested object labels:

- `root`
  - initial public service object
- `creator`
  - service creator object, for example `ISfDriverServiceCreator`
- `driver`
  - returned `ISfDriverService`
- `interface`
  - returned `ISfNetworkInterfaceService`
- `request`
  - `nifm` request/session-style child object
- `unknown`
  - fallback until the object type is reversed

Each `object_created` event should include:

- `object_id`
- `parent_object_id`
- `object_kind`
- `via_command_id`
- `via_service`

Example:

```json
{
  "event": "object_created",
  "service": "wlan:nd",
  "session_id": 4,
  "object_id": 11,
  "parent_object_id": 9,
  "object_kind": "driver",
  "via_command_id": 0,
  "via_service": "ISfDriverServiceCreator"
}
```

## IPC Request Records

Each inbound call should emit one `ipc_request` record before forwarding.

Required fields:

- `command_id`
- `object_kind`
- `object_path`
- `in_raw_size`
- `buffer_count`
- `in_interface_count`
- `out_interface_expected`
- `copy_handle_count`
- `move_handle_count`

Recommended fields:

- `command_name`
  - only when known from reversing or public docs
- `dispatch_hint`
  - local symbol or vtable slot when known

## Binary Payload Records

Binary streams should use a simple versioned record framing, little-endian,
with a fixed-size header followed by raw payload bytes.

Recommended header fields:

- `magic`
- `version`
- `header_size`
- `run_tick`
- `ts_monotonic_ns`
- `request_id`
- `session_id`
- `program_id`
- `process_id`
- `object_id`
- `service_code`
- `command_id`
- `phase`
  - `request` or `response`
- `buffer_direction`
  - `send`, `recv`, or `exchange`
- `buffer_index`
- `flags`
  - first/last chunk markers for large buffers, plus descriptor source flags
- `payload_offset`
- `payload_size`
- `total_buffer_size`

The matching `ipc_buffer` JSONL event should carry the same `request_id`,
`buffer_index`, and phase metadata so offline tooling can join semantic logs to
exact bytes without ambiguity.

Current flag bits:

- `0x1`
  - first chunk
- `0x2`
  - last chunk
- `0x4`
  - payload came from a pointer/static descriptor
- `send_pid`
  - whether the client sent PID metadata
- `hipc_command_type_name`
  - human-readable HIPC type such as `request_with_context` or `close`

`object_path` should be a stable dotted path such as:

- `nifm:u.root`
- `wlan:nd.creator`
- `wlan:nd.creator.driver`
- `nifm:u.root.request`

Decoded passive traces may also distinguish close packets explicitly so they do
not masquerade as public command `0` traffic.

Observed `selected_kind` values now include:

- `root`
- `domain`
- `root_close`
- `domain_close`
- `invalid`

For `nifm`, the tracer may also emit a companion `nifm_semantic` record that
keeps the raw IPC intact but adds decoded summaries for known hot commands.

## IPC Response Records

Each completed forward should emit one `ipc_response` record.

Required fields:

- `command_id`
- `result`
- `duration_ns`
- `out_raw_size`
- `out_interface_count`
- `copy_handle_count`
- `move_handle_count`

Recommended fields:

- `result_name`
  - when mapped locally
- `server_closed_session`
  - boolean when the call appears to terminate the session
- `notes`
  - short diagnostic string for exceptional cases only

## Buffer Records

Buffers should be logged separately as `ipc_buffer` records so request/response
headers stay compact.

Required fields:

- `phase`
  - `in` or `out`
- `buffer_index`
- `buffer_direction`
  - `send`, `recv`, `exchange`, `pointer`, `map-alias`, or `unknown`
- `descriptor_kind`
  - `map_alias` or `pointer_static`
- `declared_size`
- `captured_size`
- `sha256`
- `preview_hex`

Recommended fields:

- `preview_ascii`
- `semantic_tag`
  - for example `interface-filter`, `interface-record`, `ssid`, `ip-config`
- `record_size`
  - for repeated fixed-size records such as `0xb0`
- `record_count`

Capture policy:

- always log full length
- always hash full content if accessible
- log only the first `64` bytes in `preview_hex` by default
- expand to `176` bytes for known `0xb0` interface records
- do not dump arbitrarily large buffers in full unless a debug mode explicitly
  enables it

## Handle Records

Handles returned or consumed by IPC should emit `ipc_handle` records.

Required fields:

- `phase`
  - `in` or `out`
- `handle_index`
- `handle_type`
  - `copy`, `move`, `event`, `session`, `unknown`
- `handle_value`
  - MITM-visible numeric handle if available

Recommended fields:

- `semantic_tag`
  - for example `state-changed-event`
- `linked_object_id`
  - if a returned handle corresponds to a child object or event source

## Service-Specific Requirements

### `nifm`

Primary purpose:

- reconstruct connection-lifecycle and policy flow
- identify which clients request connectivity and when

Must log:

- all root-object commands
- all child request/session objects returned from `nifm`
- event-handle creation and reuse
- state/status payload previews from:
  - internet connection status
  - request state
  - current IP or network profile getters

Important tags:

- `network-request`
- `request-state`
- `internet-status`
- `ip-address`
- `profile-id`
- `event-handle`

Primary questions:

- which processes open `nifm`
- what sequence they use to wait for connectivity
- whether `nifm` points clients toward `wlan`, `eth`, or both indirectly

### `wlan`

Primary purpose:

- reconstruct driver-service, interface-open, and event flow
- determine whether public `wlan:nd` IPC exposes anything below control-plane

Must log:

- creator command `0` / `CreateDriverService`
- driver commands:
  - `OpenNetworkInterface`
  - `GetDriverInfo`
  - `GetNetworkInterfaceList`
  - event getters
  - ioctl family
- returned interface objects and their lifetimes
- exact filter bytes passed to `OpenNetworkInterface`
- exact `0xb0` interface record previews and hashes

Important tags:

- `driver-info`
- `interface-filter`
- `interface-record`
- `state-changed-event`
- `interface-list-event`
- `ioctl-selector`

Primary questions:

- which clients open `wlan:nd`
- whether real consumers use only metadata/event commands or deeper commands
- whether any public `wlan` command exchanges packet-adjacent or frame-adjacent
  buffers

## Scenario Metadata

`meta.json` should include:

```json
{
  "schema_version": 1,
  "run_id": "2026-06-21T18-45-00Z-wifi-idle",
  "firmware": "20.5.0",
  "console_state": {
    "flight_mode": false,
    "wifi_enabled": true,
    "wifi_connected": false,
    "ethernet_present": false,
    "ethernet_connected": false,
    "docked": false
  },
  "nifm_snapshot": {
    "type": 1,
    "wifi_strength": 3,
    "status": 2
  },
  "build": {
    "module": "passive-mitm",
    "git_rev": "unknown"
  }
}
```

The exact enum names can be added later. The key point is to preserve a stable
state snapshot so traces can be compared without guessing the environment.

## Safety Rules For Phase 1

- Do not MITM commands by guessing unknown output buffer shapes.
- Log opaque/raw command metadata even when command names are unknown.
- If a command is known to destabilize `wlan` in connected state, log that it
  was seen from other clients, but do not synthesize extra calls.
- Do not expand buffers recursively or follow unknown returned objects
  proactively during passive tracing.

## Minimum Useful Trace Set

For each scenario, the trace is considered minimally useful only if it captures:

1. all `nifm` client opens and first-level child object creation
2. all `wlan:nd` creator and driver object creation
3. all `OpenNetworkInterface` calls, including filter bytes and result codes
4. all event-handle getters on `nifm` and `wlan`
5. enough timing to identify long-blocking calls or session-closing calls

## Future Extension To `bsd:*`

The same schema should later extend to `bsd:u` and `bsd:s`, with added tags for:

- `socket-domain`
- `socket-type`
- `socket-protocol`
- `sockaddr`
- `send-length`
- `recv-length`
- `poll-fd-set`

That future extension should reuse:

- `run_id`
- `session_id`
- `object_id`
- `request_id`
- `ipc_request` / `ipc_response` / `ipc_buffer` / `ipc_handle`

so traces remain comparable across all service layers.

## Immediate Implementation Target

Phase 1 implementation should support:

1. `nifm:u` passive MITM
2. `nifm:s` passive MITM if accessible from the chosen host context
3. `wlan:nd` passive MITM

Before adding `bsd:*`, confirm that:

- the log volume stays manageable
- `session_id` / `object_id` correlation works under real client traffic
- traces can be compared cleanly across the three Wi-Fi scenarios
