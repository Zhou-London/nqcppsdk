#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include <nq/cross.hpp>
#include <nq/cross_stats.hpp>
#include <nq/feature_host.hpp>
#include <nq/feature_testing.hpp>

#include "check.hpp"

static const double kNaN = std::numeric_limits<double>::quiet_NaN();

static bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
static bool Same(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }

static int TestStats() {
    std::mt19937_64 rng(11);
    std::vector<double> values;
    nq::RunningMoments moments;
    nq::RunningRank rank;
    CHECK(moments.Count() == 0 && std::isnan(moments.Mean()) && std::isnan(moments.Std()) && rank.Count() == 0);

    for (int round = 0; round < 400; ++round) {
        if (round % 20 == 0) values.resize(values.size() + 37, kNaN);
        std::vector<std::uint32_t> changed;
        std::vector<std::uint8_t> picked(values.size(), 0);
        const std::size_t want = 1 + rng() % 25;
        while (changed.size() < want) {
            const auto row = static_cast<std::uint32_t>(rng() % values.size());
            if (picked[row]) continue;
            picked[row] = 1;
            changed.push_back(row);
            const std::uint64_t kind = rng() % 10;
            if (kind == 0) values[row] = kNaN;
            else if (kind == 1) values[row] = 5.0;
            else if (kind >= 2 && kind < 4) continue;
            else values[row] = static_cast<double>(rng() % 100000) / 100.0;
        }
        moments.Update(values, changed);
        rank.Update(values, changed);

        double sum = 0.0;
        double sq = 0.0;
        std::vector<std::uint32_t> order;
        for (std::uint32_t r = 0; r < values.size(); ++r) {
            if (std::isnan(values[r])) continue;
            sum += values[r];
            sq += values[r] * values[r];
            order.push_back(r);
        }
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            return values[a] < values[b] || (values[a] == values[b] && a < b);
        });
        const double n = static_cast<double>(order.size());
        CHECK(moments.Count() == order.size());
        if (!order.empty()) {
            CHECK(Near(moments.Mean(), sum / n, 1e-7));
            CHECK(Near(moments.Variance(), sq / n - (sum / n) * (sum / n), 1e-4));
        }
        CHECK(rank.Count() == order.size() && std::equal(order.begin(), order.end(), rank.Order().begin()));

        std::vector<double> out(values.size(), 7.0);
        rank.Write(out);
        for (std::size_t i = 0; i < order.size(); ++i) {
            CHECK(Near(out[order[i]], order.size() > 1 ? static_cast<double>(i) / (n - 1.0) : 0.0));
        }
        for (std::uint32_t r = 0; r < values.size(); ++r) CHECK(std::isnan(out[r]) == std::isnan(values[r]));
    }

    nq::RunningMoments drift;
    std::vector<double> big(8, 1e6);
    std::vector<std::uint32_t> all{0, 1, 2, 3, 4, 5, 6, 7};
    drift.Update(big, all);
    for (std::uint64_t i = 0; i < 2 * nq::RunningMoments::kRefreshEvery / 8; ++i) {
        for (double& x : big) x = 1e6 + static_cast<double>(rng() % 1000) / 1000.0;
        drift.Update(big, all);
    }
    double sum = 0.0;
    for (const double x : big) sum += x;
    CHECK(Near(drift.Mean(), sum / 8.0, 1e-6));
    return 0;
}

static int TestPlugin() {
    nq::host::Plugin plain(NQ_TEST_PLUGIN_PATH);
    CHECK(!plain.HasCross() && plain.Fns().cross_bind == nullptr);

    nq::host::Plugin plugin(NQ_TEST_CROSS_PLUGIN_PATH);
    CHECK(plugin.HasCross() && plugin.Schema().ncol == 3);
    CHECK(plugin.CrossSchema().nin == 1 && plugin.CrossSchema().nout == 3);
    CHECK(std::strcmp(plugin.CrossInputs()[0], "mid") == 0);
    CHECK(std::strcmp(plugin.CrossColumns()[0].name, "mean") == 0 && std::strcmp(plugin.CrossColumns()[2].name, "rank") == 0);
    CHECK(plugin.Fns().cross_bind != nullptr);

    bool threw = false;
    try { plain.CreateCross(*nq::ParamsBuilder().Build()); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    nq::ParamsBuilder bad;
    bad.Set("z_scale", "abc");
    threw = false;
    try { plugin.CreateCross(*bad.Build()); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    nq::ParamsBuilder pb;
    pb.Symbol("TEST").PriceScale(100).Set("z_scale", "2");
    nq::host::CrossInstance cross = plugin.CreateCross(*pb.Build());
    nq::host::PluginInstance inst = plugin.Create(*pb.Build());
    nq::testing::MockBook book;

    constexpr std::uint32_t stride = 64;
    std::vector<double> in(stride, kNaN);
    std::vector<double> out(3 * stride, kNaN);
    std::vector<std::uint64_t> valid{0b111};
    std::vector<std::uint32_t> changed{0, 1, 2};
    in[0] = 101.0;
    in[1] = 51.0;

    std::vector<double> row(3);
    inst.Read(row, book.Raw());
    CHECK(std::isnan(row[2]));
    inst.BindCross(out.data() + 1, stride);
    inst.Read(row, book.Raw());
    CHECK(std::isnan(row[2]));

    nq_cross_panel panel{1000, 3, stride, in.data(), out.data(), valid.data(), changed.data(), 3};
    const nq::CrossView view(&panel);
    CHECK(view.Ts() == 1000 && view.RowCount() == 3 && view.Changed().size() == 3 && view.Valid(2) && !view.Valid(3));
    CHECK(view.In(0).data() == in.data() && view.Out(2).data() == out.data() + 2 * stride);

    cross.Apply(panel);
    CHECK(Near(out[0], 76.0) && Near(out[1], 76.0) && Near(out[2], 76.0));
    CHECK(Near(out[stride], 2.0) && Near(out[stride + 1], -2.0) && std::isnan(out[stride + 2]));
    CHECK(Same(out[2 * stride], 1.0) && Same(out[2 * stride + 1], 0.0) && std::isnan(out[2 * stride + 2]));
    inst.Read(row, book.Raw());
    CHECK(Near(row[2], 76.0));

    in[2] = 28.0;
    changed = {2};
    panel.changed = changed.data();
    panel.nchanged = 1;
    cross.Apply(panel);
    CHECK(Near(out[0], 60.0) && Same(out[2 * stride], 1.0) && Same(out[2 * stride + 1], 0.5) && Same(out[2 * stride + 2], 0.0));
    inst.Read(row, book.Raw());
    CHECK(Near(row[2], 60.0));

    nq::host::PluginInstance other = plain.Create(*pb.Build());
    other.BindCross(out.data(), stride);
    return 0;
}

int main() {
    if (int r = TestStats()) return r;
    if (int r = TestPlugin()) return r;
    std::printf("ok\n");
    return 0;
}
