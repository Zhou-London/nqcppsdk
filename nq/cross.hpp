#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include <nq/feature.hpp>

namespace nq {

class CrossView {
public:
    explicit CrossView(const nq_cross_panel* p) noexcept : p_(p) {}

    std::int64_t Ts() const noexcept { return p_->ts; }
    std::uint32_t RowCount() const noexcept { return p_->nrow; }

    std::span<const double> In(std::size_t column) const noexcept {
        return std::span<const double>(p_->in + column * p_->stride, p_->nrow);
    }

    std::span<double> Out(std::size_t column) const noexcept {
        return std::span<double>(p_->out + column * p_->stride, p_->nrow);
    }

    bool Valid(std::uint32_t row) const noexcept { return (p_->valid[row >> 6] >> (row & 63)) & 1; }

    std::span<const std::uint32_t> Changed() const noexcept {
        return std::span<const std::uint32_t>(p_->changed, p_->nchanged);
    }

    const nq_cross_panel* Raw() const noexcept { return p_; }

private:
    const nq_cross_panel* p_;
};

template <std::size_t N>
constexpr std::array<const char*, N> MakeInputs(const char* const (&names)[N]) {
    std::array<const char*, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = names[i];
    return out;
}

template <class C>
concept CrossType =
    std::is_constructible_v<C, const Params&> &&
    requires(C& c, const CrossView& view) {
        { C::kInputs.size() } -> std::convertible_to<std::size_t>;
        { C::kInputs.data() } -> std::convertible_to<const char* const*>;
        { C::kColumns.size() } -> std::convertible_to<std::size_t>;
        { C::kColumns.data() } -> std::convertible_to<const Column*>;
        { c.Apply(view) } noexcept;
    };

namespace detail {

template <CrossType C>
const nq_cross_schema* CrossSchema() noexcept {
    static const nq_cross_schema schema{static_cast<std::uint32_t>(C::kInputs.size()), C::kInputs.data(),
                                        static_cast<std::uint32_t>(C::kColumns.size()), C::kColumns.data()};
    return &schema;
}

}  // namespace detail

}  // namespace nq

#define NQ_CROSS(C)                                                                          \
    static_assert(::nq::CrossType<C>, "NQ_CROSS needs kInputs, kColumns and Apply(CrossView)"); \
    extern "C" {                                                                             \
    NQ_EXPORT const nq_cross_schema* nq_cross_describe(void) {                               \
        return ::nq::detail::CrossSchema<C>();                                               \
    }                                                                                        \
    NQ_EXPORT void* nq_cross_init(const nq_params* p) {                                      \
        try {                                                                                \
            return new C(::nq::Params(p));                                                   \
        } catch (...) {                                                                      \
            return nullptr;                                                                  \
        }                                                                                    \
    }                                                                                        \
    NQ_EXPORT void nq_cross_destroy(void* s) { delete static_cast<C*>(s); }                  \
    NQ_EXPORT void nq_cross_apply(void* s, const nq_cross_panel* p) {                        \
        static_cast<C*>(s)->Apply(::nq::CrossView(p));                                       \
    }                                                                                        \
    NQ_EXPORT void nq_cross_bind(void* s, const double* row, size_t stride) {                \
        static_cast<nq_plugin_bundle_t*>(s)->BindCross(::nq::CrossRow{row, stride});         \
    }                                                                                        \
    }
