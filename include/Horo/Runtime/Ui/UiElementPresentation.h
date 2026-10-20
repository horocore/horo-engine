#pragma once

/** @file UiElementPresentation.h
 * @brief Owned authored presentation values using the canonical Runtime UI layout and style contracts.
 */

#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <string>
#include <string_view>

namespace Horo::Runtime::Ui {
    /** @brief Hard per-element encoded presentation byte ceiling, applied before decoding. */
    inline constexpr std::size_t MaximumUiElementPresentationBytes = 256U * 1024U;

    /** @brief Document-owned authored values, distinct from runtime handles and computed style/layout snapshots.
     * @details No alternate property keys or editor-owned style IDs exist. The host supplies the RuntimeStyleRegistry and
     * UiThemeSurfaceProperties bindings; registry-dependent category, token, class and policy validation occurs during resolution.
     */
    struct UiElementPresentation final {
        UiLayoutStyle layout;
        UiLayoutIntrinsicSource intrinsic;
        RuntimeStyleAssetId styleAsset;
        UiStyleClassReference typeClass;
        std::vector<UiStyleClassReference> classes;
        std::vector<UiStyleAssignment> inlineStyle;

        /** @brief Validates local values, bounded lists, references and duplicate assignments.
         * @return Whether the detached authored record can enter document storage. Does not resolve host-owned style resources.
         */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiElementPresentation &) const noexcept = default;
    };

    /** @brief Encodes a complete bounded presentation using the versioned Runtime UI source codec.
     * @param presentation Locally validated owned values. @return Canonical UTF-8 JSON or typed invalid/capacity failure.
     */
    [[nodiscard]] Result<std::string> SerializeUiElementPresentation(const UiElementPresentation &presentation);
    /** @brief Decodes one bounded presentation through the same source/cooked codec.
     * @param source Borrowed canonical UTF-8 bytes. @return Owned values or typed malformed/version/bounds failure.
     */
    [[nodiscard]] Result<UiElementPresentation> DeserializeUiElementPresentation(std::string_view source);
}  // namespace Horo::Runtime::Ui
