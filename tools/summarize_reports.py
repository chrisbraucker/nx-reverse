#!/usr/bin/env python3
"""Summarize Task 4 requester, harness, MITM, and WGNX aggregate logs."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


KEY_VALUE = re.compile(r"(\w+)=([^\s]+)")
REQUESTER = re.compile(
    r"\[(?P<status>OK|FAIL)\] bsd_system_udp_workload \| sent=(?P<sent>\d+) "
    r"recv=(?P<received>\d+) \| (?P<detail>.*)"
)
REQUESTER_METRICS = re.compile(r"\[udp-workload-summary\] (?P<detail>.*)")
HARNESS = re.compile(r"\[udp-summary\] (?P<detail>.*)")
MITM = re.compile(r"tunnel flow summary (?P<detail>.*)")
MITM_WORKER = re.compile(r"tunnel worker summary (?P<detail>.*)")
WGNX = re.compile(r"Closed tunnel UDP (?P<detail>flow=.*send_attempts=.*)")


def fields(detail: str) -> dict[str, str]:
    return dict(KEY_VALUE.findall(detail))


def rate_mb_s(byte_count: str | None, elapsed_ns: str | None) -> str | None:
    if byte_count is None or elapsed_ns is None or int(elapsed_ns) == 0:
        return None
    return f"{(int(byte_count) * 1000) / int(elapsed_ns):.3f}"


def rows_for_file(path: Path, label: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in path.read_text(errors="replace").splitlines():
        if match := REQUESTER.search(line):
            row = fields(match.group("detail"))
            row.update(kind="requester", status=match.group("status"),
                       sent=match.group("sent"), received=match.group("received"))
            rows.append(row)
        elif match := REQUESTER_METRICS.search(line):
            row = fields(match.group("detail"))
            row["workload_kind"] = row.pop("kind", "-")
            row["kind"] = "requester"
            if rate := rate_mb_s(row.get("submitted_bytes"), row.get("submission_elapsed_ns")):
                row["submission_rate_mb_s"] = rate
            rows.append(row)
        elif match := HARNESS.search(line):
            row = fields(match.group("detail"))
            row["workload_kind"] = row.pop("kind", "-")
            row["kind"] = "harness"
            if rate := rate_mb_s(row.get("received_bytes"), row.get("receive_elapsed_ns")):
                row["receiver_goodput_mb_s"] = rate
            rows.append(row)
        elif match := MITM.search(line):
            row = fields(match.group("detail"))
            row["kind"] = "mitm"
            rows.append(row)
        elif match := MITM_WORKER.search(line):
            row = fields(match.group("detail"))
            row["kind"] = "mitm-worker"
            rows.append(row)
        elif match := WGNX.search(line):
            row = fields(match.group("detail"))
            row["kind"] = "wgnx"
            rows.append(row)
    for row in rows:
        row.setdefault("run", label)
        row.setdefault("workload", "-")
        row.setdefault("flow", row.get("fd", "-"))
    return rows


def render(rows: list[dict[str, str]]) -> None:
    columns = ["run", "kind", "workload_kind", "workload", "scope", "flow", "status", "sent", "attempted", "accepted", "submitted",
               "received", "submitted_bytes", "received_bytes", "submission_elapsed_ns", "submission_rate_mb_s",
               "receive_elapsed_ns", "receiver_goodput_mb_s", "echoed", "rtt_mean_ns", "rtt_p50_upper_ns",
               "rtt_p95_upper_ns", "rtt_p99_upper_ns", "rtt_max_ns", "sends", "adapter_queued",
               "adapter_queue_full", "send_admitted", "queue_full", "send_queue_full", "too_large",
               "queued", "discarded", "writable", "reason"]
    present = [column for column in columns if any(column in row for row in rows)]
    widths = {column: max(len(column), *(len(row.get(column, "")) for row in rows))
              for column in present}
    print("  ".join(column.ljust(widths[column]) for column in present))
    print("  ".join("-" * widths[column] for column in present))
    for row in rows:
        print("  ".join(row.get(column, "").ljust(widths[column])
                        for column in present))


def counter(row: dict[str, str], name: str) -> int:
    return int(row[name])


def check(rows: list[dict[str, str]]) -> tuple[list[str], int]:
    errors: list[str] = []
    mitm_checks = 0
    for row in rows:
        if row["kind"] == "mitm" and all(
            name in row for name in ("sends", "adapter_queued", "adapter_queue_full", "too_large", "accepted", "discarded", "queued")
        ):
            mitm_checks += 1
            sends = counter(row, "sends")
            adapter_queued = counter(row, "adapter_queued")
            adapter_queue_full = counter(row, "adapter_queue_full")
            too_large = counter(row, "too_large")
            if sends != adapter_queued + adapter_queue_full + too_large:
                errors.append(
                    f"MITM fd={row['flow']}: sends={sends} does not equal adapter_queued + adapter_queue_full + too_large"
                )
            accepted = counter(row, "accepted")
            discarded = counter(row, "discarded")
            queued = counter(row, "queued")
            if adapter_queued != accepted + discarded + queued:
                errors.append(
                    f"MITM fd={row['flow']}: adapter_queued={adapter_queued} does not equal accepted + discarded + queued"
                )
        if row["kind"] == "wgnx" and all(name in row for name in ("send_attempts", "send_admitted")):
            if counter(row, "send_admitted") > counter(row, "send_attempts"):
                errors.append(
                    f"WGNX flow={row['flow']}: send_admitted exceeds send_attempts"
                )
    return errors, mitm_checks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--label", default="run",
                        help="label applied to rows without a requester workload ID")
    parser.add_argument("--check", action="store_true",
                        help="check available per-flow accounting invariants")
    args = parser.parse_args()

    rows: list[dict[str, str]] = []
    for path in args.logs:
        if not path.is_file():
            parser.error(f"not a file: {path}")
        rows.extend(rows_for_file(path, args.label))
    if not rows:
        parser.error("no Task 4 aggregate summaries found")
    render(rows)
    if args.check:
        errors, mitm_checks = check(rows)
        for error in errors:
            print(f"CHECK failed: {error}")
        if errors:
            return 1
        if mitm_checks == 0:
            print("CHECK unavailable: no MITM flow summary includes the local-admission counters")
            return 2
        print(f"CHECK passed: {mitm_checks} MITM per-flow accounting invariant(s) hold")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
