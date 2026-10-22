#pragma once
/** @file MaterialBindingIdentity.h
 * @brief Exact material-table generation identity shared by extraction and submission.
 */
#include "Horo/Runtime/Render/RenderResource.h"

namespace Horo::Render {
    /** @brief Exact renderer-owned material table generation. */
    struct MaterialBindingGenerationId {
        RenderResourceOwnerId owner;
        std::uint64_t value{0};

        /** @brief Checks representation only. @return True for a non-zero owner and generation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const MaterialBindingGenerationId &) const noexcept = default;
    };
}  // namespace Horo::Render
