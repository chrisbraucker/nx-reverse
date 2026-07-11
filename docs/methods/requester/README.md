# Requester Spec

## Purpose

`requester` is a purpose-built user-mode homebrew application for generating
repeatable, low-noise network activity while `net-probe` passively MITMs
`nifm:*`, `bsd:*`, and `ssl:*`.

It exists to replace opportunistic probing through third-party titles such as
`sphaira`, `switchfin`, or browser applets when we need deterministic behavior
and a clean, self-described action timeline.

## Design Goals

- produce a fixed sequence of known network actions
- log what the app intended to do, independently from what the MITM observed
- keep the runtime simple enough that crashes or odd traffic are easy to assign
- mirror the rough structure of `wireguard-nx.git/manager`
- write local logs to `sdmc:/nxrv/requester/`
- print short human-readable status lines via `printf()` to the text console
- exit cleanly after the probe sequence finishes

## Non-Goals

- this is not a long-lived sysmodule
- this is not a UI-heavy app
- this is not a general-purpose network diagnostic tool
- this is not the VPN implementation

## Proposed Repo Shape

The requester should be added as a separate application subtree, parallel to
`net-probe`, and intentionally shaped similarly to the existing `manager`
project:

```text
requester/
  Makefile
  src/
    main.cpp
    runtime.cpp
    runtime.hpp
    scenarios.cpp
    scenarios.hpp
    logger.cpp
    logger.hpp
    config.hpp
  data/
  out/
  build/
```

Notes:

- `main.cpp`
  - console setup, app init, top-level execution, clean exit
- `runtime.*`
  - environment snapshot, timestamping, directory setup, shared helpers
- `scenarios.*`
  - concrete probe steps
- `logger.*`
  - append-only text logging to `sdmc:/nxrv/requester/`
- `config.hpp`
  - compile-time defaults for target hosts, ports, and timeouts

The app should remain self-contained and should not depend on `net-probe`
internals.

## Runtime Behavior

The requester should run as follows:

1. initialize console output
2. initialize `socket` and `nifm`
3. create `sdmc:/nxrv/requester/` if absent
4. create one per-run text log file
5. capture and log a short environment snapshot
6. execute a fixed scenario list in order
7. print and log completion status
8. wait a short grace period
9. exit cleanly

Current implementation detail:

- the requester now uses a Sphaira-like `socketInitialize(...)` configuration
  with `num_bsd_sessions = 3` and `BsdServiceType_Auto`
- it also initializes `libcurl` and runs explicit libcurl-backed HTTP/HTTPS
  scenarios in addition to the existing raw socket and Horizon `ssl:*` paths
- this exists specifically to see whether the `bsd:s` second-launch failure can
  be reproduced with a smaller, controlled client before patching Sphaira

The grace period should default to about 3 seconds. Its purpose is:

- allow the user to read the console summary
- give `net-probe` time to flush deferred trace buffers
- avoid leaving ambiguity about whether the app hung or simply finished

## Console Contract

The text UI should stay deliberately sparse. Each scenario should print a short
line before it runs, for example:

- `Running NIFM status check`
- `Running DNS resolve for example.com`
- `Connecting TCP to 192.168.0.1:80`
- `Holding idle TCP socket to 192.168.0.1:80 for 1500 ms`
- `Running HTTP GET for http://192.168.0.1/`
- `Running HTTPS GET for https://example.com/`
- `Running UDP echo to 192.168.0.2:9000`
- `Running 3 concurrent TCP sessions to 192.168.0.1:80`

At the end it should print:

- success or failure summary per scenario
- the log path used
- `Requester finished; exiting in 3 seconds`

The console is only a high-level operator view. Detailed evidence belongs in
the log files and in the `net-probe` traces.

## Local Log Contract

Base directory:

- `sdmc:/nxrv/requester/`

Per-run file naming:

- `requester-<run-id>.log`

Suggested `run-id` shape:

- UTC timestamp plus a monotonic suffix if needed
- example: `2026-07-05T14-22-18Z`

The text log should include:

- build/version string
- HOS version
- current IP if available
- scenario start/end markers
- target host/IP/port
- exact libnx or socket-layer operation attempted
- raw `Result` values
- BSD/socket return values and `errno` where relevant
- bytes sent/received
- short previews or hashes of payloads when useful

The requester log is the client-side intent log. It does not replace the
service traces produced by `net-probe`.

## Host Harness

Two helper scripts should be used with the requester:

- `tools/requester_harness.py`
  - starts one plain TCP endpoint, one HTTP endpoint, one HTTPS endpoint, and
    one UDP endpoint at the same time
  - keeps all listening ports as top-level constants so retargeting is trivial
- `tools/generate_requester_https_certs.sh`
  - creates a minimal self-signed keypair for the HTTPS listener
  - defaults to writing under `workspace/requester-harness/tls/`

Important caveat:

- the current requester HTTPS scenario uses Horizon `ssl` with normal peer and
  hostname verification enabled
- a self-signed certificate is therefore expected to fail validation unless the
  requester is later adjusted to relax verification or a trusted certificate is
  used instead
