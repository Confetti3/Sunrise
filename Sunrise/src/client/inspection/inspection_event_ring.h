#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace sunrise::client::inspection::capture {

/** Fixed-capacity overwrite ring with deterministic oldest-to-newest iteration. */
template <typename Value, std::size_t Capacity> class ChronologicalRing final {
    static_assert(Capacity != 0);

public:
    void push(Value value) {
        std::size_t slot = 0;
        if (count_ < Capacity) {
            slot = (head_ + count_) % Capacity;
            ++count_;
        } else {
            slot = head_;
            head_ = (head_ + 1U) % Capacity;
        }
        values_[slot] = std::move(value);
    }

    void clear() noexcept {
        for (auto& value : values_) {
            value.reset();
        }
        head_ = 0;
        count_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    template <typename Visitor> void for_each(Visitor&& visitor) const {
        for (std::size_t index = 0; index < count_; ++index) {
            visitor(*values_[(head_ + index) % Capacity]);
        }
    }

private:
    std::array<std::optional<Value>, Capacity> values_{};
    std::size_t head_{};
    std::size_t count_{};
};

} // namespace sunrise::client::inspection::capture
