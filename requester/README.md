# NX Reversing Requester

The requester is a controlled Switch networking harness used to validate Horizon service behavior and the private WireGuard IPC contract.

## Build

Initialize requester submodules before building.

```sh
git submodule update --init --recursive requester
```

The requester requires current WireGuard NX common headers with `wgnx/tunnel_protocol.hpp`.

For parallel development, configure against the local WireGuard NX checkout.

```sh
cd requester
cmake --preset switch \
  -DWGNX_COMMON=/$WORKSPACE_DIR/wireguard-nx.git/common
cmake --build --preset requester
```

The `switch` preset uses Ninja and writes build files to `requester/build`.

For a persistent local header override, create the ignored `requester/CMakeUserPresets.json` file.

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
      "name": "local-requester",
      "configurePreset": "local-switch",
      "targets": ["requester_nro"]
    }
  ]
}
```

Then build the development configuration with:

```sh
cd requester
cmake --preset local-switch
cmake --build --preset local-requester
```

Use the pinned `requester/libs/wireguard-nx/common` headers by omitting `-DWGNX_COMMON`.

The built NRO is written to `requester/out/requester.nro`.

Packet-granularity requester logs are disabled by default so workload measurements do not include per-datagram payload formatting or file writes.

Enable them only for diagnostics with `-DREQUESTER_PACKET_DIAGNOSTICS=ON` during CMake configuration.

To start from a clean build tree, remove the generated directories.

```sh
rm -rf requester/build requester/out
```

## Runtime Configuration

The application starts on a run page and does not create network traffic until `Run` is selected.

The configuration page exposes one shared UDP workload with a mutually exclusive `Tunnel` or `bsd:s` data path selector and an opt-in contract-validation scenario.

Compiled values in [src/config.hpp](src/config.hpp) are the canonical defaults.

At startup, the requester overlays recognized settings from `sdmc:/switch/requester/config.ini`.

Unknown keys and invalid values are logged and that individual setting falls back to its compiled default.

The configuration page writes the same file only after an explicit `Save configuration` action.

The workload runs on Borealis' worker queue so the UI remains responsive while waiting for a completion event.

Press `ZR` on any requester page to request a generation-safe UDP binding bump through `wgnx:ctl`.
The requester keeps its existing `wgnx:tun` client and flow handles open, so this can exercise live-flow behavior across a local transport rebind without launching the manager.
The command is issued on the UI thread because Borealis provides one worker queue and an asynchronous request would otherwise wait behind an active workload.
The sysmodule operation itself only queues the rebind work.

When `wgnx:tun` returns `QueueFull`, the workload waits for the matching `Writable` completion and retries the exact blocked datagram up to 16 times within the configured receive deadline.

The scenario result records total `queue_full_events` so a bounded-burst evaluation can distinguish backpressure recovery from an unpressured run.

### WireGuard Sysmodule Shutdown

The requester consumes `wgnx:ctl` API v5 and `wgnx:tun` API v3 from the configured `WGNX_COMMON` headers.

Before opening `wgnx:tun`, requester uses Atmosphere's read-only SM `HasService` extension so an absent sysmodule reports a scenario error without blocking the worker or mutating SM registration state.

`wgnx:tun` API v3 signals every client completion event during orderly sysmodule shutdown without enqueuing a synthetic completion record.

When a running workload wakes and its following `ReceiveCompletions` call fails, requester records terminal `wgnx:tun service closed` state, stops the workload, and releases its local event, flow, and CMIF session state without retrying the closed service.

An unrelated later tunnel CMIF failure is also terminal for that workload because the client session can no longer be safely reused.

### BSD:S UDP Data Path

Select `bsd:s` as the UDP workload `Data path` to run the normal Horizon BSD scenario.

It initializes libnx sockets with `BsdServiceType_System` and uses ordinary `socket`, `connect`, `send`, `poll`, and `recvfrom` calls.

It does not open or call `wgnx:tun` itself.

The payload uses the `NXRVBS1` workload marker so the controlled harness can separately account for this path and log explicit `source_ip` and `source_port` fields.

With the MITM module disabled, the scenario establishes a direct BSD baseline.

With the requester-only MITM module active and a connected peer that covers the destination, the same scenario should reach the harness through the WireGuard exit path.

The persisted path selection uses complementary flags:

```ini
tunnel_udp.enabled=true
bsd_system_udp.enabled=false
```

Set `tunnel_udp.enabled=false` and `bsd_system_udp.enabled=true` for the `bsd:s` path.

The destination, port, payload size, count, pacing, receive deadline, seed, and echo setting are shared with the direct tunnel workload.

### Contract Validation

The `Tunnel Contract Validation` settings section is disabled by default.

It reuses the configured tunnel UDP destination, payload size, and receive deadline.

`Verify cloned session lifetime` clones a live `wgnx:tun` client, opens a flow through the original session, closes that original session, and completes an echo through the clone.

It then fills the remaining logical client-context slots, verifies that a further client cannot be opened while the clone is the final reference, closes the clone, and verifies that a client context can be opened again.

`Verify mixed batch dispositions` submits one batch containing an accepted datagram, an invalid payload range, an oversized payload, and a stale flow handle.

The requester requires the ordered dispositions `Success`, `MalformedInput`, `DatagramTooLarge`, and `StaleHandle`, then verifies the accepted datagram by echo.

Both contract checks require an echo reply regardless of the normal workload's `Echo replies` setting.

The persisted settings are:

```ini
tunnel_contract.enabled=false
tunnel_contract.verify_cloned_session_lifetime=true
tunnel_contract.verify_mixed_batch=true
```

Run this scenario only against an active real peer and the controlled echo harness.

It is a development contract check, not a normal traffic workload.

The on-screen activity log retains the most recent 160 requester events while the full per-run log continues to be written under `sdmc:/nxrv/requester`.
