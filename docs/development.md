# NQCPPSDK Development

This document is for people who change the SDK itself. Plugin authors read [manual.md](manual.md).

## Layout

```
nq/abi.h                 C: types both plugin kinds share: nq_mbo, nq_ohlcv, nq_params, the side, action, and feed enums, the scale factors
nq/feature_abi.h         C: the feature plugin ABI: nq_event, nq_level, nq_book_view, nq_column, nq_schema, the entry points
nq/adapter_abi.h         C: the adapter plugin ABI: nq_instrument, nq_adapter_schema, the entry points
nq/types.hpp             C++: Side, Action, and Feed enums over the C values, Ohlcv
nq/instrument.hpp        C++: Instrument
nq/params.hpp            C++: Params over nq_params, ParamsBuilder
nq/book_view.hpp         C++: BookView over nq_book_view
nq/feature.hpp           C++: Event, Control, FeatureType and OhlcvFeatureType concepts, Bundle, NQ_PLUGIN, NQ_OHLCV_PLUGIN
nq/adapter.hpp           C++: Mbo, Next, AdapterType and OhlcvAdapterType concepts, NQ_ADAPTER, NQ_OHLCV_ADAPTER
nq/feature_testing.hpp   C++: MockBook, Sandbox
nq/adapter_testing.hpp   C++: AdapterSandbox
nq/plugin_error.hpp      C++: the exception types the host layer throws
nq/feature_host.hpp      C++: Plugin and PluginInstance, the engine side of a feature plugin
nq/adapter_host.hpp      C++: AdapterPlugin and AdapterInstance, the engine side of an adapter
cmake/NqPlugin.cmake     nq_add_plugin()
tests/                   feature_test and adapter_test, each with one test plugin per feed built as a .so
CMakeLists.txt           the nqcppsdk INTERFACE target, the ABI hashes, the tests option
```

Three layers, from the plugin outward:

1. The C ABI. Three headers that compile as C11. This is the only thing the engine and the plugin agree on.
2. The C++ plugin layer. `feature.hpp` and `adapter.hpp` turn a plain class into the C entry points through the plugin macros: `NQ_PLUGIN`, `NQ_OHLCV_PLUGIN`, `NQ_ADAPTER`, and `NQ_OHLCV_ADAPTER`. `Bundle` concatenates the columns of several feature classes and dispatches to each.
3. The C++ host layer. `feature_host.hpp` and `adapter_host.hpp` load a `.so`, resolve the entry points, check the hash, and wrap the instance. Only the engine includes these.

## Feeds

`nq_feed` names the record an adapter produces: `NQ_FEED_MBO` or `NQ_FEED_OHLCV`. Both schemas carry it in `feed`. The feed selects the entry point that carries records:

| Feed | Adapter exports | Feature plugin exports | `nq_read` receives |
|---|---|---|---|
| MBO | `nq_adapter_next` | `nq_on_event` | the book view |
| OHLCV | `nq_adapter_next_ohlcv` | `nq_on_ohlcv` | a null book view |

The other entry points are the same for both feeds. The host reads `feed` after the hash check and resolves the record entry point of that feed only.

`Bundle` derives its feed from its feature classes. All `FeatureType` gives MBO, all `OhlcvFeatureType` gives OHLCV, and a mix fails to compile. `NQ_PLUGIN` and `NQ_ADAPTER` accept MBO classes only. `NQ_OHLCV_PLUGIN` and `NQ_OHLCV_ADAPTER` accept OHLCV classes only.

`provides` and `requirement` describe order book depth. An OHLCV plugin sets them to `NQ_REQUIRES_NONE`.

## The ABI hash

`CMakeLists.txt` hashes the ABI headers at configure time and defines two macros on the `nqcppsdk` target:

| Macro | Covers | Checked by |
|---|---|---|
| `NQ_ABI_HASH` | `abi.h` and `feature_abi.h` | `host::Plugin` against `nq_schema.abi_hash` |
| `NQ_ADAPTER_ABI_HASH` | `abi.h` and `adapter_abi.h` | `host::AdapterPlugin` against `nq_adapter_schema.abi_hash` |

The plugin macros write the macro's value into the schema the plugin exports. The engine, built against its own copy of the headers, compares. A change to `feature_abi.h` therefore invalidates feature plugins only, a change to `adapter_abi.h` adapters only, and a change to `abi.h` both. The ABI headers are listed in `CMAKE_CONFIGURE_DEPENDS`, so editing one reconfigures the build.

Rules for the C headers:

- They must compile as C11 with `-Wall -Wextra -Wpedantic`. `tests/` in NQBook keeps a plain C plugin that proves it.
- A field is never renamed, reordered, or resized; add fields at the end. Any such change still changes the hash, so every plugin rebuilds, but the C++ layer keeps compiling.
- Entry points are `extern "C"` functions named by the `NQ_SYM_*` macros. The host resolves them by those names.
- `NQ_SDK_VERSION` is the human-readable version. The hash is the machine-checked one; both move on a release.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

The tests are built when the SDK is the top-level project (`NQCPPSDK_BUILD_TESTS`, default `ON` there and `OFF` when consumed through `add_subdirectory` or `FetchContent`). `feature_test` runs a feature through `Sandbox` in-process and again through a `.so` loaded with `host::Plugin`. `adapter_test` does the same for an adapter with `AdapterSandbox` and `host::AdapterPlugin`, and checks that each loader refuses the other kind of plugin. Both tests cover both feeds.

`tests/check.hpp` holds the `CHECK` macro every test uses.

## Release

1. Commit the change.
2. Bump `NQ_SDK_VERSION` in `nq/abi.h` and `VERSION` in `CMakeLists.txt`. Update the version in the README banner and the `GIT_TAG` in the README and the manual.
3. `git tag -a vX.Y.Z -m "nqcppsdk vX.Y.Z"`, then `git push origin main` and `git push origin vX.Y.Z`.
4. In NQBook, set `GIT_TAG vX.Y.Z` in its `CMakeLists.txt`, delete `build/` and `shared/nqcppsdk`, reconfigure, and run its tests.

A change to any ABI header is a release: plugins built before it stop loading.

## Conventions

- Header-only. No `.cpp` files, no dependencies beyond the standard library.
- No comments in code. This file and the manual carry the explanations.
- C headers: `nq_` prefixed snake_case types and `NQ_` prefixed macros and enum values. C++ headers: namespace `nq`, PascalCase types and methods.
- Every C++ wrapper is a thin view over one C struct and never owns engine state.
- Plugin-facing concepts (`FeatureType`, `OhlcvFeatureType`, `AdapterType`, `OhlcvAdapterType`) are checked by the plugin macros with a `static_assert`, so a wrong member signature fails in the plugin's own build.
