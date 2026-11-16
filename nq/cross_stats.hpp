#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace nq {

class RunningMoments {
public:
    static constexpr std::uint64_t kRefreshEvery = 1u << 16;

    void Update(std::span<const double> in, std::span<const std::uint32_t> changed) {
        if (last_.size() < in.size()) last_.resize(in.size(), std::numeric_limits<double>::quiet_NaN());
        for (const std::uint32_t row : changed) {
            const double was = last_[row];
            const double now = in[row];
            if (was == was) {
                sum_ -= was;
                sq_ -= was * was;
                --count_;
            }
            if (now == now) {
                sum_ += now;
                sq_ += now * now;
                ++count_;
            }
            last_[row] = now;
        }
        pending_ += changed.size();
        if (pending_ >= kRefreshEvery) Refresh();
    }

    std::uint64_t Count() const noexcept { return count_; }

    double Mean() const noexcept {
        return count_ ? sum_ / static_cast<double>(count_) : std::numeric_limits<double>::quiet_NaN();
    }

    double Variance() const noexcept {
        if (!count_) return std::numeric_limits<double>::quiet_NaN();
        const double mean = Mean();
        return std::max(sq_ / static_cast<double>(count_) - mean * mean, 0.0);
    }

    double Std() const noexcept { return std::sqrt(Variance()); }

private:
    void Refresh() noexcept {
        sum_ = 0.0;
        sq_ = 0.0;
        count_ = 0;
        for (const double x : last_) {
            if (x != x) continue;
            sum_ += x;
            sq_ += x * x;
            ++count_;
        }
        pending_ = 0;
    }

    std::vector<double> last_;
    double sum_ = 0.0;
    double sq_ = 0.0;
    std::uint64_t count_ = 0;
    std::uint64_t pending_ = 0;
};

class RunningRank {
public:
    void Update(std::span<const double> in, std::span<const std::uint32_t> changed) {
        if (key_.size() < in.size()) {
            key_.resize(in.size(), std::numeric_limits<double>::quiet_NaN());
            moved_.resize(in.size(), 0);
        }
        delta_.clear();
        std::size_t removed = 0;
        for (const std::uint32_t row : changed) {
            const double was = key_[row];
            const double now = in[row];
            if (was == now || (was != was && now != now)) continue;
            if (was == was) {
                moved_[row] = 1;
                ++removed;
            }
            key_[row] = now;
            if (now == now) delta_.push_back(row);
        }
        if (removed) {
            std::erase_if(order_, [&](std::uint32_t row) { return moved_[row] != 0; });
            for (const std::uint32_t row : changed) moved_[row] = 0;
        }
        if (delta_.empty()) return;

        const auto before = [&](std::uint32_t a, std::uint32_t b) {
            return key_[a] < key_[b] || (key_[a] == key_[b] && a < b);
        };
        std::sort(delta_.begin(), delta_.end(), before);
        merged_.resize(order_.size() + delta_.size());
        std::merge(order_.begin(), order_.end(), delta_.begin(), delta_.end(), merged_.begin(), before);
        order_.swap(merged_);
    }

    std::span<const std::uint32_t> Order() const noexcept { return order_; }
    std::size_t Count() const noexcept { return order_.size(); }

    void Write(std::span<double> out) const noexcept {
        std::fill(out.begin(), out.end(), std::numeric_limits<double>::quiet_NaN());
        const double last = order_.size() > 1 ? static_cast<double>(order_.size() - 1) : 1.0;
        for (std::size_t i = 0; i < order_.size(); ++i) out[order_[i]] = static_cast<double>(i) / last;
    }

private:
    std::vector<double> key_;
    std::vector<std::uint8_t> moved_;
    std::vector<std::uint32_t> order_;
    std::vector<std::uint32_t> delta_;
    std::vector<std::uint32_t> merged_;
};

}  // namespace nq
