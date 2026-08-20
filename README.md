# nx-reversing

`nx-reversing` contains the controlled Horizon networking probes used alongside `wireguard-nx`.
It documents only the service contracts and observable flows required to reproduce or extend those probes.

- `net-probe/` is the passive MITM and trace-capture sysmodule.
- `toolbox/` is the controlled Switch workload client.
- `tools/` contains the controlled host harness and report reconciliation utilities.
- `docs/` contains the public MITM target registry, structured contracts, and concise service-flow guides.

Firmware-local analysis, raw reports, device setup, and research outside this probe surface live in `internal-nx.git` or `device-lab`.
