#include <cassert>

#include "udp_workload_metrics.hpp"

int main() {
    requester::UdpWorkloadMetrics metrics;
    metrics.RecordAttempt();
    metrics.RecordAttempt();
    metrics.RecordSubmission(100, 1200);
    metrics.RecordSubmission(400, 1200);
    assert(metrics.attempted_datagrams() == 2);
    assert(metrics.submitted_datagrams() == 2);
    assert(metrics.submitted_bytes() == 2400);
    assert(metrics.SubmissionElapsedNanoseconds(1000) == 300000000);

    metrics.RecordEcho(0, 1000, 1000000000, 1200);
    metrics.RecordEcho(0, 4000, 1000000000, 1200);
    metrics.RecordEcho(0, 8000, 1000000000, 1200);
    metrics.RecordEcho(0, 16000, 1000000000, 1200);
    assert(metrics.echoed_datagrams() == 4);
    assert(metrics.echoed_bytes() == 4800);
    assert(metrics.RttMinimumNanoseconds() == 1000);
    assert(metrics.RttMeanNanoseconds() == 7250);
    assert(metrics.RttMeanMilliseconds() == 0.00725);
    assert(metrics.RttMaximumNanoseconds() == 16000);
    assert(metrics.RttPercentileUpperNanoseconds(50) == 4096);
    assert(metrics.RttPercentileUpperNanoseconds(95) == 16384);
}
