<p align="center"><img src="https://capsule-render.vercel.app/api?type=soft&height=220&color=gradient&text=NQCPPSDK&fontSize=64&fontAlign=50&fontAlignY=42&desc=v0.1.1&descSize=22&descAlign=50&descAlignY=68" /></p>

<p align="center">
  <img alt="Version 0.1.1" src="https://img.shields.io/badge/version-0.1.1-blue" />
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" />
  <img alt="CMake 3.28+" src="https://img.shields.io/badge/CMake-3.28%2B-064F8C?logo=cmake&logoColor=white" />
  <img alt="Header-only" src="https://img.shields.io/badge/header--only-INTERFACE-4c1" />
  <img alt="C ABI" src="https://img.shields.io/badge/plugin-C%20ABI-orange" />
</p>

# NQCPPSDK

NQCPPSDK is the SDK for writing NQBook plugins. There are two kinds:

- **Feature**: computes numbers from an order book. The engine calls it once per event and reads it once per sample.
- **Adapter**: produces the `mbo` event stream. The engine calls its `Next` method repeatedly on a thread the engine owns.

The SDK is header-only, has no dependencies, and lives in namespace `nq`. A plugin includes only headers under `nq/` and never sees engine code. The engine calls the plugin through a C ABI. The content hash of each ABI header is compiled into the plugin. The engine refuses a plugin whose hash differs from its own.

Terms used below:

| Term | Meaning |
|---|---|
| instrument | one tradable thing with its own order book, identified by a `uint32` id the adapter assigns |
| resting order | an order that is already in the book |
| level | all resting orders at one price on one side: total quantity, order count, and the FIFO queue |
| snapshot column | a column that holds the value at the sampling instant |
| cumulative column | a column that holds a running total; NQBook writes the increase since the previous row |

## Setup

NQBook fetches the SDK from GitHub at a pinned tag. A plugin repository does the same:

```cmake
include(FetchContent)
FetchContent_Declare(nqcppsdk
    GIT_REPOSITORY https://github.com/Zhou-London/nqcppsdk.git
    GIT_TAG v0.1.0
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nqcppsdk)

nq_add_plugin(my_feature my_feature.cpp)
```

A checkout on disk works too: `add_subdirectory(path/to/nqcppsdk)`.

`nq_add_plugin` builds `my_feature.so` and exports only the plugin entry points. NQBook names the plugin after the file: `my_feature`.

## Writing a feature

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

NQBook names the output columns `my_feature.imb` and `my_feature.adds`.

A feature is a class with these members:

| Member | Role |
|---|---|
| `kColumns` | the output columns: name, unit, description, kind. `nq::MakeColumns` builds the array from an initializer list |
| `kRequires` | optional. The data fidelity the feature needs: `Requires::L1`, `MBP`, or `MBO`. Default `MBO`. NQBook records it in the schema and does not check it yet |
| constructor `(const nq::Params&)` | receives the parameters. Throw to reject them. The plugin then fails to load |
| `OnEvent(const Event&, const BookView&) noexcept` | called once per event. Optional. `FeatureBase` provides an empty one |
| `OnControl(const Control&) noexcept` | receives control events. `Reset` means the book was cleared. Optional |
| `Read(std::span<double>, const BookView&) const noexcept` | writes `kColumns.size()` values at sampling time |

`NQ_PLUGIN(A, B, C)` packs one or more feature classes into one plugin. The columns of `A` come first, then `B`, then `C`.

### Column kinds

| Kind | Meaning | What NQBook writes |
|---|---|---|
| `nq::kSnapshot` (default) | the value at the sampling instant | the value as is |
| `nq::kCumulative` | a running total that never decreases | the increase since the previous row |

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

Parameters come from the plugin's `.params` file and the command line. The engine creates one feature instance per instrument, so `Symbol()` differs per instance.

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

`Feed` hands the event to the feature. It leaves the mock book unchanged.

## Writing an adapter

```cpp
#include <nq/adapter.hpp>

class Replay : public nq::AdapterBase {
public:
    static constexpr const char* kName = "replay";
    static constexpr nq::Provides kProvides = nq::Provides::MBO;

    explicit Replay(const nq::Params& p) : reader_(std::string(p.Require("path"))) {
        for (const auto& r : reader_.Instruments())
            table_.push_back(nq::Instrument{r.id, r.symbol, r.tick_size, r.lot_size});
    }

    nq::Next Next(nq::Mbo& out) {
        nq_mbo raw;
        const int rc = reader_.Next(raw);
        if (rc == 1) { out = nq::Mbo(raw); return nq::Next::Event; }
        return rc == 0 ? nq::Next::End : nq::Next::Error;
    }

    const nq::Instrument* FindInstrument(std::uint32_t id) const noexcept {
        for (const auto& i : table_) if (i.id == id) return &i;
        return nullptr;
    }

private:
    nq::mbo_file::Reader reader_;
    std::vector<nq::Instrument> table_;
};

NQ_ADAPTER(Replay)
```

