# NQCPPSDK

NQCPPSDK is the SDK for writing NQBook plugins. There are two kinds:

- **Feature**: computes features from an order book. The engine calls it once per event and reads it once per sample.
- **Adapter**: produces the `mbo` event stream. The engine calls `Next` repeatedly on a thread of its own.

The SDK is header-only, has no dependencies, and lives in namespace `nq`. A plugin includes only headers under `nq/` and never sees engine code. The engine and the plugin meet at a C ABI. The content hash of each ABI header is compiled into the plugin, and the engine refuses a plugin whose hash differs from its own.

## Setup

```cmake
add_subdirectory(path/to/nqcppsdk)
nq_add_plugin(my_feature my_feature.cpp)
```

`nq_add_plugin` builds `my_feature.so` and exports only the plugin entry points.

## Writing a feature

```cpp
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
| `kColumns` | output columns: name, unit, description, kind. `nq::MakeColumns` builds the array from an initializer list |
| `kRequires` | optional data fidelity the feature needs: `Requires::L1`, `MBP`, or `MBO`; defaults to `MBO` |
| constructor `(const nq::Params&)` | receives parameters. A thrown exception marks the parameters invalid and the plugin fails to load |
| `OnEvent(const Event&, const BookView&) noexcept` | called once per event. Optional; `FeatureBase` provides an empty one |
| `OnControl(const Control&) noexcept` | receives control events; `Reset` means the book was cleared. Optional |
| `Read(std::span<double>, const BookView&) const noexcept` | writes `kColumns.size()` values at sampling time |

`NQ_PLUGIN(A, B, C)` packs one or more feature classes into one plugin. Columns concatenate in class order.

### Column kinds

| Kind | Meaning | Engine behavior |
|---|---|---|
| `nq::kSnapshot` (default) | the value at the sampling instant | emitted as is |
| `nq::kCumulative` | a monotone running total | emitted as the difference from the previous row |

Cumulative columns let one feature state serve several sampling rates: a 1-second sampler and a 1-minute sampler read the same total and each takes its own difference.

### Three rules

- `OnEvent` does O(1) accumulation only. Reading the book and computing ratios belong in `Read`, which runs only at sampling time.
- No wall clock, no random numbers, no I/O. Two runs over the same data must produce identical output.
- `kCumulative` columns reset to zero on `OnControl(Reset)`.

### Event

| Field | Meaning |
|---|---|
| `ts` | exchange event time, UTC nanoseconds |
| `id` | order id. On a trade, the resting order's id; 0 when unknown |
| `price` `qty` | fixed-point integers; scale factors come from `Params` |
| `filled` | quantity the book actually removed on a trade; 0 for other events |
| `side` | order side. On a trade, the side of the resting order |
| `action` | `Add`, `Cancel`, `Modify`, or `Trade` |

`Clear` never arrives as an `Event`; it becomes `OnControl(Reset)`.

### BookView

| Method | Returns |
|---|---|
| `Best(side)` | the best level as `std::optional<Level>`; `nullopt` on an empty side |
| `GetLevel(side, k)` | the k-th level, k from 0; costs O(k) |
| `Ts()` | the time of the last event the book applied |

`Level` has `price`, `qty`, and `count`.

### Params

| Method | Returns |
|---|---|
| `Symbol()` `TickSize()` `LotSize()` | the current instrument |
| `PriceScale()` `QtyScale()` | fixed-point scale factors; price divided by `PriceScale()` is the floating-point price |
| `Get(key)` | `std::optional<std::string_view>` |
| `GetInt(key, fallback)` `GetDouble` `GetBool` | parsed value with a default |
| `Require(key)` `RequireInt` `RequireDouble` | parsed value; throws when missing or malformed |

Parameters come from the plugin's `.params` file and the command line. The engine creates one feature instance per instrument, so `Symbol()` and the other instrument fields differ per instance.

### Testing a feature

`nq/feature_testing.hpp` needs no engine:

```cpp
#include <nq/feature_testing.hpp>

