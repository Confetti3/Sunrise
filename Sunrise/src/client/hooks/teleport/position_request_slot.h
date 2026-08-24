#pragma once

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::teleport::position_request {

struct Request final {
    std::array<float, 3> destination{};
    std::uint64_t sequence{};
    std::uint64_t activitySession{};
    std::uint64_t activityRevision{};
};

/** Lock-external, allocation-free storage for one replaceable request. */
class Slot final {
public:
    [[nodiscard]] bool publish(const Request& request, Request* replaced = nullptr) noexcept {
        if (request.sequence == 0 || request.activitySession == 0
            || request.activityRevision == 0) {
            return false;
        }
        if (replaced != nullptr) {
            *replaced = pending_ ? request_ : Request{};
        }
        request_ = request;
        pending_ = true;
        return true;
    }

    [[nodiscard]] bool take(Request& request) noexcept {
        if (!pending_) {
            request = {};
            return false;
        }
        request = request_;
        request_ = {};
        pending_ = false;
        return true;
    }

    void clear() noexcept {
        request_ = {};
        pending_ = false;
    }

    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

private:
    Request request_{};
    bool pending_{};
};

} // namespace sunrise::client::hooks::teleport::position_request
