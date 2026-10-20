<p align="center"><img src="https://capsule-render.vercel.app/api?type=blur&height=240&color=gradient&text=NQCPPSDK&fontSize=64&fontAlign=50&fontAlignY=45&desc=v0.1.2&descSize=22&descAlign=50&descAlignY=65" /></p>

<p align="center">
  <img alt="Version 0.1.2" src="https://img.shields.io/badge/version-0.1.2-blue" />
  <img alt="C++23" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" />
  <img alt="CMake 3.28+" src="https://img.shields.io/badge/CMake-3.28%2B-064F8C?logo=cmake&logoColor=white" />
  <img alt="Header-only" src="https://img.shields.io/badge/header--only-INTERFACE-4c1" />
  <img alt="C ABI" src="https://img.shields.io/badge/plugin-C%20ABI-orange" />
</p>

# NQCPPSDK

NQCPPSDK is the SDK for writing [NQBook](https://github.com/Zhou-London/nqbook) plugins: feature plugins that compute numbers from an order book, and adapter plugins that produce the MBO event stream.

- **Header-only, no dependencies.** A plugin includes headers under `nq/` and links nothing.
- **A C ABI between engine and plugin.** The C++ layer is a thin wrapper, so a plugin can also be written in C or any language that can export C symbols.
- **Hash-checked.** The content hash of the ABI headers is compiled into every plugin. NQBook refuses a plugin built against a different SDK.
- **Testable without the engine.** `feature_testing.hpp` and `adapter_testing.hpp` run a plugin in-process with a mock book.

## Thirty seconds

```cmake
include(FetchContent)
FetchContent_Declare(nqcppsdk
    GIT_REPOSITORY https://github.com/Zhou-London/nqcppsdk.git
    GIT_TAG v0.1.2
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(nqcppsdk)

nq_add_plugin(my_feature my_feature.cpp)
```

```cpp
#include <nq/feature.hpp>

struct Spread : nq::FeatureBase {
    static constexpr auto kColumns = nq::MakeColumns({{"spread", "price", "best ask minus best bid"}});
    explicit Spread(const nq::Params&) {}
    void Read(std::span<double> out, const nq::BookView& book) const noexcept {
        const auto b = book.Best(nq::Side::Bid), a = book.Best(nq::Side::Ask);
        out[0] = (b && a) ? double(a->price - b->price) : 0.0;
    }
};

NQ_PLUGIN(Spread)
```

## Documents

| Document | For | Contents |
|---|---|---|
| [docs/manual.md](docs/manual.md) | plugin authors | project setup, writing and testing a feature, writing and testing an adapter, the event stream contract, the API reference |
| [docs/development.md](docs/development.md) | people changing the SDK | header layout, the ABI and its hash, tests, release loop |
| [NQBook manual](https://github.com/Zhou-London/nqbook/blob/main/docs/manual.md) | users of the engine | how plugins are loaded, configured, and run |
