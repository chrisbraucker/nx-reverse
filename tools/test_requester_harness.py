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


class StallTcpHandlerTests(unittest.TestCase):
    def test_stall_handler_holds_the_established_stream_without_replying(self) -> None:
        class Request:
            def settimeout(self, timeout: float) -> None:
                del timeout

        handler = requester_harness.StallTcpHandler.__new__(requester_harness.StallTcpHandler)
        handler.request = Request()
        handler.client_address = ("10.0.0.2", 49152)
        with patch.object(requester_harness.time, "sleep") as sleep:
            handler.handle()
        sleep.assert_called_once_with(requester_harness.TCP_STALL_SECONDS)


if __name__ == "__main__":
    unittest.main()