| Member | Role |
|---|---|
| `kName` | the adapter name |
| `kProvides` | optional. The fidelity of the stream. Default `MBO` |
| constructor `(const nq::Params&)` | opens the file or the connection. Throw to report failure |
| `Next(nq::Mbo&)` | produces one event. See the table below |
| `FindInstrument(id) const noexcept` | returns the instrument's metadata, or `nullptr` for an unknown id |

Return values of `Next`:

| Value | Meaning |
|---|---|
| `Next::Event` | `out` holds one event |
| `Next::Again` | no event yet. The engine asks again soon. A live source returns this instead of blocking |
| `Next::End` | the stream ended. Later calls also return `End` |
| `Next::Error` | the stream failed and ended |

An adapter is synchronous. The engine runs it on a thread the engine owns. The engine calls `Next` and `FindInstrument` on that thread only, never at the same time. `Next` must return within a bounded time. Return `Again` when no data is available, so the engine can act on a stop request.

### The mbo stream contract

The engine trusts the adapter on these points. `nqbook check-feed` verifies the ones it can:

- `ts` never decreases.
- `instrument` is a `uint32_t` id the adapter assigns. `FindInstrument(id)` must answer before the first event with that id.
- `id` identifies one order within the stream.
- `Add`: `qty` is the quantity of the new order.
- `Cancel`: `qty` is the quantity to remove. A `qty` of 0 removes the whole order. A `qty` larger than the remaining quantity also removes the whole order.
- `Modify`: `price` and `qty` are the new values. A price change moves the order to the back of the queue. A quantity increase also moves it to the back.
- `Trade`: `id` is the resting order, `side` is the side of the resting order, `qty` is the traded quantity. With `id` 0 the engine finds the level by `price` and reduces orders from the front of the queue.
- `Clear`: empties the book of that instrument. Send it after a sequence gap or a resubscribe.

Prices and quantities are fixed-point integers:

```
stored price    = round(price    * NQ_PRICE_SCALE)   NQ_PRICE_SCALE = 10000000000
stored quantity = round(quantity * NQ_QTY_SCALE)     NQ_QTY_SCALE   = 100000000
```

Example: the price 100.5 is stored as 1005000000000.

### Testing an adapter

```cpp
#include <nq/adapter_testing.hpp>

nq::ParamsBuilder pb;
pb.Set("path", "fixture.mbo");
nq::testing::AdapterSandbox<Replay> sb(pb.Build());
auto events = sb.Drain();            // pulls until End or Error
sb.LastStatus();                     // nq::Next::End
```

### The .mbo file

`nq/mbo_file.hpp` defines the replay file format with a reader and a writer. `Writer` takes the instrument table and then writes one `nq_mbo` per call. `Reader` returns the instrument table and yields one event per `Next`. The file describes itself. It needs no companion metadata file.

## Headers

| Header | Contents |
|---|---|
| `nq/abi.h` | shared C types: `nq_mbo`, `nq_params`, the side and action enums, the scale factors |
| `nq/feature_abi.h` | the feature C ABI |
| `nq/adapter_abi.h` | the adapter C ABI |
| `nq/feature.hpp` | every C++ type a feature needs, plus `NQ_PLUGIN` |
| `nq/adapter.hpp` | every C++ type an adapter needs, plus `NQ_ADAPTER` |
| `nq/feature_testing.hpp` `nq/adapter_testing.hpp` | test helpers that need no engine |
| `nq/mbo_file.hpp` | the replay file format |
| `nq/feature_host.hpp` `nq/adapter_host.hpp` | the engine-side loaders. Plugins never include them |

Which plugins a header change invalidates:

| Changed header | Invalidated plugins |
|---|---|
| `feature_abi.h` | feature plugins |
| `adapter_abi.h` | adapters |
| `abi.h` | both |

The engine rejects an invalidated plugin at load time with exit code 3. Rebuilding the plugin fixes it.

## Testing the SDK itself

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
