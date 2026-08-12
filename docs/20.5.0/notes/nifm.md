# 20.5.0 NIFM Interface-Manager Triage

## 2026-08-11: suspected manager service is a compressed-image false positive

NIFM currently appears to consume and aggregate a fixed set of native network media rather than expose a general interface-registration API.

Its source-selection path has capacity and policy for two source kinds only.

This is consistent with the existing WLAN and Ethernet driver-service model.

NIM only exposes ordinary client access to the three NIFM service variants in the current static pass.

Neither module presently provides evidence that a custom sysmodule can introduce a third Horizon network interface through a documented or implied NIM or NIFM API.

The suspected private manager service was absent from the decompressed NIFM executable and from NIFM's service permissions, so it is not a runtime probe target.

NIFM's real interface-manager code is a client of the existing network-driver and BSD user-service roles.

The traced manager wrappers retain supplied native-interface sessions, dispatch through already-held sessions, and return existing session objects to their callers.

They do not register an interface with the service manager or host a new interface service.

Evidence class: static Ghidra CLI analysis of the decompressed 20.5.0 NIFM executable.

Probe conditions: no on-device probe was run.

Confidence: high.

Next question: trace the established driver-to-BSD assignment handoff on-device only if native uplink becomes the active workstream.