- this is still useful for trace generation because the `ssl:*` handshake path
  is exercised in a deterministic way

## Initial Scenario Set

The first revision should stay narrow and cover the paths we care about most.

### Scenario 1: Environment snapshot

Actions:

- `nifmGetInternetConnectionStatus(...)`
- `nifmGetCurrentIpAddress(...)`
- record whether the device appears offline, Wi-Fi attached, or connected

Purpose:

- give a baseline for the rest of the run
- correlate with `nifm:u` / `nifm:s` traces

### Scenario 2: DNS resolve

Actions:

- resolve a configured hostname using the normal socket resolver path

Default target:

- a hostname served by the host harness or another controlled endpoint

Purpose:

- identify the plain resolver path in `bsd:*`
- verify whether resolver traffic is visible where expected

### Scenario 3: Plain TCP round trip

Actions:

- open `AF_INET` `SOCK_STREAM`
- connect to configured host and port
- send a tiny marker payload
- read a short reply
- close cleanly

Purpose:

- produce a minimal non-TLS TCP sequence
- correlate `socket`, `fcntl`, `connect`, `send`, `recv`, and `close`

### Scenario 4: Idle TCP hold

Actions:

- open `AF_INET` `SOCK_STREAM`
- connect to configured host and port
- hold the socket open without sending payload data for a short fixed interval
- close cleanly

Purpose:

- identify what purely connected-but-idle TCP state looks like in `bsd:*`
- distinguish connect lifecycle from payload-bearing traffic

### Scenario 5: HTTP GET

Actions:

- perform a deliberately small HTTP/1.1 GET over plain TCP
- target should be a simple controlled endpoint
- read enough of the response to prove the data path

Purpose:

- verify where plain HTTP appears
- compare with earlier browser/applet ambiguity

### Scenario 6: HTTPS GET

Actions:

- perform one small HTTPS request through the standard Horizon/libnx path
- read enough of the response to prove the TLS-backed data path

Purpose:

- correlate the handoff between `bsd:*` and `ssl:*`
- compare with the existing `sphaira` observations

### Scenario 7: UDP echo

Actions:

- open `AF_INET` `SOCK_DGRAM`
- send one small datagram to a controlled echo endpoint
- wait for the reply
- log success, timeout, or mismatch

Purpose:

- exercise the non-TCP path directly
- give us a clean reference for future tunnel-oriented work

### Scenario 8: Concurrent TCP burst

Actions:

- open several `AF_INET` `SOCK_STREAM` sockets to the same target
- keep them open briefly at the same time
- send a small per-socket marker payload on each connection
- read a short reply from each socket
- close all sockets cleanly

Purpose:

- reveal whether `bsd:*` bookkeeping changes once multiple active sessions overlap
- provide a clean baseline for future throughput and multiplexing experiments

## Host Harness Expectations

The requester should be built against endpoints we control.

Minimum useful host-side services:

- DNS answer for one known test name
- TCP echo or banner service
- HTTP server on a known port
- HTTPS server on a known port
- UDP echo service

This harness does not need to be complex. Its job is to make the Switch-side
traffic predictable and easy to recognize in traces and packet captures.

## Scenario Ordering

Default order:

1. environment snapshot
2. DNS resolve
3. plain TCP connect
4. HTTP GET
5. HTTPS GET
6. UDP echo

This order is intentional:

- it starts with the lightest control-plane queries
- it exercises `bsd:*` before `ssl:*`
- it leaves UDP until the end because it is the least tied to higher-level app
  semantics

## Failure Handling

The requester should not abort on the first failure unless initialization
itself fails.

Per-scenario failures should be:

- printed to the console in one short line
- logged with enough detail to explain the failure
- followed by the next scenario

Initialization failures that may justify early exit:

- `socketInitializeDefault()` failure
- `nifmInitialize(...)` failure
- inability to create `sdmc:/nxrv/requester/`
- inability to open the run log

## Correlation Requirements

To align requester logs with `net-probe` traces, each run should expose:

- one explicit `run_id`
- scenario names stable across runs
- UTC timestamps
- target hostnames and resolved addresses

Later, if needed, we can add a tiny application-level marker payload format, but
the first revision should avoid inventing extra protocol unless correlation is
still ambiguous.

## Output Example

Example console flow:

```text
Requester build 0.0.1-dev-abcdef0
Logging to sdmc:/nxrv/requester/requester-2026-07-05T14-22-18Z.log
Running NIFM status check
Running DNS resolve for test.nxrv.local
Connecting TCP to 192.168.0.2:8081
Running HTTP GET for http://192.168.0.2:8080/
Running HTTPS GET for https://192.168.0.2:8443/
Running UDP echo to 192.168.0.2:9000
Requester finished; exiting in 3 seconds
```

## First Implementation Boundary

The first implementation pass should stop at:

- a buildable standalone NRO
- deterministic scenario execution
- console output
- persistent text logging

It should not yet add:

- JSON logs
- custom binary payload capture
- interactive menus
- asynchronous scenario scheduling
- multiple profiles or config files

Those can be added once the basic requester is producing useful traces.
