#!/usr/bin/env python3

from __future__ import annotations

import http.server
import signal
import socketserver
import ssl
import threading
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

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
TLS_DIR = REPO_ROOT / "workspace" / "requester-harness" / "tls"
TLS_CERT_FILE = TLS_DIR / "requester-local.crt"
TLS_KEY_FILE = TLS_DIR / "requester-local.key"


def log(message: str) -> None:
    print(message, flush=True)


class ThreadedTcpServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class ThreadedUdpServer(socketserver.ThreadingMixIn, socketserver.UDPServer):
    allow_reuse_address = True
    daemon_threads = True


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
        peer = f"{self.client_address[0]}:{self.client_address[1]}"
        local_port = self.server.server_address[1]
        reply = data if UDP_ECHO_INPUT else UDP_FIXED_REPLY
        log(f"[udp:{local_port}] peer={peer} recv={data[:64]!r} reply={reply[:64]!r}")
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


def main() -> int:
    tcp_server = ThreadedTcpServer((LISTEN_HOST, TCP_ACK_PORT), PlainTcpHandler)
    http_server = QuietHttpServer((LISTEN_HOST, HTTP_PORT), CannedHttpHandler)
    #https_server = build_https_server()

    servers: list[tuple[str, socketserver.BaseServer]] = [
        ("tcp", tcp_server),
        ("http", http_server),
        #("https", https_server),
    ]
    servers.extend(
        (f"udp:{port}", ThreadedUdpServer((LISTEN_HOST, port), UdpHandler))
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
        log(f"  udp   : {LISTEN_HOST}:{port} echo={UDP_ECHO_INPUT}")

    try:
        stop_event.wait()
    finally:
        for label, server in servers:
            log(f"stopping {label}")
            server.shutdown()
            server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
