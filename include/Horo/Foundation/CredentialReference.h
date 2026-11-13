#pragma once

/**
 * @file CredentialReference.h
 * @brief Bounded opaque identity shared by configuration and credential providers.
 */

#include "Horo/Foundation/Result.h"

#include <compare>
#include <string>
#include <string_view>

namespace Horo::Security {
    class CredentialVault;
}

namespace Horo {
    /** @brief Non-secret provider identity containing no credential material. */
    class CredentialReference final {
    public:
        /** @brief Creates an unset reference for an optional schema default. */
        CredentialReference() = default;

        /**
         * @brief Validates a serialized provider identity without consulting a provider.
         * @param value Exactly cred:v1: followed by 48 lowercase hexadecimal digits.
         * @return Opaque reference or a value-invalid failure containing no input text.
         */
        [[nodiscard]] static Result<CredentialReference> Parse(std::string_view value);

        /** @brief Returns non-secret text for persistence and provider routing. @return Borrowed opaque identity. */
        [[nodiscard]] std::string_view Value() const noexcept;

        auto operator<=>(const CredentialReference &) const = default;

    private:
        explicit CredentialReference(std::string value);
        std::string value_;
        friend class Security::CredentialVault;
    };
}  // namespace Horo
