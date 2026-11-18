# NQCPPSDK Manual

This manual is for people and automated harnesses that write NQ HPC plugins. It covers project setup, writing and testing a feature plugin, writing and testing an adapter plugin, and the API reference. The [NQ HPC README](https://github.com/Zhou-London/nqbook#readme) describes how to build and run the engine. People who change the SDK itself read [development.md](development.md).

## 1. Terms

| Term | Meaning |
|---|---|
| feed | the kind of record an adapter produces: MBO or OHLCV. One run of NQ HPC reads one feed |
| event | one MBO record: an add, cancel, modify, trade, or clear for one order |
| OHLCV record | one bar of one instrument: open, high, low, close, and volume over one period |
| warm-up bar | an OHLCV record the adapter hands to the features before the feed starts. It fills feature state and writes no row |
| instrument | one tradable thing, identified by a `uint32` id the adapter assigns. On an MBO feed it has its own order book |
| resting order | an order that is already in the book |
| level | all resting orders at one price on one side: total quantity, order count, and the FIFO queue |
| feature | a plugin class that computes numbers from the feed. NQ HPC creates one instance per instrument |
| column | one number a feature produces. NQ HPC names it `plugin.column`, where `plugin` is the `.so` file name without the extension |
| snapshot column | a column that holds the value at the sampling instant |
| cumulative column | a column that holds a running total; NQ HPC writes the change since the previous row |
| cross | a class in a feature plugin that computes numbers across all instruments at one instant. NQ HPC creates one instance per run |
| boundary | an instant on the time grid of a cross. NQ HPC runs the cross once per boundary |
| adapter | a plugin class that produces the feed |

### Feeds

| Feed | Record | Feature macro | Adapter macro | A feature reads |
|---|---|---|---|---|
| MBO | `nq::Mbo`: one order event | `NQ_PLUGIN` | `NQ_ADAPTER` | the event and the order book |
| OHLCV | `nq::Ohlcv`: one bar | `NQ_OHLCV_PLUGIN` | `NQ_OHLCV_ADAPTER` | the record |

An adapter produces one feed. A feature plugin reads one feed. NQ HPC refuses to run a feature plugin with an adapter of the other feed and exits with code 9. Sections 3 and 4 describe the MBO feed first, then the OHLCV feed.

## 2. Project setup

A plugin is its own CMake project. It fetches the SDK from GitHub at a pinned tag:

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_plugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

include(FetchContent)
FetchContent_Declare(nqcppsdk
    GIT_REPOSITORY https://github.com/Zhou-London/nqcppsdk.git
    GIT_TAG v0.4.0
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nqcppsdk)

nq_add_plugin(my_feature my_feature.cpp)
```

```bash
cmake -S . -B build
cmake --build build -j
nqhpc check --plugin build/my_feature.so
```

`nq_add_plugin` builds `my_feature.so` and exports only the plugin entry points. NQ HPC names the plugin `my_feature` and its columns `my_feature.<column>`.

Use the SDK tag your `nqhpc` was built with. NQ HPC pins it as the `GIT_TAG` of nqcppsdk in its `CMakeLists.txt`. `nqhpc` refuses a plugin built with another ABI at load time with exit code 3.

A checkout on disk works too: `add_subdirectory(path/to/nqcppsdk)`.

## 3. Writing a feature

```cpp
#include <cmath>

#include <nq/feature.hpp>

struct Imbalance : nq::FeatureBase {
    static constexpr auto kColumns = nq::MakeColumns({
        {"imb", "ratio", "bid qty / (bid + ask) at best"},
        {"adds", "count", "add events", nq::kCumulative},
    });

    explicit Imbalance(const nq::Params& p) : min_qty_(p.GetInt("min_qty", 0)) {}

    void OnEvent(const nq::Event& e, const nq::BookView&) noexcept {
        if (e.action == nq::Action::Add && e.qty >= min_qty_) ++adds_;
    }

    void OnControl(const nq::Control& c) noexcept {
        if (c.type == nq::ControlType::Reset) adds_ = 0;
    }

    void Read(std::span<double> out, const nq::BookView& book) const noexcept {
        const auto b = book.Best(nq::Side::Bid);
        const auto a = book.Best(nq::Side::Ask);
        out[0] = (b && a) ? double(b->qty) / double(b->qty + a->qty) : NAN;
        out[1] = double(adds_);
    }

private:
    std::int64_t min_qty_;
    std::int64_t adds_ = 0;
};

NQ_PLUGIN(Imbalance)
```

A feature is a class with these members:

| Member | Role |
|---|---|
| `kColumns` | the output columns: name, unit, description, kind. `nq::MakeColumns` builds the array from an initializer list |
| `kRequires` | optional. The data fidelity the feature needs: `Requires::L1`, `MBP`, or `MBO`. Default `MBO`. NQ HPC records it and does not check it yet |
| constructor `(const nq::Params&)` | receives the parameters. Throw to reject them. The plugin then fails to load |
| `OnEvent(const Event&, const BookView&) noexcept` | called once per event. Optional. `FeatureBase` provides an empty one |
| `OnControl(const Control&) noexcept` | receives control events. `Reset` means the book was cleared. Optional |
| `OnWarmup(const Ohlcv&) noexcept` | called once per warm-up bar, before the first event. Optional. See [Warm-up](#warm-up) |
| `Read(std::span<double>, const BookView&) const noexcept` | writes `kColumns.size()` values at sampling time |

`NQ_PLUGIN(A, B, C)` packs one or more feature classes into one plugin. The columns of `A` come first, then `B`, then `C`.

NQ HPC creates one instance of each feature class per instrument. An instance never sees events of another instrument.

### Column kinds

| Kind | Meaning | What NQ HPC writes |
|---|---|---|
| `nq::kSnapshot` (default) | the value at the sampling instant | the value as is |
| `nq::kCumulative` | a running total. It may rise or fall | the change since the previous row |

A cumulative column lets one feature serve several sampling rates. A 1-second sampler and a 1-minute sampler read the same total. Each takes its own difference.

### Three rules

- `OnEvent` does O(1) accumulation only. Reading the book and computing ratios belong in `Read`. `Read` runs only at sampling time.
- No wall clock, no random numbers, no I/O. Two runs over the same data must produce identical output.
- A `kCumulative` column resets to zero on `OnControl(Reset)`.

### Event

| Field | Meaning |
|---|---|
| `ts` | exchange event time, UTC nanoseconds |
| `id` | the order id. On a trade, the id of the resting order. 0 when unknown |
| `price` `qty` | fixed-point integers. The scale factors come from `Params` |
| `filled` | the quantity the book removed on a trade. 0 for other events |
| `side` | the side of the order. On a trade, the side of the resting order |
| `action` | `Add`, `Cancel`, `Modify`, or `Trade` |

`Clear` never arrives as an `Event`. It arrives as `OnControl(Reset)`.

### BookView

| Method | Returns |
|---|---|
| `Best(side)` | the best level as `std::optional<Level>`. `nullopt` when the side is empty |
| `GetLevel(side, k)` | the k-th level, k from 0. Costs O(k) |
| `Ts()` | the `ts` of the last event the book applied |

`Level` has `price`, `qty`, and `count`.

### Params

| Method | Returns |
|---|---|
| `Symbol()` `TickSize()` `LotSize()` | the symbol, tick size, and lot size of the instrument this instance serves |
| `PriceScale()` `QtyScale()` | the fixed-point scale factors. `price / PriceScale()` is the floating-point price |
| `Get(key)` | `std::optional<std::string_view>` |
| `GetInt(key, fallback)` `GetDouble` `GetBool` | the parsed value, or the fallback when the key is absent |
| `Require(key)` `RequireInt` `RequireDouble` | the parsed value. Throws when the key is absent or malformed |

Parameters come from the plugin's `.params` file and from the `nqhpc` command line.

### An OHLCV feature

```cpp
#include <cmath>

#include <nq/feature.hpp>

struct Candle : nq::FeatureBase {
    static constexpr auto kColumns = nq::MakeColumns({
        {"close", "price", "close of the last record"},
        {"volume", "qty", "traded volume", nq::kCumulative},
    });

    explicit Candle(const nq::Params& p) : scale_(double(p.PriceScale())) {}

    void OnOhlcv(const nq::Ohlcv& ohlcv) noexcept {
        close_ = double(ohlcv.close) / scale_;
        volume_ += ohlcv.volume;
    }

    void OnControl(const nq::Control& c) noexcept {
        if (c.type == nq::ControlType::Reset) volume_ = 0.0;
    }

    void Read(std::span<double> out) const noexcept {
        out[0] = close_;
        out[1] = volume_;
    }

private:
    double scale_;
    double close_ = NAN;
    double volume_ = 0.0;
};

NQ_OHLCV_PLUGIN(Candle)
```

An OHLCV feature differs from an MBO feature in these members:

| Member | Role |
|---|---|
| `OnOhlcv(const Ohlcv&) noexcept` | called once per record. Optional. `FeatureBase` provides an empty one |
| `Read(std::span<double>) const noexcept` | writes `kColumns.size()` values at sampling time. It takes no `BookView`: an OHLCV feed has no order book |
| `OnControl(const Control&) noexcept` | optional. Only an MBO `Clear` causes a `Reset`, so an OHLCV feature receives none |
| `kRequires` | unused |

`NQ_OHLCV_PLUGIN(A, B, C)` packs OHLCV features into one plugin. A plugin holds features of one feed. A plugin that mixes both feeds fails to compile.

Column kinds, the three rules, and `Params` are the same for both feeds.

`nq::Ohlcv`:

| Field | Meaning |
|---|---|
| `ts` | start of the period, UTC nanoseconds |
| `open` `high` `low` `close` | fixed-point prices. `price / PriceScale()` is the floating-point price |
| `volume` | the quantity traded in the period, a `double` in whole units |
| `instrument` | the instrument id |

### Warm-up

A feature that needs history defines `OnWarmup`. This MBO feature holds the mean close of the last 20 daily bars:

```cpp
#include <array>
#include <cmath>

#include <nq/feature.hpp>

struct DayMean : nq::FeatureBase {
    static constexpr std::size_t kDays = 20;
    static constexpr auto kColumns = nq::MakeColumns({
        {"mean20", "price", "mean close of the last 20 daily bars"},
    });

    explicit DayMean(const nq::Params& p) : scale_(double(p.PriceScale())) {}

    void OnWarmup(const nq::Ohlcv& day) noexcept {
        closes_[days_++ % kDays] = double(day.close) / scale_;
    }

    void Read(std::span<double> out, const nq::BookView&) const noexcept {
        double sum = 0.0;
        for (const double close : closes_) sum += close;
        out[0] = days_ >= kDays ? sum / kDays : NAN;
    }

private:
    double scale_;
    std::array<double, kDays> closes_{};
    std::size_t days_ = 0;
};

NQ_PLUGIN(DayMean)
```

NQ HPC calls `OnWarmup` once per warm-up bar of the instrument. Every call comes before the first record of the feed. A feature of either feed may define `OnWarmup`. A feature without it ignores the warm-up bars.

- The adapter chooses the warm-up bars: which instruments, how many bars, and which period. The feature must know the period the adapter sends.
- Warm-up writes no rows. A cumulative column counts from its value at the end of warm-up, so the first row holds only the change since then.
- Write NaN while the feature has too few bars. A run may send fewer bars than the feature needs, or none.
- Keep the warm-up state on `OnControl(Reset)`. NQ HPC sends warm-up bars once, at the start of the run.
- Warm-up happens once. A feature that runs across days adds each finished day to its history itself.

### A cross

A cross reads one or more columns of its own plugin for every instrument and writes new columns. A feature of the same plugin can read those columns back.

```cpp
#include <nq/cross.hpp>
#include <nq/cross_stats.hpp>

struct MidCross {
    static constexpr auto kInputs = nq::MakeInputs({"mid"});
    static constexpr auto kColumns = nq::MakeColumns({
        {"mean", "price", "cross-sectional mean of mid"},
        {"rank", "", "cross-sectional rank of mid in [0, 1]"},
    });

    explicit MidCross(const nq::Params&) {}

    void Apply(const nq::CrossView& view) noexcept {
        const auto mid = view.In(0);
        moments_.Update(mid, view.Changed());
        rank_.Update(mid, view.Changed());

        const auto mean = view.Out(0);
        for (std::size_t r = 0; r < mid.size(); ++r) mean[r] = moments_.Mean();
        rank_.Write(view.Out(1));
    }

private:
    nq::RunningMoments moments_;
    nq::RunningRank rank_;
};

NQ_PLUGIN(Mid)
NQ_CROSS(MidCross)
```

| Member | Role |
|---|---|
| `kInputs` | names of snapshot columns of this plugin, without the plugin prefix. `nq::MakeInputs` builds the array |
| `kColumns` | the output columns. NQ HPC names them `cross.column`, where `cross` is the name given on the command line |
| constructor `(const nq::Params&)` | receives the plugin's parameters. `Symbol()` is empty |
| `Apply(const CrossView&) noexcept` | called once per boundary |

`NQ_CROSS(C)` goes after `NQ_PLUGIN` or `NQ_OHLCV_PLUGIN` in the same file. A plugin holds at most one cross.

`nqhpc run --cross NAME=PLUGIN:EVERY` runs the cross of `PLUGIN` every `EVERY`, for example `--cross x=my_feature:1s`. NQ HPC writes one table named `NAME`. Each row holds one instrument at one boundary: the input columns, then the output columns. The table holds the instruments that had a record since the previous boundary. With `--fill-gaps` the table holds every valid instrument at every boundary.

`CrossView` is the panel at one boundary. Each instrument owns one row for the whole run.

| Method | Returns |
|---|---|
| `Ts()` | the boundary, UTC nanoseconds |
| `RowCount()` | the number of rows |
| `In(c)` | input column `c` as `std::span<const double>`, one value per row |
| `Out(c)` | output column `c` as `std::span<double>`. NQ HPC keeps the values between boundaries |
| `Valid(row)` | true once the instrument of `row` has had a boundary |
| `Changed()` | the rows that had a record since the previous boundary |

Rules for `Apply`:

- A record with `ts` equal to the boundary belongs to the next boundary.
- A row outside `Changed()` keeps its input values from its last boundary.
- An input is NaN until the row is valid, and whenever the feature wrote NaN. Skip NaN inputs.
- Cost should follow `Changed().size()`, not `RowCount()`. `nq/cross_stats.hpp` holds two helpers that do so.

| Helper | Keeps | Cost per `Update` |
|---|---|---|
| `nq::RunningMoments` | count, mean, population variance and standard deviation of the non-NaN values | O(changed) |
| `nq::RunningRank` | the rows sorted by value, ties by row. `Write(out)` writes ranks in [0, 1] and NaN for NaN inputs | O(rows + changed log changed) |

A feature reads the cross output through `BindCross`:

```cpp
struct Relative : nq::FeatureBase {
    static constexpr auto kColumns = nq::MakeColumns({{"market_mid", "price", "mean of mid across instruments"}});

    explicit Relative(const nq::Params&) {}

    void BindCross(nq::CrossRow row) noexcept { cross_ = row; }

    void Read(std::span<double> out, const nq::BookView&) const noexcept {
        out[0] = cross_ ? cross_[0] : std::numeric_limits<double>::quiet_NaN();
    }

private:
    nq::CrossRow cross_;
};
```

`cross_[c]` is output column `c` of this instrument at the last boundary. The value is NaN before the first boundary. `cross_` is false when the run has no `--cross` for the plugin. NQ HPC may call `BindCross` again at any time, so store the `CrossRow` and read through it. A feature may read `cross_` in `OnEvent`, `OnOhlcv`, and `Read`. A sampler row at a boundary sees the cross output of that boundary.

### Testing a feature

`nq/feature_testing.hpp` needs no engine:

```cpp
#include <nq/feature_testing.hpp>

nq::ParamsBuilder pb;
pb.PriceScale(100).QtyScale(10).Set("min_qty", "5");

nq::testing::Sandbox<Imbalance> sb(pb.Build());
sb.Book().SetBids({{10050, 30, 1}}).SetAsks({{10070, 10, 1}});   // {price, qty, count}, best level first
sb.Feed(nq::testing::Add(1, 1, nq::Side::Bid, 10050, 7));        // ts, id, side, price, qty
auto v = sb.Read();   // v[0] == 0.75, v[1] == 1
```

`Feed` hands the event to the feature. It leaves the mock book unchanged. `sb.Warmup(bar)` hands a warm-up bar to `OnWarmup`.

An OHLCV feature runs in the same `Sandbox`:

```cpp
nq::ParamsBuilder pb;
pb.PriceScale(100);

nq::testing::Sandbox<Candle> sb(pb.Build());
sb.Feed(nq::testing::MakeOhlcv(60, 10000, 10250, 9900, 10100, 1.5));   // ts, open, high, low, close, volume
auto v = sb.Read();   // v[0] == 101.0, v[1] == 1.5
```

## 4. Writing an adapter

This adapter produces `count` add events for one instrument:

```cpp
#include <cstdint>

#include <nq/adapter.hpp>

class Ticks : public nq::AdapterBase {
public:
    static constexpr const char* kName = "ticks";

    explicit Ticks(const nq::Params& p) : count_(p.RequireInt("count")) {}

    nq::Next Next(nq::Mbo& out) {
        if (next_ >= count_) return nq::Next::End;
        const std::int64_t i = next_++;
        out.ts = i + 1;
        out.id = static_cast<std::uint64_t>(i + 1);
        out.instrument = 1;
        out.side = i % 2 ? nq::Side::Ask : nq::Side::Bid;
        out.action = nq::Action::Add;
        out.price = 1000 + i;
        out.qty = 1;
        return nq::Next::Event;
    }

    const nq::Instrument* FindInstrument(std::uint32_t id) const noexcept {
        return id == 1 ? &instrument_ : nullptr;
    }

private:
    std::int64_t count_;
    std::int64_t next_ = 0;
    nq::Instrument instrument_{1, "DEMO", 10, 1};
};

NQ_ADAPTER(Ticks)
```

Build it with the same `nq_add_plugin` as a feature.

| Member | Role |
|---|---|
| `kName` | the adapter name |
| `kProvides` | optional. The fidelity of the stream: `Provides::L1`, `MBP`, or `MBO`. Default `MBO` |
| constructor `(const nq::Params&)` | opens the file or the connection. Throw to report failure |
| `Next(nq::Mbo&)` | produces one event. See the table below |
| `Warmup(nq::Ohlcv&)` | optional. Produces one warm-up bar. See [Warm-up bars](#warm-up-bars) |
| `FindInstrument(id) const noexcept` | returns the instrument's metadata, or `nullptr` for an unknown id |

Return values of `Next`:

| Value | Meaning |
|---|---|
| `Next::Event` | `out` holds one event |
| `Next::Again` | no event yet. NQ HPC asks again soon. A live source returns this instead of blocking |
| `Next::End` | the stream ended. Later calls also return `End` |
| `Next::Error` | the stream failed and ended |

An adapter is synchronous. NQ HPC calls `Next` and `FindInstrument` from one thread, never at the same time. `Next` must return within a bounded time. Return `Again` when no data is available, so a stop request can take effect.

`nq::Mbo` has the fields `ts`, `id`, `price`, `qty`, `instrument`, `side`, and `action`. `nq::Instrument` has `id`, `symbol`, `tick_size`, and `lot_size`.

### The event stream contract

NQ HPC trusts the adapter on these points. `nqhpc check-feed` verifies the ones it can:

- `ts` never decreases.
- `instrument` is a `uint32_t` id the adapter assigns. `FindInstrument(id)` must answer before the first event with that id.
- `id` identifies one order within the stream.
- `Add`: `qty` is the quantity of the new order.
- `Cancel`: `qty` is the quantity to remove. A `qty` of 0 removes the whole order. A `qty` larger than the remaining quantity also removes the whole order.
- `Modify`: `price` and `qty` are the new values. A price change moves the order to the back of the queue. A quantity increase also moves it to the back.
- `Trade`: `id` is the resting order, `side` is the side of the resting order, `qty` is the traded quantity. With `id` 0 NQ HPC finds the level by `price` and reduces orders from the front of the queue.
- `Clear`: empties the book of that instrument. Send it after a sequence gap or a resubscribe.

Prices and quantities are fixed-point integers:

```
stored price    = round(price    * NQ_PRICE_SCALE)   NQ_PRICE_SCALE = 10000000000
stored quantity = round(quantity * NQ_QTY_SCALE)     NQ_QTY_SCALE   = 100000000
```

Example: the price 100.5 is stored as 1005000000000.

### An OHLCV adapter

This adapter produces `count` one-minute records for one instrument:

```cpp
#include <cstdint>

#include <nq/adapter.hpp>

class Bars : public nq::AdapterBase {
public:
    static constexpr const char* kName = "bars";

    explicit Bars(const nq::Params& p) : count_(p.RequireInt("count")), scale_(p.PriceScale()) {}

    nq::Next Next(nq::Ohlcv& out) {
        if (next_ >= count_) return nq::Next::End;
        const std::int64_t i = next_++;
        out.ts = i * 60'000'000'000;
        out.open = (100 + i) * scale_;
        out.high = out.open + 2 * scale_;
        out.low = out.open - scale_;
        out.close = out.open + scale_;
        out.volume = 12.5;
        out.instrument = 1;
        return nq::Next::Event;
    }

    const nq::Instrument* FindInstrument(std::uint32_t id) const noexcept {
        return id == 1 ? &instrument_ : nullptr;
    }

private:
    std::int64_t count_;
    std::int64_t scale_;
    std::int64_t next_ = 0;
    nq::Instrument instrument_{1, "DEMO", 10, 1};
};

NQ_OHLCV_ADAPTER(Bars)
```

An OHLCV adapter has the members of an MBO adapter, with these differences:

| Member | Role |
|---|---|
| `Next(nq::Ohlcv&)` | produces one OHLCV record. The return values are the same |
| `kProvides` | unused |

An adapter class has one `Next`. A class with both overloads fails to compile.

### The OHLCV feed contract

- `ts` is the start of the record's period. It never decreases.
- `instrument` follows the MBO rule: `FindInstrument(id)` must answer before the first record with that id.
- `open`, `high`, `low`, and `close` are fixed-point prices scaled by `NQ_PRICE_SCALE`.
- `volume` is a `double` in whole units. It has no scale factor.
- NQ HPC builds no order book from an OHLCV feed. Each record goes to the features of its instrument.

### Warm-up bars

An adapter of either feed may define `Warmup`. This member sends the last `days_` daily bars the adapter loaded in its constructor:

```cpp
nq::Next Warmup(nq::Ohlcv& out) {
    if (next_day_ == days_.size()) return nq::Next::End;
    out = days_[next_day_++];
    return nq::Next::Event;
}
```

NQ HPC calls `Warmup` until it returns `End`, then starts calling `Next`. The return values mean the same as for `Next`. `AdapterBase` provides a `Warmup` that returns `End` at once.

- A warm-up bar is an `nq::Ohlcv` with the fields of the OHLCV feed contract. The bars may have any period and come in any order.
- `instrument` uses the ids of the feed. `FindInstrument(id)` must answer before the first warm-up bar with that id.
- Send only bars from before the first record of the feed. NQ HPC does not check the timestamps.
- NQ HPC ends all `Warmup` calls before the first `Next` call. `Warmup` and `Next` may run on different threads, never at the same time.
- `Error` ends the run with exit code 7 before the feed starts.

### Testing an adapter

```cpp
#include <nq/adapter_testing.hpp>

nq::ParamsBuilder pb;
pb.Set("count", "5");
nq::testing::AdapterSandbox<Ticks> sb(pb.Build());
auto events = sb.Drain();            // pulls until End or Error; 5 events here
sb.LastStatus();                     // nq::Next::End
```

`AdapterSandbox` runs adapters of both feeds. `Drain` returns `nq::Mbo` events or `nq::Ohlcv` records. `DrainWarmup` returns the warm-up bars.

## 5. Headers

| Header | Contents |
|---|---|
| `nq/feature.hpp` | every C++ type a feature needs, plus `NQ_PLUGIN` and `NQ_OHLCV_PLUGIN` |
| `nq/cross.hpp` | `CrossView`, `MakeInputs`, and `NQ_CROSS`. Includes `nq/feature.hpp` |
| `nq/cross_stats.hpp` | `RunningMoments` and `RunningRank` |
| `nq/adapter.hpp` | every C++ type an adapter needs, plus `NQ_ADAPTER` and `NQ_OHLCV_ADAPTER` |
| `nq/feature_testing.hpp` `nq/adapter_testing.hpp` | test helpers that need no engine |
| `nq/abi.h` `nq/feature_abi.h` `nq/adapter_abi.h` | the C ABI. Include these directly only when writing a plugin in C |
| `nq/feature_host.hpp` `nq/adapter_host.hpp` | the engine-side loaders. Plugins never include them |

## 6. Checklist for a harness

1. Pin the SDK tag that NQ HPC pins as the `GIT_TAG` of nqcppsdk in its `CMakeLists.txt`.
2. Unit-test the plugin with `feature_testing.hpp` or `adapter_testing.hpp`. No engine is needed.
3. `nqhpc check --plugin build/x.so --format json`: exit code 0, the expected `feed`, and the expected column list.
4. `nqhpc run` with the plugin and read the Parquet output. `nqhpc help run` lists the options.

Exit codes of `nqhpc`:

| Code | Meaning |
|---|---|
| 0 | success |
| 2 | a plugin or adapter `.so` failed to load |
| 3 | a plugin or adapter was built with another ABI |
| 4 | a plugin or adapter constructor threw |
| 5 | a sampler or cross spec is invalid |
| 6 | a `.params` file or a `KEY=VALUE` parameter is invalid |
| 7 | the adapter stream ended with `Next::Error` |
| 8 | the output files could not be written |
| 9 | the plugins and the adapter do not all read one feed |
| 130 | the run was interrupted |
