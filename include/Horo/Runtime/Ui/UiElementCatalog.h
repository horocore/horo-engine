#pragma once

/** @file UiElementCatalog.h
 * @brief Canonical core element taxonomy, authored property semantics and surface projection.
 */

#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiTheme.h"

#include <span>
#include <string_view>

namespace Horo::Runtime::Ui {
    /** @brief Stable ADR-176 taxonomy identities; values are never inferred from catalog position. */
    enum class UiElementKind : std::uint16_t {
        Canvas = 1,
        Screen = 2,
        Panel = 3,
        Frame = 4,
        LayoutGroup = 5,
        Text = 6,
        Image = 7,
        Button = 8,
        ProgressBar = 9,
        Slider = 10,
        Toggle = 11,
        TextInput = 12,
        ScrollView = 13,
        CollectionView = 14,
    };

    /** @brief Core type metadata; it neither creates owners nor chooses layouts, skins, actions or backends. */
    struct UiElementTypeDescriptor final {
        UiElementKind kind;
        std::string_view documentType;
        std::string_view localizationKey;
        bool admitsChildren{};
        bool surface{}; /**< True only for the structural types consumed by UiThemeRuntime's solid-surface path. */
    };

    /** @brief Read-only single authority for the ADR-176 core taxonomy and its typed presentation contract. */
    class UiElementCatalog final {
    public:
        /** @brief Returns all core types in stable palette order. @return Process-lifetime immutable metadata. */
        [[nodiscard]] static std::span<const UiElementTypeDescriptor> All() noexcept;
        /** @brief Resolves one known kind. @param kind Stable numeric kind. @return Descriptor or null for unknown values. */
        [[nodiscard]] static const UiElementTypeDescriptor *Find(UiElementKind kind) noexcept;
        /** @brief Resolves one exact durable type token. @param type Serialized type. @return Descriptor or null. */
        [[nodiscard]] static const UiElementTypeDescriptor *Find(std::string_view type) noexcept;
    };

    /** @brief Creates inert authored data from a known core descriptor.
     * @param kind Exact core kind. @param id New stable element identity. @param parent Stable parent; only Canvas has no parent.
     * @return Complete primitive record or typed kind/identity failure; no ambient state is inspected or mutated.
     */
    [[nodiscard]] Result<UiDocumentElement> CreateUiElement(UiElementKind kind, UiElementId id, UiElementId parent);

    /** @brief Validates the reserved core namespace without interpreting legacy/package descriptors.
     * @param element Authored record. @return Success, or typed unknown kind/property/value failure.
     * @details Other namespaces retain the existing document behavior; this does not grant them execution or palette admission.
     */
    [[nodiscard]] Result<void> ValidateCoreUiElement(const UiDocumentElement &element);

    /** @brief Projects typed authored values directly to the actual layout consumer.
     * @param element Authored element. @param handle Exact owner-issued runtime handle.
     * @return Valid layout input or typed descriptor failure; no document borrows survive the result.
     */
    [[nodiscard]] Result<UiLayoutElementDescriptor> ProjectUiElementLayout(const UiDocumentElement &element, UiElementHandle handle);
    /** @brief Projects authored style inputs for one synchronous resolution call.
     * @param element Source whose immutable lifetime encloses resolution. @param handle Exact runtime handle.
     * @return Style input borrowing source presentation spans and carrying authored initial disabled/checked state.
     * Caller retains the source until resolution returns; active control owners replace initial state with current live state.
     */
    [[nodiscard]] Result<UiStyleElementInput> ProjectUiElementStyle(const UiDocumentElement &element, UiElementHandle handle);

    /** @brief Projects supported structural primitives into the actual theme/layout/render consumer input.
     * @param element Validated authored surface. @param handle Exact retained-tree handle supplied by its runtime owner.
     * @return Owned UiThemeSurfaceInput or typed unsupported/invalid failure. Text, imagery and controls require their own owners.
     * @details Bounded by MaximumUiDocumentProperties; no I/O, service lookup or backend selection occurs. Returned style spans borrow
     * element.presentation until it changes or is destroyed.
     */
    [[nodiscard]] Result<UiThemeSurfaceInput> ProjectUiElementSurface(const UiDocumentElement &element, UiElementHandle handle);
    /** @brief Applies authored initial state to an actual host-owned control descriptor.
     * @param document Pinned source document. @param element Stable authored control identity.
     * @param tree Active retained tree from the exact source revision. @param descriptor Real owner/action/handle descriptor.
     * @details Bounded creation-time projection; never applied to an already active mutable control.
     * @return Validated control descriptor or a typed kind/value/ownership error; live control state is never patched.
     */
    [[nodiscard]] Result<UiControlDescriptor> ProjectUiElementControl(const UiDocument &document, UiElementId element,
                                                                      const UiElementTree &tree, UiControlDescriptor descriptor);
    /** @brief Applies authored accessibility metadata to a resolved owner-produced semantic node.
     * @param element Exact authored element. @param node Node with resolved localized labels and real live values/actions/bounds.
     * @return Canonically validated node borrowing its caller's text/actions, or a typed failure; no live authority changes.
     * @pre Caller resolved element accessibility message references against its pinned Localization snapshot.
     */
    [[nodiscard]] Result<UiAccessibilityNodeInput> ProjectUiElementAccessibility(const UiDocumentElement &element,
                                                                                 UiAccessibilityNodeInput node);
    /** @brief Queries the canonical localized content reference for a Text element.
     * @param element Immutable authored Text element retained through resolution.
     * @return Borrowed immutable reference or null; Localization remains responsible for catalog resolution.
     */
    [[nodiscard]] const UiLocalizedText *UiElementLocalizedText(const UiDocumentElement &element) noexcept;
    /** @brief Returns the canonical initial semantic role for a core element kind.
     * @param kind Catalog kind. @return Appropriate role; unknown kinds return Group and are rejected by catalog validation.
     */
    [[nodiscard]] UiAccessibilityRole DefaultUiElementAccessibilityRole(UiElementKind kind) noexcept;
}  // namespace Horo::Runtime::Ui
