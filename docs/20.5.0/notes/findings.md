# 20.5.0 Findings

This directory is now split by topic so the `eth` and `anif` work stays readable.

## Files

- [anif-model.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/anif-model.md:1)
  - high-level `bsdsockets` / `eth` / `anif` model
  - confirmed `bsd:u` / `bsd:a` / `bsd:s` registration
  - confirmed user-side `anif` machinery in `bsdsockets_main`
  - recovered `bsd:nu` `ISfUserService` / `Assign` command table
  - confirmed `Assign` gates on serialized interface-descriptor byte `0xa8 == 1`
- [eth.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/eth.md:1)
  - detailed `eth:nd` reverse notes
  - `CreateDriverService`
  - `OpenNetworkInterface`
  - recovered `ISfDriverService` public command table
- [net-probe.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/net-probe.md:1)
  - current runtime probe expectations
  - what successful wired-ethernet results should look like
- [resolver.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/resolver.md:1)
  - confirmed `sfdnsres` and `dns:priv` registration inside `bsdsockets_main`
  - correlates the binary with local `libnx` resolver headers
  - narrows the next DNS probe target to `sfdnsres`
- [mitm-phase1.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/mitm-phase1.md:1)
  - compiled passive MITM coverage currently narrowed to `nifm:u`
  - current trace file locations and logged event set
  - local libstratosphere changes needed to observe forwarded IPC cleanly
- [pkg2.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/pkg2.md:1)
  - first `pkg2` inventory and kernel observations
  - confirmed absence of network sysmodules from `pkg2`
  - current kernel/KIP prioritization for lower-boundary research
- [sm.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/sm.md:1)
  - first focused `sm.kip1` reverse pass
  - candidate stock `RegisterService` / `UnregisterService` paths
  - confirmed `0x815` is embedded in the `sm` service-registration scan path
- [wlan-usb.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/wlan-usb.md:1)
  - first-pass `wlan_main` and `usb_main` triage
  - why `wlan_main` is the best next static target
  - why `usb_main` likely feeds physical interface inventory into `eth_main`
- [module-triage.md](/workspaces/switch-workspace/nx-reversing.git/docs/20.5.0/notes/module-triage.md:1)
  - extraction/mapping notes

## Current Summary

- `bsdsockets_main` almost certainly owns the `bsd:nu` / `ISfUserServiceCreator` side, even though
  the plain `bsd:nu` string is not preserved, and the `ISfUserService` public table now lines up
  with Switchbrew's `Assign` / `GetUserInfo` / `GetStateChangedEvent` description.
- `bsdsockets_main` also definitely owns the resolver IPC surface on `20.5.0`:
  - `sfdnsres`
  - `dns:priv`
  - eight `nn.socket.ResolverIpcServer` workers
- `eth_main` definitely owns a real `ISfDriverService` / `ISfNetworkInterfaceService` stack, and
  `OpenNetworkInterface` is a real implementation that currently fails on-device with
  `0x00048425` when no eligible wired interface matches.
- `wlan_main` mirrors the same `anif::drv` object model on the live Wi-Fi medium.
- `usb_main` appears to own a concrete physical USB interface inventory layer, which is the
  strongest current explanation for where `eth_main` gets real NIC candidates.
- passive MITM is now intentionally narrowed to `nifm:u` in `net-probe`, with added `sm`
  lifecycle tracing so the next run can separate duplicate-registration state from ordinary
  `nifm` traffic.
- DNS should now be treated as a `bsdsockets`-hosted sibling IPC path rather than an unresolved
  generic socket side effect. The best current runtime target is `sfdnsres`.
- `pkg2` inventory does not reveal any hidden network sysmodule in the boot package, which pushes
  the kernel hypothesis toward generic IPC/memory/object primitives rather than a buried netstack.
- `sm.kip1` now gives us a concrete service-manager angle on the probe instability: `0x815` is
  part of the real registration logic, so the `RegisterMitmServer(nifm:u)` failures are grounded
  in `sm` service-table state, not just wrapper behavior.

## Current Priority

1. Add a narrow passive MITM for `sfdnsres` and correlate it with the controlled requester DNS
   scenario.
2. Keep `bsd:u` and `ssl` tracing focused on transport behavior while the resolver path is mapped
   separately.
3. Continue lower-boundary `pkg2` / KIP reversing in parallel, but only where it helps answer the
   system-wide VPN integration question.
