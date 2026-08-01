#!/usr/bin/env python3

from __future__ import annotations

import http.server
import argparse
import signal
import socketserver
import ssl
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path

# Easy-to-edit endpoint configuration.
LISTEN_HOST = "0.0.0.0"
TCP_ACK_PORT = 28080
HTTP_PORT = 28081
HTTPS_PORT = 28443
UDP_PORTS = (29000, 29001)

TCP_REPLY = b"NXRV TCP ACK\r\n"
HTTP_BODY = b"nxrv harness http ok\n"
HTTPS_BODY = b"nxrv harness https ok\n"
UDP_FIXED_REPLY = b"NXRV UDP ACK"
UDP_ECHO_INPUT = True
UDP_VERBOSE = True

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
TLS_DIR = REPO_ROOT / "workspace" / "requester-harness" / "tls"
TLS_CERT_FILE = TLS_DIR / "requester-local.crt"
TLS_KEY_FILE = TLS_DIR / "requester-local.key"


def log(message: str) -> None:
    print(message, flush=True)


class UdpWorkloadStats:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._received = Counter[tuple[str, int, int]]()
        self._echoed = Counter[tuple[str, int, int]]()
        self._bytes_received = Counter[tuple[str, int, int]]()
        self._bytes_echoed = Counter[tuple[str, int, int]]()
        self._sources: dict[tuple[str, int, int], set[str]] = defaultdict(set)
        self._sequences: dict[tuple[str, int, int], set[int]] = defaultdict(set)
        self._last_sequence: dict[tuple[str, int, int], int] = {}
        self._duplicates = Counter[tuple[str, int, int]]()
        self._reordered = Counter[tuple[str, int, int]]()
        self._first_received_ns: dict[tuple[str, int, int], int] = {}
        self._last_received_ns: dict[tuple[str, int, int], int] = {}
        self._unexpected = 0
        self._malformed = 0

    def record(self, data: bytes, source: str, echoed: bool) -> str | None:
        identity, malformed = parse_workload_identity(data)
        with self._lock:
            if identity is None:
                if malformed:
                    self._malformed += 1
                else:
                    self._unexpected += 1
                return None
            workload_kind, workload_id, flow_index, sequence, seed = identity
            key = (workload_kind, workload_id, flow_index)
            received_ns = time.monotonic_ns()
            self._received[key] += 1
            self._bytes_received[key] += len(data)
            self._first_received_ns.setdefault(key, received_ns)
            self._last_received_ns[key] = received_ns
            self._sources[key].add(source)
            if sequence in self._sequences[key]:
                self._duplicates[key] += 1
            self._sequences[key].add(sequence)
            previous = self._last_sequence.get(key)
            if previous is not None and sequence < previous:
                self._reordered[key] += 1
            self._last_sequence[key] = max(sequence, previous) if previous is not None else sequence
            if echoed:
                self._echoed[key] += 1
                self._bytes_echoed[key] += len(data)
        return f"kind={workload_kind} workload={workload_id} flow={flow_index} sequence={sequence} seed=0x{seed:08x}"

    def summary_rows(self) -> list[dict[str, int | str]]:
        with self._lock:
            keys = sorted(set(self._received) | set(self._echoed))
            rows = [self._summary_row(key, "flow") for key in keys]
            workload_keys: dict[tuple[str, int], list[tuple[str, int, int]]] = defaultdict(list)
            for key in keys:
                workload_keys[key[:2]].append(key)
            for workload_kind, workload_id in sorted(workload_keys):
                grouped_keys = workload_keys[(workload_kind, workload_id)]
                first_received_ns = min(self._first_received_ns[key] for key in grouped_keys)
                last_received_ns = max(self._last_received_ns[key] for key in grouped_keys)
                rows.append(
                    {
                        "kind": workload_kind,
                        "workload": workload_id,
                        "scope": "workload",
                        "flow": "all",
                        "received": sum(self._received[key] for key in grouped_keys),
                        "received_bytes": sum(self._bytes_received[key] for key in grouped_keys),
                        "echoed": sum(self._echoed[key] for key in grouped_keys),
                        "echoed_bytes": sum(self._bytes_echoed[key] for key in grouped_keys),
                        "unique": sum(len(self._sequences[key]) for key in grouped_keys),
                        "duplicates": sum(self._duplicates[key] for key in grouped_keys),
                        "reordered": sum(self._reordered[key] for key in grouped_keys),
                        "receive_elapsed_ns": last_received_ns - first_received_ns,
                        "sources": ",".join(sorted({source for key in grouped_keys for source in self._sources[key]})),
                    }
                )
            return rows

    def _summary_row(self, key: tuple[str, int, int], scope: str) -> dict[str, int | str]:
        workload_kind, workload_id, flow_index = key
        return {
            "kind": workload_kind,
            "workload": workload_id,
            "scope": scope,
            "flow": flow_index,
            "received": self._received[key],
            "received_bytes": self._bytes_received[key],
            "echoed": self._echoed[key],
            "echoed_bytes": self._bytes_echoed[key],
            "unique": len(self._sequences[key]),
            "duplicates": self._duplicates[key],
            "reordered": self._reordered[key],
            "receive_elapsed_ns": self._last_received_ns[key] - self._first_received_ns[key],
            "sources": ",".join(sorted(self._sources[key])),
        }

    def log_summary(self) -> None:
        for row in self.summary_rows():
            log(
                "[udp-summary] "
                f"kind={row['kind']} workload={row['workload']} scope={row['scope']} flow={row['flow']} "
                f"received={row['received']} received_bytes={row['received_bytes']} "
                f"echoed={row['echoed']} echoed_bytes={row['echoed_bytes']} unique={row['unique']} "
                f"duplicates={row['duplicates']} reordered={row['reordered']} "
                f"receive_elapsed_ns={row['receive_elapsed_ns']} sources={row['sources']}"
            )
        with self._lock:
            if self._unexpected:
                log(f"[udp-summary] unexpected_datagrams={self._unexpected}")
            if self._malformed:
                log(f"[udp-summary] malformed_workload_datagrams={self._malformed}")


