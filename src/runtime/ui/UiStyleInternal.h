#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiStyle.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    struct UiComputedStyleSnapshot::Storage final {
        struct StyleRange final {
            UiComputedStyleId id;
            std::uint32_t firstProperty{};
            std::uint32_t propertyCount{};
        };

        mutable std::atomic<std::uint64_t> leases{};
        UiComputedStyleSnapshotDescriptor descriptor;
        std::vector<UiComputedStyleRecord> records;
        std::vector<UiComputedStyleProperty> properties;
        std::vector<std::uint32_t> lookup;
        std::vector<StyleRange> styles;

        Storage(std::uint32_t elementCapacity, std::uint32_t propertyCapacity);
    };
}  // namespace Horo::Runtime::Ui

namespace Horo::Runtime::Ui::StyleInternal {
    template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
        return Result<T>::Failure(MakeError(descriptor));
    }

    inline constexpr std::uint16_t KnownVisualStateBits =
        static_cast<std::uint16_t>(UiVisualState::Checked) | static_cast<std::uint16_t>(UiVisualState::Selected) |
        static_cast<std::uint16_t>(UiVisualState::Focused) | static_cast<std::uint16_t>(UiVisualState::Hovered) |
        static_cast<std::uint16_t>(UiVisualState::Pressed) | static_cast<std::uint16_t>(UiVisualState::Dragging) |
        static_cast<std::uint16_t>(UiVisualState::Disabled) | static_cast<std::uint16_t>(UiVisualState::Invalid) |
        static_cast<std::uint16_t>(UiVisualState::Busy);

    [[nodiscard]] bool IsKnownCategory(UiStyleValueCategory category) noexcept;
    [[nodiscard]] bool IsKnownLayer(UiStateLayer layer) noexcept;
    [[nodiscard]] bool SameOwner(RuntimeUiInstanceId instance, UiCanvasInstanceId canvas) noexcept;
    [[nodiscard]] bool IsValueValid(const UiStyleValue &value) noexcept;
    [[nodiscard]] bool IsValueCompatible(const UiStylePropertyDescriptor &descriptor, const UiStyleValue &value) noexcept;

    [[nodiscard]] std::uint64_t HashElementContent(const UiStyleElementInput &input) noexcept;
    [[nodiscard]] std::uint64_t HashElementState(UiVisualStateMask state) noexcept;

    [[nodiscard]] const UiStylePropertyDescriptor *FindProperty(std::span<const UiStylePropertyDescriptor> properties,
                                                                UiStylePropertyId id) noexcept;
    [[nodiscard]] const UiStyleAssetDefinition *FindAsset(const UiStyleRegistryDefinition &definition, RuntimeStyleAssetId id) noexcept;
    [[nodiscard]] const UiStyleAssetDefinition *FindAsset(std::span<const UiStyleAssetDefinition> assets, RuntimeStyleAssetId id) noexcept;
    [[nodiscard]] const UiStyleClassDefinition *FindClass(const UiStyleAssetDefinition &asset, UiStyleClassId id) noexcept;
    [[nodiscard]] const UiStyleTokenDefinition *FindToken(const UiStyleAssetDefinition &asset, UiStyleTokenId id) noexcept;
    [[nodiscard]] const UiStyleClassDefinition *FindClass(std::span<const UiStyleAssetDefinition> assets,
                                                          UiStyleClassReference reference) noexcept;
    [[nodiscard]] const UiStyleTokenDefinition *FindToken(std::span<const UiStyleAssetDefinition> assets,
                                                          UiStyleTokenReference reference) noexcept;

    [[nodiscard]] Result<void> ValidateAssetAndClassShape(const UiStyleRegistryDefinition &definition);
    [[nodiscard]] Result<UiStyleValue> ResolveTokenDefinition(const UiStyleRegistryDefinition &definition, UiStyleTokenReference reference,
                                                              std::vector<UiStyleTokenReference> &path, std::size_t depth);

    struct WorkingProperty final {
        UiComputedStyleProperty value;
        bool sealed{};
    };

    struct WorkingStyle final {
        std::array<WorkingProperty, MaximumUiStyleProperties> properties{};
        std::uint32_t count{};

        [[nodiscard]] WorkingProperty *Find(UiStylePropertyId id) noexcept;
        [[nodiscard]] const WorkingProperty *Find(UiStylePropertyId id) const noexcept;
    };

    [[nodiscard]] Result<void> ValidateElementInput(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
                                                    std::uint32_t propertyCapacity);
    [[nodiscard]] Result<WorkingStyle> ResolveElement(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
                                                      const WorkingStyle *parent, std::uint32_t propertyCapacity);

}  // namespace Horo::Runtime::Ui::StyleInternal
