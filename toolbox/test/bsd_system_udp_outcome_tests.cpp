#include <cassert>
#include <poll.h>

#include "bsd_system_udp_outcome.hpp"

int main() {
    using namespace toolbox;

    assert(ClassifyBsdPollObservation(0, 0) == BsdPollObservation::Timeout);
    assert(ClassifyBsdPollObservation(-1, 0) == BsdPollObservation::Error);
    assert(ClassifyBsdPollObservation(1, POLLIN) == BsdPollObservation::Readable);
    assert(ClassifyBsdPollObservation(1, POLLHUP) == BsdPollObservation::Closed);
    assert(ClassifyBsdPollObservation(1, POLLIN | POLLHUP) == BsdPollObservation::Closed);
    assert(ClassifyBsdPollObservation(1, POLLOUT) == BsdPollObservation::Unexpected);
    assert(IsExpectedBsdPollObservation(BsdSystemUdpExpectedOutcome::EchoReply, BsdPollObservation::Readable));
    assert(IsExpectedBsdPollObservation(BsdSystemUdpExpectedOutcome::NoReplyTimeout, BsdPollObservation::Timeout));
    assert(IsExpectedBsdPollObservation(BsdSystemUdpExpectedOutcome::TerminalClosure, BsdPollObservation::Closed));
    assert(!HasWritableRecovery(0, true));
    assert(!HasWritableRecovery(1, false));
    assert(HasWritableRecovery(1, true));
}