def parse_workload_identity(data: bytes) -> tuple[tuple[str, int, int, int, int] | None, bool]:
    workload_kind = {
        b"NXRVWG1": "wgnx-tun",
        b"NXRVBS1": "bsd-system",
    }.get(data[:7])
    if workload_kind is None:
        return None, False
    if len(data) < 24 or data[7] != 0:
        return None, True
    return (
        workload_kind,
        int.from_bytes(data[8:12], "big"),
        int.from_bytes(data[12:16], "big"),
        int.from_bytes(data[16:20], "big"),
        int.from_bytes(data[20:24], "big"),
    ), False


UDP_STATS = UdpWorkloadStats()


class ThreadedTcpServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class AggregateUdpServer(socketserver.UDPServer):
    allow_reuse_address = True


class QuietHttpServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


class PlainTcpHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(2.0)
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        try:
            data = self.request.recv(1024)
        except TimeoutError:
            data = b""
        log(f"[tcp] peer={peer} recv={data[:64]!r}")
        self.request.sendall(TCP_REPLY)


class UdpHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        data, sock = self.request
        source_ip, source_port = self.client_address
        peer = f"{source_ip}:{source_port}"
        local_port = self.server.server_address[1]
        reply = data if UDP_ECHO_INPUT else UDP_FIXED_REPLY
        workload = UDP_STATS.record(data, peer, UDP_ECHO_INPUT)
        if UDP_VERBOSE and workload is None:
            log(
                f"[udp:{local_port}] source_ip={source_ip} source_port={source_port} "
                f"recv={data[:64]!r} reply={reply[:64]!r}"
            )
        elif UDP_VERBOSE:
            log(
                f"[udp:{local_port}] source_ip={source_ip} source_port={source_port} "
                f"{workload} bytes={len(data)} echo={UDP_ECHO_INPUT}"
            )
        if UDP_ECHO_INPUT:
            sock.sendto(reply, self.client_address)