nq::ParamsBuilder pb;
pb.PriceScale(100).QtyScale(10).Set("min_qty", "5");

nq::testing::Sandbox<Imbalance> sb(pb.Build());
sb.Book().SetBids({{10050, 30, 1}}).SetAsks({{10070, 10, 1}});
sb.Feed(nq::testing::Add(1, 1, nq::Side::Bid, 10050, 7));
auto v = sb.Read();   // v[0] == 0.75, v[1] == 1
```

`MockBook::SetBids` and `SetAsks` take levels best first. `Feed` hands the event to the feature and leaves the mock book unchanged.

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
| `kName` | adapter name |
| `kProvides` | optional fidelity of the stream; defaults to `MBO` |
| constructor `(const nq::Params&)` | opens the file or connection. A thrown exception marks the adapter failed |
| `Next(nq::Mbo&)` | produces one event; see the table below |
| `FindInstrument(id) const noexcept` | returns instrument metadata, or `nullptr` for an unknown id |

Return values of `Next`:

| Value | Meaning |
|---|---|
| `Next::Event` | `out` holds one event |
| `Next::Again` | nothing yet; ask again later. Live sources return this instead of blocking |
| `Next::End` | the stream ended; later calls also return `End` |
| `Next::Error` | the stream failed and ended |

An adapter is synchronous. The engine runs it on a dedicated thread and calls `Next` and `FindInstrument` on that thread only, never concurrently. `Next` must not block forever: return `Again` when no data is available, so the engine can act on a stop request.

### The mbo stream contract

The engine trusts the adapter on these points. `nqbook check-feed` verifies the ones it can:

- `ts` never decreases.
- `instrument` is a `uint32_t` id the adapter assigns. `FindInstrument(id)` must answer before the first event with that id.
- `id` identifies one order within the stream.
- `Add`: `qty` is the new order's quantity.
- `Cancel`: `qty` is the quantity removed; 0, or more than the remainder, removes the whole order.
- `Modify`: `price` and `qty` are the new values. A price change or a quantity increase loses queue priority.
- `Trade`: `id` is the resting order, `side` is the resting order's side, `qty` is the traded quantity. With `id` 0 the engine finds the level by `price` and reduces orders from the head of the queue.
- `Clear`: empties that instrument's book. Send it after a sequence gap or a resubscribe.

Prices and quantities are fixed-point integers: price times `NQ_PRICE_SCALE` (1e10), quantity times `NQ_QTY_SCALE` (1e8).

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

`nq/mbo_file.hpp` defines the replay file format with a reader and a writer. `Writer` takes the instrument table and then writes one `nq_mbo` per call. `Reader` returns the instrument table and yields one event per `Next`. The file describes itself; no companion metadata file is needed.

## Headers

| Header | Contents |
|---|---|
| `nq/abi.h` | shared C types: `nq_mbo`, `nq_params`, side and action enums, scale factors |
| `nq/feature_abi.h` | the feature C ABI |
| `nq/adapter_abi.h` | the adapter C ABI |
| `nq/feature.hpp` | every C++ type a feature needs, plus `NQ_PLUGIN` |
| `nq/adapter.hpp` | every C++ type an adapter needs, plus `NQ_ADAPTER` |
| `nq/feature_testing.hpp` `nq/adapter_testing.hpp` | test helpers that need no engine |
| `nq/mbo_file.hpp` | the replay file format |
| `nq/feature_host.hpp` `nq/adapter_host.hpp` | the engine-side loaders; plugins never include them |

A change to `feature_abi.h` invalidates feature plugins only, a change to `adapter_abi.h` invalidates adapters only, and a change to `abi.h` invalidates both. The engine rejects an invalidated plugin at load time with exit code 3; rebuilding the plugin fixes it.

## Testing the SDK itself

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
