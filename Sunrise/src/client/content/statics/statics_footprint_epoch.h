#pragma once

#include <cstdint>

namespace sunrise::client::content::statics {

/** Request-generation state; callers serialize access with the worker command mutex. */
class RequestEpochs final {
public:
    [[nodiscard]] std::uint64_t issue() noexcept {
        ++next_;
        if (next_ == 0) {
            next_ = 1;
        }
        return next_;
    }

    void activate(std::uint64_t epoch) noexcept {
        active_ = epoch;
        cancelled_ = false;
    }

    void cancel() noexcept {
        cancelled_ = true;
    }

    void reset_for_start() noexcept {
        active_ = 0;
        cancelled_ = false;
    }

    [[nodiscard]] bool cancelled(std::uint64_t epoch, bool stopping) const noexcept {
        return stopping || cancelled_ || active_ != epoch;
    }

    [[nodiscard]] bool
    publishable(std::uint64_t epoch, bool stopping, bool newerPending) const noexcept {
        return !stopping && !cancelled_ && active_ == epoch && !newerPending;
    }

    [[nodiscard]] bool has_active() const noexcept {
        return active_ != 0;
    }

private:
    std::uint64_t next_{};
    std::uint64_t active_{};
    bool cancelled_{};
};

} // namespace sunrise::client::content::statics
