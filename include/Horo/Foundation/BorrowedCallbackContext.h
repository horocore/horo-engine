#pragma once

/** @file BorrowedCallbackContext.h @brief Type-checked, non-owning synchronous callback context. */

#include <cstddef>
#include <type_traits>

namespace Horo {
    /**
     * @brief Small borrowed object reference with exact type checking and no allocation or virtual dispatch.
     * @note The caller retains the object and any native code lease through callback retirement. This value grants no
     * lifetime ownership and is neither serialized nor an identity across process or independently rebuilt ABI boundaries.
     */
    class BorrowedCallbackContext final {
    public:
        /** @brief Creates an empty context. */
        constexpr BorrowedCallbackContext() noexcept = default;

        /** @brief Creates an empty context from a null callback argument. @param null Null pointer sentinel. */
        explicit constexpr BorrowedCallbackContext(std::nullptr_t null) noexcept : BorrowedCallbackContext() {
            (void)null;
        }

        /** @brief Borrows one mutable exact-type object. @param object Object retained by the callback owner. */
        template <class T>
            requires(std::is_object_v<T> && !std::is_const_v<T>)
        explicit constexpr BorrowedCallbackContext(T *object) noexcept : object_(object), type_(&TypeMarker<T>) {}

        /** @brief Checks whether an object was supplied. @return True for a non-null borrowed object. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return object_ != nullptr;
        }

        /** @brief Resolves only the exact registered object type. @return Borrowed typed pointer or nullptr on mismatch. */
        template <class T> [[nodiscard]] T *Get() const noexcept {
            return type_ == &TypeMarker<T> ? static_cast<T *>(object_) : nullptr;
        }

    private:
        template <class T> static inline constexpr std::byte TypeMarker{};
        void *object_{};
        const std::byte *type_{};
    };
}  // namespace Horo
