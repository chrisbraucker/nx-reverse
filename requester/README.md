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

To start from a clean build tree, remove the generated directories.

```sh
rm -rf requester/build requester/out
```

## Runtime Configuration

The application starts on a run page and does not create network traffic until `Run` is selected.

The configuration page exposes the direct `wgnx:tun` UDP workload and an opt-in contract-validation scenario.

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
