#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace requester {

class UdpWorkloadMetrics {
  public:
    void RecordAttempt() {
        ++attempted_datagrams_;
    }

    void RecordSubmission(std::uint64_t tick, std::size_t payload_bytes) {
        if (submitted_datagrams_ == 0) {
            first_submission_tick_ = tick;
        }
        last_submission_tick_ = tick;
        ++submitted_datagrams_;
        submitted_bytes_ += payload_bytes;
    }

    void RecordEcho(std::uint64_t start_tick, std::uint64_t end_tick, std::uint64_t tick_frequency, std::size_t payload_bytes) {
        const std::uint64_t elapsed_ns = TicksToNanoseconds(end_tick - start_tick, tick_frequency);
        if (echoed_datagrams_ == 0 || elapsed_ns < rtt_min_ns_) {
            rtt_min_ns_ = elapsed_ns;
        }
        if (elapsed_ns > rtt_max_ns_) {
            rtt_max_ns_ = elapsed_ns;
        }
        rtt_total_ns_ += elapsed_ns;
        ++echoed_datagrams_;
        echoed_bytes_ += payload_bytes;
        ++rtt_histogram_[RttBucket(elapsed_ns)];
    }

    [[nodiscard]] std::uint32_t attempted_datagrams() const {
        return attempted_datagrams_;
    }
    [[nodiscard]] std::uint32_t submitted_datagrams() const {
        return submitted_datagrams_;
    }
    [[nodiscard]] std::uint64_t submitted_bytes() const {
        return submitted_bytes_;
    }
    [[nodiscard]] std::uint32_t echoed_datagrams() const {
        return echoed_datagrams_;
    }
    [[nodiscard]] std::uint64_t echoed_bytes() const {
        return echoed_bytes_;
    }
    [[nodiscard]] std::uint64_t SubmissionElapsedNanoseconds(std::uint64_t tick_frequency) const {
        if (submitted_datagrams_ < 2) {
            return 0;
        }
        return TicksToNanoseconds(last_submission_tick_ - first_submission_tick_, tick_frequency);
    }
    [[nodiscard]] std::uint64_t RttMinimumNanoseconds() const {
        return rtt_min_ns_;
    }
    [[nodiscard]] std::uint64_t RttMeanNanoseconds() const {
        return echoed_datagrams_ == 0 ? 0 : rtt_total_ns_ / echoed_datagrams_;
    }
    [[nodiscard]] double RttMeanMilliseconds() const {
        return static_cast<double>(RttMeanNanoseconds()) / 1000000.0;
    }
    [[nodiscard]] std::uint64_t RttMaximumNanoseconds() const {
        return rtt_max_ns_;
    }
    [[nodiscard]] std::uint64_t RttPercentileUpperNanoseconds(std::uint32_t percentile) const {
        if (echoed_datagrams_ == 0) {
            return 0;
        }
        const std::uint64_t target = (static_cast<std::uint64_t>(echoed_datagrams_) * percentile + 99U) / 100U;
        std::uint64_t observed = 0;
        for (std::size_t index = 0; index < rtt_histogram_.size(); ++index) {
            observed += rtt_histogram_[index];
            if (observed >= target) {
                return index + 1U == rtt_histogram_.size() ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << (index + 1U));
            }
        }
        return 0;
    }

    [[nodiscard]] static std::uint64_t TicksToNanoseconds(std::uint64_t ticks, std::uint64_t tick_frequency) {
        if (tick_frequency == 0) {
            return 0;
        }
        return (ticks / tick_frequency) * 1000000000ULL + ((ticks % tick_frequency) * 1000000000ULL) / tick_frequency;
    }

  private:
    [[nodiscard]] static std::size_t RttBucket(std::uint64_t elapsed_ns) {
        return elapsed_ns == 0 ? 0 : std::bit_width(elapsed_ns) - 1U;
    }

    std::uint64_t first_submission_tick_{};
    std::uint64_t last_submission_tick_{};
    std::uint64_t submitted_bytes_{};
    std::uint64_t echoed_bytes_{};
    std::uint64_t rtt_min_ns_{};
    std::uint64_t rtt_max_ns_{};
    std::uint64_t rtt_total_ns_{};
    std::uint32_t attempted_datagrams_{};
    std::uint32_t submitted_datagrams_{};
    std::uint32_t echoed_datagrams_{};
    std::array<std::uint32_t, 64> rtt_histogram_{};
};

} // namespace requester
