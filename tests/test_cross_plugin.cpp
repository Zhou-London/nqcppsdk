#include <cmath>
#include <limits>
#include <span>

#include <nq/cross.hpp>
#include <nq/cross_stats.hpp>

#include "test_features.hpp"

namespace nq_test {

struct MarketMid : nq::FeatureBase {
    static constexpr auto kColumns = nq::MakeColumns({
        {"market_mid", "price", "cross-sectional mean of mid at the last boundary"},
    });

    explicit MarketMid(const nq::Params&) {}

    void BindCross(nq::CrossRow row) noexcept { cross_ = row; }

    void Read(std::span<double> out, const nq::BookView&) const noexcept {
        out[0] = cross_ ? cross_[0] : std::numeric_limits<double>::quiet_NaN();
    }

private:
    nq::CrossRow cross_;
};

struct MidCross {
    static constexpr auto kInputs = nq::MakeInputs({"mid"});
    static constexpr auto kColumns = nq::MakeColumns({
        {"mean", "price", "cross-sectional mean of mid"},
        {"z", "", "cross-sectional z-score of mid"},
        {"rank", "", "cross-sectional rank of mid in [0, 1]"},
    });

    explicit MidCross(const nq::Params& p) : scale_(p.GetDouble("z_scale", 1.0)) {}

    void Apply(const nq::CrossView& view) noexcept {
        const auto mid = view.In(0);
        moments_.Update(mid, view.Changed());
        rank_.Update(mid, view.Changed());

        const double mean = moments_.Mean();
        const double sd = moments_.Std();
        const auto mean_out = view.Out(0);
        const auto z = view.Out(1);
        for (std::size_t r = 0; r < mid.size(); ++r) {
            mean_out[r] = mean;
            z[r] = scale_ * (mid[r] - mean) / sd;
        }
        rank_.Write(view.Out(2));
    }

private:
    double scale_;
    nq::RunningMoments moments_;
    nq::RunningRank rank_;
};

}  // namespace nq_test

NQ_PLUGIN(nq_test::Spread, nq_test::MarketMid)
NQ_CROSS(nq_test::MidCross)
