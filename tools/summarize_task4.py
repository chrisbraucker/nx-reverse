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
HARNESS = re.compile(r"\[udp-summary\] (?P<detail>.*)")
MITM = re.compile(r"tunnel flow summary (?P<detail>.*)")
MITM_WORKER = re.compile(r"tunnel worker summary (?P<detail>.*)")
WGNX = re.compile(r"Closed tunnel UDP (?P<detail>flow=.*send_attempts=.*)")


def fields(detail: str) -> dict[str, str]:
    return dict(KEY_VALUE.findall(detail))


def rows_for_file(path: Path, label: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in path.read_text(errors="replace").splitlines():
        if match := REQUESTER.search(line):
            row = fields(match.group("detail"))
            row.update(kind="requester", status=match.group("status"),
                       sent=match.group("sent"), received=match.group("received"))
            rows.append(row)
        elif match := HARNESS.search(line):
            row = fields(match.group("detail"))
            row["kind"] = "harness"
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
    columns = ["run", "kind", "workload", "flow", "status", "sent", "received",
               "echoed", "accepted", "send_admitted", "queue_full", "send_queue_full",
               "writable", "reason"]
    present = [column for column in columns if any(column in row for row in rows)]
    widths = {column: max(len(column), *(len(row.get(column, "")) for row in rows))
              for column in present}
    print("  ".join(column.ljust(widths[column]) for column in present))
    print("  ".join("-" * widths[column] for column in present))
    for row in rows:
        print("  ".join(row.get(column, "").ljust(widths[column])
                        for column in present))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--label", default="run",
                        help="label applied to rows without a requester workload ID")
    args = parser.parse_args()

    rows: list[dict[str, str]] = []
    for path in args.logs:
        if not path.is_file():
            parser.error(f"not a file: {path}")
        rows.extend(rows_for_file(path, args.label))
    if not rows:
        parser.error("no Task 4 aggregate summaries found")
    render(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
