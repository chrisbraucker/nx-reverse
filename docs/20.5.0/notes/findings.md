# 20.5.0 Findings

This directory is now split by topic so the `eth` and `anif` work stays readable.

## Files

- [anif-model.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/anif-model.md:1)
  - high-level `bsdsockets` / `eth` / `anif` model
  - confirmed `bsd:u` / `bsd:a` / `bsd:s` registration
  - confirmed user-side `anif` machinery in `bsdsockets_main`
  - recovered `bsd:nu` `ISfUserService` / `Assign` command table
  - confirmed `Assign` gates on serialized interface-descriptor byte `0xa8 == 1`
- [eth.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/eth.md:1)
  - detailed `eth:nd` reverse notes
  - `CreateDriverService`
  - `OpenNetworkInterface`
  - recovered `ISfDriverService` public command table
- [net-probe.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/net-probe.md:1)
  - current runtime probe expectations
  - what successful wired-ethernet results should look like
- [mitm-phase1.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/mitm-phase1.md:1)
  - compiled passive MITM coverage currently narrowed to `nifm:u`
  - current trace file locations and logged event set
  - local libstratosphere changes needed to observe forwarded IPC cleanly
- [pkg2.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/pkg2.md:1)
  - first `pkg2` inventory and kernel observations
  - confirmed absence of network sysmodules from `pkg2`
  - current kernel/KIP prioritization for lower-boundary research
- [sm.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/sm.md:1)
  - first focused `sm.kip1` reverse pass
  - candidate stock `RegisterService` / `UnregisterService` paths
  - confirmed `0x815` is embedded in the `sm` service-registration scan path
- [wlan-usb.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/wlan-usb.md:1)
  - first-pass `wlan_main` and `usb_main` triage
  - why `wlan_main` is the best next static target
  - why `usb_main` likely feeds physical interface inventory into `eth_main`
- [module-triage.md](/workspaces/nx-reversing.git/docs/20.5.0/notes/module-triage.md:1)
  - extraction/mapping notes

## Current Summary

- `bsdsockets_main` almost certainly owns the `bsd:nu` / `ISfUserServiceCreator` side, even though
  the plain `bsd:nu` string is not preserved, and the `ISfUserService` public table now lines up
  with Switchbrew's `Assign` / `GetUserInfo` / `GetStateChangedEvent` description.
- `eth_main` definitely owns a real `ISfDriverService` / `ISfNetworkInterfaceService` stack, and
  `OpenNetworkInterface` is a real implementation that currently fails on-device with
  `0x00048425` when no eligible wired interface matches.
- `wlan_main` mirrors the same `anif::drv` object model on the live Wi-Fi medium.
- `usb_main` appears to own a concrete physical USB interface inventory layer, which is the
  strongest current explanation for where `eth_main` gets real NIC candidates.
- passive MITM is now intentionally narrowed to `nifm:u` in `net-probe`, with added `sm`
  lifecycle tracing so the next run can separate duplicate-registration state from ordinary
  `nifm` traffic.
- `pkg2` inventory does not reveal any hidden network sysmodule in the boot package, which pushes
  the kernel hypothesis toward generic IPC/memory/object primitives rather than a buried netstack.
- `sm.kip1` now gives us a concrete service-manager angle on the probe instability: `0x815` is
  part of the real registration logic, so the `RegisterMitmServer(nifm:u)` failures are grounded
  in `sm` service-table state, not just wrapper behavior.

## Current Priority

1. Deploy the compiled passive MITM probe and collect first `nifm:u` plus `sm` lifecycle traces.
2. Start the first labeled `kernel.bin` / `sm.kip1` / `ProcessMana.kip1` reversing pass.
3. Keep `wlan_main` and `usb_main` in scope while trace and kernel work refine the next probe.
