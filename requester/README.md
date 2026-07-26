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

The configuration page currently exposes only the direct `wgnx:tun` UDP workload used for IPC and tunnel validation.

Compiled values in [src/config.hpp](src/config.hpp) are the canonical defaults.

At startup, the requester overlays recognized settings from `sdmc:/switch/requester/config.ini`.

Unknown keys and invalid values are logged and that individual setting falls back to its compiled default.

The configuration page writes the same file only after an explicit `Save configuration` action.

The workload runs on Borealis' worker queue so the UI remains responsive while waiting for a completion event.

The on-screen activity log retains the most recent 160 requester events while the full per-run log continues to be written under `sdmc:/nxrv/requester`.
