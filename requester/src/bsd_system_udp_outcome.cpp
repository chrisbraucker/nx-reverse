#include "bsd_system_udp_outcome.hpp"

#include <poll.h>

namespace requester {

BsdPollObservation ClassifyBsdPollObservation(const int poll_result, const short revents) {
    if (poll_result < 0) {
        return BsdPollObservation::Error;
    }
    if (poll_result == 0) {
        return BsdPollObservation::Timeout;
    }
    if ((revents & POLLHUP) != 0) {
        return BsdPollObservation::Closed;
    }
    if ((revents & POLLIN) != 0) {
        return BsdPollObservation::Readable;
    }
    return BsdPollObservation::Unexpected;
}

bool IsExpectedBsdPollObservation(const BsdSystemUdpExpectedOutcome expected, const BsdPollObservation observation) {
    switch (expected) {
    case BsdSystemUdpExpectedOutcome::EchoReply:
        return observation == BsdPollObservation::Readable;
    case BsdSystemUdpExpectedOutcome::NoReplyTimeout:
        return observation == BsdPollObservation::Timeout;
    case BsdSystemUdpExpectedOutcome::TerminalClosure:
        return observation == BsdPollObservation::Closed;
    }
    return false;
}

bool HasWritableRecovery(const std::uint32_t queue_full_retries, const bool observed_pollout) {
    return queue_full_retries != 0 && observed_pollout;
}

} // namespace requester
