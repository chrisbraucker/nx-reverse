# NX Reversing Toolbox

The toolbox is a controlled Switch networking harness used to validate Horizon service behavior and the private WireGuard IPC contract.

## Build

Initialize toolbox submodules before building.

```sh
git submodule update --init --recursive toolbox
```

The toolbox requires current WireGuard NX common headers with `wgnx/tunnel_protocol.hpp`.

For parallel development, configure against the local WireGuard NX checkout.

```sh
cd toolbox
cmake --preset switch \
  -DWGNX_COMMON=/$WORKSPACE_DIR/wireguard-nx.git/common
cmake --build --preset toolbox
```

The `switch` preset uses Ninja and writes build files to `toolbox/build`.

Run the pure BSD outcome-classification test without the Switch toolchain with:

```sh
cd toolbox
cmake --preset host-tests
cmake --build --preset host-tests
ctest --test-dir build-host-tests --output-on-failure
```

For a persistent local header override, create the ignored `toolbox/CMakeUserPresets.json` file.

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local-switch",
      "inherits": "switch",
      "cacheVariables": {
        "WGNX_COMMON": "/$WORKSPACE_DIR/wireguard-nx.git/common"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "local-toolbox",
      "configurePreset": "local-switch",
      "targets": ["toolbox_nro"]
    }
  ]
}
```

Then build the development configuration with:

```sh
cd toolbox
cmake --preset local-switch
cmake --build --preset local-toolbox
```

Use the pinned `toolbox/libs/wireguard-nx/common` headers by omitting `-DWGNX_COMMON`.

The built NRO is written to `toolbox/out/toolbox.nro`.

Packet-granularity toolbox logs are disabled by default so workload measurements do not include per-datagram payload formatting or file writes.

Enable them only for diagnostics with `-DTOOLBOX_PACKET_DIAGNOSTICS=ON` during CMake configuration.

Each UDP workload emits one final aggregate `[udp-workload-summary]` record.
They report accepted submission bytes and interval, echoed bytes, queue-pressure events, and a fixed-bucket RTT summary when echo is enabled.
`rtt_p50_upper_ns`, `rtt_p95_upper_ns`, and `rtt_p99_upper_ns` are conservative power-of-two histogram upper bounds rather than exact sample percentiles.
Successful echo workload results also display `avg_echo_latency_ms` in the toolbox log.

The controlled harness emits matching `[udp-summary]` records per flow and per workload with unique payload count, byte totals, and its local receive-window interval.
Toolbox submission rate and harness receiver goodput are intentionally separate local-clock measurements and must not be subtracted across hosts.

To start from a clean build tree, remove the generated directories.

```sh
rm -rf toolbox/build toolbox/out
```

## Runtime Configuration

The application starts on a run page and does not create network traffic until `Run` is selected.

The Main page selects one runtime scenario and one target profile for each run.

Profiles own the name, tunnel destination, BSD destination, and UDP destination port.

The Settings page owns scenario behavior such as UDP traffic shape, BSD system checks, and tunnel contract checks.

The legacy Scenarios page remains available for compiled diagnostics that are not yet runtime scenarios.

Compiled values in [src/config.hpp](src/config.hpp) are the canonical defaults.

At startup, the toolbox overlays recognized settings from `sdmc:/config/nxrv-toolbox/config.ini`.

Unknown keys and invalid values are logged and that individual setting falls back to its compiled default.

The Main page displays both the active configuration and the compiled defaults at a glance.

The Profiles page can select the active profile, edit it, and add a copy as the next target profile.

`Expected outcome` reflects a persisted no-reply or terminal-closure selection rather than always displaying the normal workload.

The Settings and Profiles pages write the configuration after an explicit `Save configuration` action.

Starting a Main-page scenario reserves the current `run.next_workload_id`, increments it, and persists the increment before the worker starts.

If that persistence fails, the run does not start and the ID remains available.

The workload runs on Borealis' worker queue so the UI remains responsive while waiting for a completion event.

Press `ZR` on any toolbox page to request a generation-safe tunnel binding bump through `wgnx:ctl`.
The toolbox keeps its existing `wgnx:tun` client and flow handles open, so this can exercise live-flow behavior across a local transport rebind without launching the manager.
The command is issued on the UI thread because Borealis provides one worker queue and an asynchronous request would otherwise wait behind an active workload.
The sysmodule operation itself only queues the rebind work.

When `wgnx:tun` returns `QueueFull`, the workload waits for the matching `Writable` completion and retries the exact blocked datagram up to 16 times within the configured receive deadline.

The scenario result records total `queue_full_events` so a bounded-burst evaluation can distinguish backpressure recovery from an unpressured run.

### WireGuard Sysmodule Shutdown

The toolbox consumes `wgnx:ctl` API v5 and `wgnx:tun` API v3 from the configured `WGNX_COMMON` headers.

Before opening `wgnx:tun`, toolbox uses Atmosphere's read-only SM `HasService` extension so an absent sysmodule reports a scenario error without blocking the worker or mutating SM registration state.

`wgnx:tun` API v3 signals every client completion event during orderly sysmodule shutdown without enqueuing a synthetic completion record.

When a running workload wakes and its following `ReceiveCompletions` call fails, toolbox records terminal `wgnx:tun service closed` state, stops the workload, and releases its local event, flow, and CMIF session state without retrying the closed service.

An unrelated later tunnel CMIF failure is also terminal for that workload because the client session can no longer be safely reused.

### BSD:S UDP Data Path

Select `BSD system UDP` on the Main page to run the normal Horizon BSD scenario.

It initializes libnx sockets with `BsdServiceType_System` and uses ordinary `socket`, `connect`, `getsockname`, `fcntl`, `send`, `poll`, and `recvfrom` calls.

After every successful connection, the workload verifies and logs a concrete local IPv4 address and ephemeral port.
This confirms that a tunneled BSD:S socket exposes the device-facing local endpoint rather than the WireGuard interface tuple.

The workload sets `O_NONBLOCK` after each successful connection and waits with `poll` before receiving an echo or retrying an `EAGAIN` send.

`Expected BSD:S outcome` selects either the normal workload, a one-datagram no-reply timeout, or terminal closure validation.
The timeout mode requires `Echo replies=false`, one datagram, and one flow, then succeeds only when `poll(POLLIN)` expires without a reply.
The terminal mode requires one echoed datagram and one flow, then waits up to the shared receive deadline for the operator to deactivate the peer or stop the sysmodule.
It succeeds only after `POLLHUP`, a later zero-flag `send` returns `ECONNABORTED`, and `close` succeeds.
`Require writable recovery after queue pressure` turns a burst with no observed `EAGAIN` followed by `POLLOUT` into an inconclusive failure instead of a throughput success.

Enable `bsd_system_udp.verify_post_route_rejection=true` only for the tunneled MITM path to verify that a post-route `SetSockOpt` fails with `EOPNOTSUPP` instead of mutating the retained upstream descriptor.

It does not open or call `wgnx:tun` itself.

The payload uses the `NXRVBS1` workload marker so the controlled harness can separately account for this path and log explicit `source_ip` and `source_port` fields.

With the MITM module disabled, the scenario establishes a direct BSD baseline.

With the toolbox-only MITM module active and a connected peer that covers the destination, the same scenario should reach the harness through the WireGuard exit path.

The runtime configuration stores the selected scenario, next workload ID, active profile, and profile entries explicitly.

```ini
run.scenario=direct_tunnel_udp
run.next_workload_id=1
run.active_profile=0
profiles.count=1
profile.0.name=Default
profile.0.tunnel_destination_ipv4=10.1.0.2
profile.0.bsd_destination_ipv4=10.1.0.2
profile.0.udp_destination_port=29000
```

Use `run.scenario=bsd_system_udp` for the BSD scenario or `run.scenario=tunnel_contract_validation` for the contract scenario.

The active profile determines the destination used by the selected scenario.

UDP behavior is stored separately from the profile.

```ini
udp.payload_bytes=48
udp.datagram_count=1
udp.pacing_ms=0
udp.concurrent_flows=1
udp.receive_deadline_ms=5000
udp.payload_seed=1314417238
udp.echo_replies=true
```

The persisted BSD:S-specific settings are:

```ini
bsd_system_udp.verify_post_route_rejection=false
bsd_system_udp.expected_outcome=echo
bsd_system_udp.require_writable_recovery=false
```

Run `python3 tools/summarize_reports.py --check <toolbox.log> <harness.log> <mitm.log> <wgnx.log>` from the repository root to render the aggregate Task 4 rows and validate the available per-flow accounting invariants.
For the new timing records, it displays toolbox `submission_rate_mb_s` and harness `receiver_goodput_mb_s` separately.
`adapter_queued` records a successful BSD-to-MITM local FIFO admission, while `adapter_queue_full` records a BSD-visible `EAGAIN` before that admission.
`queue_full` and WireGuard `send_queue_full` record later downstream staging pressure, which can occur after the BSD send has succeeded and is therefore not expected to equal toolbox retry count.
The tool does not parse per-packet output and leaves raw logs authoritative.

### Contract Validation

Select `Tunnel contract validation` on the Main page to run the contract scenario.

It reuses the configured tunnel UDP destination, payload size, and receive deadline.

`Verify cloned session lifetime` clones a live `wgnx:tun` client, opens a flow through the original session, closes that original session, and completes an echo through the clone.

It then fills the remaining logical client-context slots, verifies that a further client cannot be opened while the clone is the final reference, closes the clone, and verifies that a client context can be opened again.

`Verify mixed batch dispositions` submits one batch containing an accepted datagram, an invalid payload range, an oversized payload, and a stale flow handle.

The toolbox requires the ordered dispositions `Success`, `MalformedInput`, `DatagramTooLarge`, and `StaleHandle`, then verifies the accepted datagram by echo.

Both contract checks require an echo reply regardless of the normal workload's `Echo replies` setting.

The persisted contract settings are:

```ini
tunnel_contract.verify_cloned_session_lifetime=true
tunnel_contract.verify_mixed_batch=true
```

Run this scenario only against an active real peer and the controlled echo harness.

It is a development contract check, not a normal traffic workload.

The on-screen activity log retains the most recent 160 toolbox events while the full per-run log continues to be written under `sdmc:/nxrv/toolbox`.
