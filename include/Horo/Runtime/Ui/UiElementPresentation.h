#pragma once

/** @file UiElementPresentation.h
 * @brief Owned authored presentation values using the canonical Runtime UI layout and style contracts.
 */

#include "Horo/Runtime/Ui/UiAccessibility.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiLocalization.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <string>
#include <string_view>

namespace Horo::Runtime::Ui {
    /** @brief Hard per-element encoded presentation byte ceiling, applied before decoding. */
    inline constexpr std::size_t MaximumUiElementPresentationBytes = 256U * 1024U;

    /** @brief Initial authored control value; live focus/press/capture and mutable control state are excluded. */
    using UiElementInitialValue = std::variant<std::monostate, bool, UiControlScalarValues, std::string>;

    /** @brief Inert initial state, applied only when the owning runtime creates a new control incarnation. */
    struct UiElementAuthoredState final {
        bool visible{true};
        bool enabled{true};
        bool focusable{true};
        UiElementInitialValue initial;
        /** @brief Checks bounded UTF-8 text and canonical scalar initialization semantics. @return Whether valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Projects initial disabled/checked styling without persisting live focus or pointer state.
         * @return Initial visual state; an active control owner supplies subsequent live state.
         */
        [[nodiscard]] UiVisualStateMask InitialVisualState() const noexcept;
        [[nodiscard]] auto operator<=>(const UiElementAuthoredState &) const noexcept = default;
    };

    /** @brief Owned accessibility authoring; live value, focus, bounds and actions stay with their runtime owners. */
    struct UiElementAccessibility final {
        UiAccessibilityRole role{UiAccessibilityRole::Group};
        std::optional<UiLocalizedText> name;
        std::optional<UiLocalizedText> description;
        bool hidden{};
        bool readOnly{};
        bool required{};
        /** @brief Validates canonical role policy and immutable message references. @return Whether valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiElementAccessibility &) const noexcept = default;
    };

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
        UiElementAuthoredState state;
        std::optional<UiLocalizedText> localizedText;
        std::optional<UiElementAccessibility> accessibility;

        /** @brief Reports whether encoding requires source schema 1.3 and presentation format 2. @return Whether extended fields exist. */
        [[nodiscard]] bool UsesAuthoringExtensions() const noexcept;

        /** @brief Validates local values, bounded lists, references and duplicate assignments.
         * @return Whether the detached authored record can enter document storage. Does not resolve host-owned style resources.
         */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiElementPresentation &) const noexcept = default;
    };

    /** @brief Validate the actual registered asset/class/inline cascade, including sealed assignments, before authoring publication.
     * @param registry Real active style registry. @param presentation Locally validated authored values.
     * @return Original canonical style-resolution failure or success; creates no runtime tree, handle or registry.
     */
    [[nodiscard]] Result<void> ValidateUiElementPresentationStyles(const RuntimeStyleRegistry &registry,
                                                                   const UiElementPresentation &presentation);

    /** @brief Encodes a complete bounded presentation using the versioned Runtime UI source codec.
     * @param presentation Locally validated owned values. @return Canonical UTF-8 JSON or typed invalid/capacity failure.
     */
    [[nodiscard]] Result<std::string> SerializeUiElementPresentation(const UiElementPresentation &presentation);
    /** @brief Decodes one bounded presentation through the same source/cooked codec.
     * @param source Borrowed canonical UTF-8 bytes. @return Owned values or typed malformed/version/bounds failure.
     */
    [[nodiscard]] Result<UiElementPresentation> DeserializeUiElementPresentation(std::string_view source);
}  // namespace Horo::Runtime::Ui
