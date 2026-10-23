#pragma once
/** @file NetworkProviderBoundary.h @brief Private synchronous admission-provider exception containment. */
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace Horo::Network::Detail {
    /**
     * @brief Invoke foreign admission code without letting standard or non-standard exceptions escape.
     * @param callback Synchronous borrowed callback; it and its result may allocate, but the boundary adds no allocation.
     * @return Owned result, or absence for a thrown callback/result construction. Callers create typed errors outside this noexcept
     * boundary.
     */
    template <class Callback>
    [[nodiscard]] auto InvokeAdmissionProvider(Callback &&callback) noexcept -> std::optional<std::invoke_result_t<Callback>> {
        try {
            return std::invoke(std::forward<Callback>(callback));
        } catch (...) {
            return std::nullopt;
        }
    }
}  // namespace Horo::Network::Detail
