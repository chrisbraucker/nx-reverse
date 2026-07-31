#pragma once

#include <cstdint>

namespace requester {

enum class BsdSystemUdpExpectedOutcome : std::uint8_t {
  EchoReply,
  NoReplyTimeout,
  TerminalClosure,
};

enum class BsdPollObservation : std::uint8_t {
  Timeout,
  Readable,
  Closed,
  Error,
  Unexpected,
};

[[nodiscard]] BsdPollObservation ClassifyBsdPollObservation(int poll_result,
                                                            short revents);
[[nodiscard]] bool
IsExpectedBsdPollObservation(BsdSystemUdpExpectedOutcome expected,
                             BsdPollObservation observation);
[[nodiscard]] bool HasWritableRecovery(std::uint32_t queue_full_retries,
                                       bool observed_pollout);

} // namespace requester