class CannedHttpHandler(http.server.BaseHTTPRequestHandler):
    server_version = "NXRVHarness/1.0"
    protocol_version = "HTTP/1.1"
    body = HTTP_BODY
    label = "http"

    def do_GET(self) -> None:  # noqa: N802
        log(f"[{self.label}] peer={self.client_address[0]}:{self.client_address[1]} path={self.path}")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(self.body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(self.body)

    def do_HEAD(self) -> None:  # noqa: N802
        log(f"[{self.label}] peer={self.client_address[0]}:{self.client_address[1]} path={self.path} method=HEAD")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(self.body)))
        self.send_header("Connection", "close")
        self.end_headers()

    def log_message(self, fmt: str, *args: object) -> None:
        del fmt, args


class CannedHttpsHandler(CannedHttpHandler):
    body = HTTPS_BODY
    label = "https"


def serve_in_thread(server: socketserver.BaseServer, label: str) -> threading.Thread:
    thread = threading.Thread(target=server.serve_forever, name=f"{label}-server", daemon=True)
    thread.start()
    return thread


def require_tls_material() -> None:
    if TLS_CERT_FILE.is_file() and TLS_KEY_FILE.is_file():
        return
    raise SystemExit(
        "missing HTTPS certificate material: "
        f"{TLS_CERT_FILE} / {TLS_KEY_FILE}\n"
        "run tools/generate_requester_https_certs.sh first"
    )


def build_https_server() -> QuietHttpServer:
    require_tls_material()
    server = QuietHttpServer((LISTEN_HOST, HTTPS_PORT), CannedHttpsHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=TLS_CERT_FILE, keyfile=TLS_KEY_FILE)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Controlled requester counterparty")
    parser.add_argument("--listen-host", default=LISTEN_HOST)
    parser.add_argument("--udp-ports", default=",".join(str(port) for port in UDP_PORTS))
    parser.add_argument("--udp-no-echo", action="store_true", help="record UDP traffic without replying")
    parser.add_argument("--udp-quiet", action="store_true", help="aggregate UDP workload accounting and suppress per-datagram output")
    return parser.parse_args()


def main() -> int:
    global LISTEN_HOST, UDP_PORTS, UDP_ECHO_INPUT, UDP_VERBOSE
    args = parse_args()
    LISTEN_HOST = args.listen_host
    UDP_PORTS = tuple(int(value) for value in args.udp_ports.split(",") if value)
    if not UDP_PORTS or any(port < 1 or port > 65535 for port in UDP_PORTS):
        raise SystemExit("--udp-ports must contain one or more ports in 1..65535")
    UDP_ECHO_INPUT = not args.udp_no_echo
    UDP_VERBOSE = not args.udp_quiet
    tcp_server = ThreadedTcpServer((LISTEN_HOST, TCP_ACK_PORT), PlainTcpHandler)
    http_server = QuietHttpServer((LISTEN_HOST, HTTP_PORT), CannedHttpHandler)
    #https_server = build_https_server()

    servers: list[tuple[str, socketserver.BaseServer]] = [
        ("tcp", tcp_server),
        ("http", http_server),
        #("https", https_server),
    ]
    servers.extend(
        (f"udp:{port}", AggregateUdpServer((LISTEN_HOST, port), UdpHandler))
        for port in UDP_PORTS
    )

    stop_event = threading.Event()

    def request_stop(signum: int, _frame: object) -> None:
        log(f"received signal {signum}, shutting down")
        stop_event.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    for label, server in servers:
        serve_in_thread(server, label)

    log("requester harness listening")
    log(f"  tcp   : {LISTEN_HOST}:{TCP_ACK_PORT}")
    log(f"  http  : {LISTEN_HOST}:{HTTP_PORT}")
    #log(f"  https : {LISTEN_HOST}:{HTTPS_PORT} cert={TLS_CERT_FILE}")
    for port in UDP_PORTS:
        log(f"  udp   : {LISTEN_HOST}:{port} echo={UDP_ECHO_INPUT} verbose={UDP_VERBOSE}")

    try:
        stop_event.wait()
    finally:
        for label, server in servers:
            log(f"stopping {label}")
            server.shutdown()
            server.server_close()
        UDP_STATS.log_summary()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
