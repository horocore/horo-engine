#pragma once
/** @file GnsNativeCall.h @brief Private containment of native C++ calls; no error allocation at the boundary. */
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace Horo::Network::GnsDetail {
    /** @brief Invoke native code and move its result inside one noexcept barrier. @return Empty only on an exception. */
    template <typename Callback>
    [[nodiscard]] auto NativeCall(Callback &&callback) noexcept -> std::optional<std::invoke_result_t<Callback>> {
        try {
            return std::invoke(std::forward<Callback>(callback));
        } catch (...) {
            return std::nullopt;
        }
    }

    /** @brief Invoke a void native operation without projecting/allocating an Error. @return False only on an exception. */
    template <typename Callback> [[nodiscard]] bool NativeVoid(Callback &&callback) noexcept {
        try {
            std::invoke(std::forward<Callback>(callback));
            return true;
        } catch (...) {
            return false;
        }
    }
}  // namespace Horo::Network::GnsDetail
