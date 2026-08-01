#!/usr/bin/env python3

import unittest
from unittest.mock import patch

import requester_harness


def workload_datagram(flow: int, sequence: int) -> bytes:
    return b"NXRVWG1\0" + (7).to_bytes(4, "big") + flow.to_bytes(4, "big") + sequence.to_bytes(4, "big") + (9).to_bytes(4, "big")


class UdpWorkloadStatsTests(unittest.TestCase):
    def test_summary_includes_flow_and_workload_receive_windows(self) -> None:
        stats = requester_harness.UdpWorkloadStats()
        with patch.object(requester_harness.time, "monotonic_ns", side_effect=[100, 200, 300]):
            stats.record(workload_datagram(0, 0), "10.0.0.1:1", True)
            stats.record(workload_datagram(0, 1), "10.0.0.1:1", True)
            stats.record(workload_datagram(1, 0), "10.0.0.2:2", True)
        rows = {(row["scope"], str(row["flow"])): row for row in stats.summary_rows()}
        self.assertEqual(rows[("flow", "0")]["receive_elapsed_ns"], 100)
        self.assertEqual(rows[("flow", "0")]["unique"], 2)
        self.assertEqual(rows[("workload", "all")]["received"], 3)
        self.assertEqual(rows[("workload", "all")]["receive_elapsed_ns"], 200)


if __name__ == "__main__":
    unittest.main()
