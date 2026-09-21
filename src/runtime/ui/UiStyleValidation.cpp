#include "UiStyleInternal.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui::StyleInternal {
    bool IsKnownCategory(const UiStyleValueCategory category) noexcept {
        return static_cast<std::uint8_t>(category) <= static_cast<std::uint8_t>(UiStyleValueCategory::Motion);
    }

    bool IsKnownLayer(const UiStateLayer layer) noexcept {
        return static_cast<std::uint8_t>(layer) <= static_cast<std::uint8_t>(UiStateLayer::Availability);
    }

    bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
        return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
    }

    bool IsValueValid(const UiStyleValue &value) noexcept {
        return std::visit([](const auto &typed) noexcept {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, UiStyleColor>)
                return typed.IsValid();
            else if constexpr (std::is_same_v<Value, UiStyleTypography>)
                return typed.IsValid();
            else if constexpr (std::is_same_v<Value, UiStyleImage>)
                return typed.IsValid();
            else if constexpr (std::is_same_v<Value, UiStyleShape>)
                return typed.IsValid();
            else if constexpr (std::is_same_v<Value, UiStyleScalar>)
                return typed.IsValid();
            else if constexpr (std::is_same_v<Value, UiStyleMotionReference>)
                return typed.durationMicroseconds <= 60'000'000U && typed.easing <= 4095U;
            else
                return true;
        }, value);
    }

    bool IsValueCompatible(const UiStylePropertyDescriptor &descriptor, const UiStyleValue &value) noexcept {
        if (!IsValueValid(value) || UiStyleValueCategoryOf(value) != descriptor.category)
            return false;
        if (descriptor.category == UiStyleValueCategory::Dimension) {
            const auto *dimension = std::get_if<UiStyleDimension>(&value);
            return dimension != nullptr && (descriptor.allowsSignedDimension || dimension->value >= 0);
        }
        if (descriptor.category == UiStyleValueCategory::Scalar) {
            const auto *scalar = std::get_if<UiStyleScalar>(&value);
            return scalar != nullptr && scalar->value >= descriptor.minimumScalar && scalar->value <= descriptor.maximumScalar;
        }
        return true;
    }

    const UiStylePropertyDescriptor *FindProperty(const std::span<const UiStylePropertyDescriptor> properties,
                                                  const UiStylePropertyId id) noexcept {
        const auto found = std::lower_bound(properties.begin(), properties.end(), id,
                                            [](const UiStylePropertyDescriptor &property, const UiStylePropertyId value) {
            return property.id < value;
        });
        return found != properties.end() && found->id == id ? &*found : nullptr;
    }

    const UiStyleAssetDefinition *FindAsset(const UiStyleRegistryDefinition &definition, const RuntimeStyleAssetId id) noexcept {
        return FindAsset(std::span<const UiStyleAssetDefinition>{definition.assets}, id);
    }

    const UiStyleAssetDefinition *FindAsset(const std::span<const UiStyleAssetDefinition> assets, const RuntimeStyleAssetId id) noexcept {
        const auto found =
            std::lower_bound(assets.begin(), assets.end(), id, [](const UiStyleAssetDefinition &asset, const RuntimeStyleAssetId value) {
            return asset.id < value;
        });
        return found != assets.end() && found->id == id ? &*found : nullptr;
    }

    const UiStyleClassDefinition *FindClass(const UiStyleAssetDefinition &asset, const UiStyleClassId id) noexcept {
        const auto found = std::lower_bound(asset.classes.begin(), asset.classes.end(), id,
                                            [](const UiStyleClassDefinition &styleClass, const UiStyleClassId value) {
            return styleClass.id < value;
        });
        return found != asset.classes.end() && found->id == id ? &*found : nullptr;
    }

    const UiStyleTokenDefinition *FindToken(const UiStyleAssetDefinition &asset, const UiStyleTokenId id) noexcept {
        const auto found = std::lower_bound(asset.tokens.begin(), asset.tokens.end(), id,
                                            [](const UiStyleTokenDefinition &token, const UiStyleTokenId value) {
            return token.id < value;
        });
        return found != asset.tokens.end() && found->id == id ? &*found : nullptr;
    }

    const UiStyleClassDefinition *FindClass(const std::span<const UiStyleAssetDefinition> assets,
                                            const UiStyleClassReference reference) noexcept {
        const auto *asset = FindAsset(assets, reference.asset);
        return asset == nullptr ? nullptr : FindClass(*asset, reference.id);
    }

    const UiStyleTokenDefinition *FindToken(const std::span<const UiStyleAssetDefinition> assets,
                                            const UiStyleTokenReference reference) noexcept {
        const auto *asset = FindAsset(assets, reference.asset);
        return asset == nullptr ? nullptr : FindToken(*asset, reference.id);
    }

    namespace {
        struct ShapeCounts final {
            std::size_t tokens{};
            std::size_t classes{};
            std::size_t states{};
        };

        [[nodiscard]] Result<void> ValidateAssignmentShape(const UiStyleAssignment &assignment) {
            if (!assignment.property.IsValid() || !assignment.value.IsValid())
                return Failure(UiErrors::StyleInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStateShape(const UiStyleStateOverride &state) {
            if (!state.required.IsValid() || !state.forbidden.IsValid() || (state.required.bits & state.forbidden.bits) != 0 ||
                !IsKnownLayer(state.layer) || state.assignments.empty() || state.assignments.size() > MaximumUiStyleProperties)
                return Failure(UiErrors::StyleStateInvalid);
            for (const auto &assignment : state.assignments)
                if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateProperties(const UiStyleRegistryDefinition &definition) {
            for (std::size_t index = 0; index < definition.properties.size(); ++index) {
                const auto &property = definition.properties[index];
                if (!property.IsValid())
                    return Failure(UiErrors::StyleInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (definition.properties[previous].id == property.id)
                        return Failure(UiErrors::StyleIdentityConflict);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAssetHeader(const UiStyleAssetDefinition &asset,
                                                       const std::span<const UiStyleAssetDefinition> assets, const std::size_t assetIndex,
                                                       ShapeCounts &counts) {
            if (!asset.id.IsValid() || asset.tokens.size() > MaximumUiStyleTokens || asset.classes.size() > MaximumUiStyleClasses ||
                asset.states.size() > MaximumUiStyleStateBlocks || asset.assignments.size() > MaximumUiStyleProperties)
                return Failure(UiErrors::StyleInvalid);
            for (std::size_t previous = 0; previous < assetIndex; ++previous)
                if (assets[previous].id == asset.id)
                    return Failure(UiErrors::StyleIdentityConflict);
            counts.tokens += asset.tokens.size();
            counts.classes += asset.classes.size();
            counts.states += asset.states.size();
            if (counts.tokens > MaximumUiStyleTokens || counts.classes > MaximumUiStyleClasses || counts.states > MaximumUiStyleStateBlocks)
                return Failure(UiErrors::CapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateTokens(const UiStyleAssetDefinition &asset) {
            for (std::size_t tokenIndex = 0; tokenIndex < asset.tokens.size(); ++tokenIndex) {
                const auto &token = asset.tokens[tokenIndex];
                if (!token.id.IsValid() || !IsKnownCategory(token.category) || !token.value.IsValid())
                    return Failure(UiErrors::StyleInvalid);
                if (token.value.referencesToken && !token.value.token.IsValid())
                    return Failure(UiErrors::StyleReferenceInvalid);
                for (std::size_t previous = 0; previous < tokenIndex; ++previous)
                    if (asset.tokens[previous].id == token.id)
                        return Failure(UiErrors::StyleIdentityConflict);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateClass(const UiStyleClassDefinition &styleClass,
                                                 const std::span<const UiStyleClassDefinition> classes, const std::size_t classIndex,
                                                 ShapeCounts &counts) {
            if (!styleClass.id.IsValid() || styleClass.assignments.size() > MaximumUiStyleProperties ||
                styleClass.states.size() > MaximumUiStyleStateBlocks)
                return Failure(UiErrors::StyleInvalid);
            if (styleClass.base.has_value() && !styleClass.base->IsValid())
                return Failure(UiErrors::StyleReferenceInvalid);
            for (std::size_t previous = 0; previous < classIndex; ++previous)
                if (classes[previous].id == styleClass.id)
                    return Failure(UiErrors::StyleIdentityConflict);
            for (const auto &assignment : styleClass.assignments)
                if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                    return valid;
            for (const auto &state : styleClass.states)
                if (const auto valid = ValidateStateShape(state); valid.HasError())
                    return valid;
            counts.states += styleClass.states.size();
            return counts.states <= MaximumUiStyleStateBlocks ? Result<void>::Success() : Failure(UiErrors::CapacityExceeded);
        }

        [[nodiscard]] Result<void> ValidateAssetContents(const UiStyleAssetDefinition &asset, ShapeCounts &counts) {
            if (const auto valid = ValidateTokens(asset); valid.HasError())
                return valid;
            for (const auto &assignment : asset.assignments)
                if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                    return valid;
            for (const auto &state : asset.states)
                if (const auto valid = ValidateStateShape(state); valid.HasError())
                    return valid;
            for (std::size_t classIndex = 0; classIndex < asset.classes.size(); ++classIndex)
                if (const auto valid = ValidateClass(asset.classes[classIndex], asset.classes, classIndex, counts); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAssetChain(const UiStyleRegistryDefinition &definition, const UiStyleAssetDefinition &asset) {
            if (asset.base.has_value() && FindAsset(definition, *asset.base) == nullptr)
                return Failure(UiErrors::StyleReferenceInvalid);
            std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> chain{};
            std::size_t depth = 0;
            auto current = asset.id;
            while (current.IsValid()) {
                if (depth == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::size_t previous = 0; previous < depth; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                chain[depth++] = current;
                const auto *currentAsset = FindAsset(definition, current);
                if (currentAsset == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                current = currentAsset->base.value_or(RuntimeStyleAssetId{});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateClassChain(const UiStyleRegistryDefinition &definition, const UiStyleAssetDefinition &asset,
                                                      const UiStyleClassDefinition &styleClass) {
            if (!styleClass.base.has_value())
                return Result<void>::Success();
            const auto assets = std::span<const UiStyleAssetDefinition>{definition.assets};
            if (FindClass(assets, *styleClass.base) == nullptr)
                return Failure(UiErrors::StyleReferenceInvalid);
            std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
            std::size_t depth = 0;
            auto current = UiStyleClassReference{asset.id, styleClass.id};
            while (current.IsValid()) {
                if (depth == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::size_t previous = 0; previous < depth; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                chain[depth++] = current;
                const auto *currentDefinition = FindClass(assets, current);
                if (currentDefinition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (current != UiStyleClassReference{asset.id, styleClass.id} && currentDefinition->sealed)
                    return Failure(UiErrors::StyleReferenceInvalid);
                current = currentDefinition->base.value_or(UiStyleClassReference{});
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAssetReferences(const UiStyleRegistryDefinition &definition,
                                                           const UiStyleAssetDefinition &asset) {
            if (const auto valid = ValidateAssetChain(definition, asset); valid.HasError())
                return valid;
            for (const auto &styleClass : asset.classes)
                if (const auto valid = ValidateClassChain(definition, asset, styleClass); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateAssetAndClassShape(const UiStyleRegistryDefinition &definition) {
        if (definition.properties.empty() || definition.properties.size() > MaximumUiStyleProperties || definition.assets.empty() ||
            definition.assets.size() > MaximumUiStyleAssets)
            return Failure(UiErrors::StyleInvalid);
        if (const auto valid = ValidateProperties(definition); valid.HasError())
            return valid;

        ShapeCounts counts;
        const auto assets = std::span<const UiStyleAssetDefinition>{definition.assets};
        for (std::size_t assetIndex = 0; assetIndex < assets.size(); ++assetIndex) {
            if (const auto valid = ValidateAssetHeader(assets[assetIndex], assets, assetIndex, counts); valid.HasError())
                return valid;
            if (const auto valid = ValidateAssetContents(assets[assetIndex], counts); valid.HasError())
                return valid;
        }
        for (const auto &asset : assets)
            if (const auto valid = ValidateAssetReferences(definition, asset); valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    Result<UiStyleValue> ResolveTokenDefinition(const UiStyleRegistryDefinition &definition, const UiStyleTokenReference reference,
                                                std::vector<UiStyleTokenReference> &path, const std::size_t depth) {
        if (!reference.IsValid())
            return Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        if (depth >= MaximumUiStyleInheritanceDepth)
            return Failure<UiStyleValue>(UiErrors::StyleCycle);
        if (std::find(path.begin(), path.end(), reference) != path.end())
            return Failure<UiStyleValue>(UiErrors::StyleCycle);
        const auto *token = FindToken(std::span<const UiStyleAssetDefinition>{definition.assets}, reference);
        if (token == nullptr)
            return Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        path.push_back(reference);
        UiStyleValue value{};
        if (token->value.referencesToken) {
            auto resolved = ResolveTokenDefinition(definition, token->value.token, path, depth + 1);
            if (resolved.HasError())
                return resolved;
            value = std::move(resolved).Value();
        } else {
            value = token->value.literal;
        }
        path.pop_back();
        if (UiStyleValueCategoryOf(value) != token->category || !IsValueValid(value))
            return Failure<UiStyleValue>(UiErrors::StyleTypeMismatch);
        return Result<UiStyleValue>::Success(std::move(value));
    }
}  // namespace Horo::Runtime::Ui::StyleInternal
