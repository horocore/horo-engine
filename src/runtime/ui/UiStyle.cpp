#include "Horo/Runtime/Ui/UiStyle.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        constexpr std::uint16_t KnownVisualStateBits =
            static_cast<std::uint16_t>(UiVisualState::Checked) | static_cast<std::uint16_t>(UiVisualState::Selected) |
            static_cast<std::uint16_t>(UiVisualState::Focused) | static_cast<std::uint16_t>(UiVisualState::Hovered) |
            static_cast<std::uint16_t>(UiVisualState::Pressed) | static_cast<std::uint16_t>(UiVisualState::Dragging) |
            static_cast<std::uint16_t>(UiVisualState::Disabled) | static_cast<std::uint16_t>(UiVisualState::Invalid) |
            static_cast<std::uint16_t>(UiVisualState::Busy);

        [[nodiscard]] bool IsKnownCategory(const UiStyleValueCategory category) noexcept {
            return static_cast<std::uint8_t>(category) <= static_cast<std::uint8_t>(UiStyleValueCategory::Motion);
        }

        [[nodiscard]] bool IsKnownLayer(const UiStateLayer layer) noexcept {
            return static_cast<std::uint8_t>(layer) <= static_cast<std::uint8_t>(UiStateLayer::Availability);
        }

        [[nodiscard]] bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas) noexcept {
            return instance.IsValid() && canvas.IsValid() && instance.ownership == canvas.ownership;
        }

        [[nodiscard]] bool IsValueValid(const UiStyleValue &value) noexcept {
            return std::visit(
                [](const auto &typed) noexcept {
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
                },
                value);
        }

        [[nodiscard]] bool IsValueCompatible(const UiStylePropertyDescriptor &descriptor, const UiStyleValue &value) noexcept {
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

        template <typename Identity> void HashIdentity(std::uint64_t &hash, const Identity &identity) noexcept {
            for (const std::uint8_t byte : identity.Bytes()) {
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
        }

        void HashBytes(std::uint64_t &hash, const std::uint8_t *bytes, const std::size_t count) noexcept {
            for (std::size_t index = 0; index < count; ++index) {
                hash ^= bytes[index];
                hash *= 1099511628211ULL;
            }
        }

        void HashValue(std::uint64_t &hash, const UiStyleValue &value) noexcept {
            hash ^= static_cast<std::uint8_t>(UiStyleValueCategoryOf(value));
            hash *= 1099511628211ULL;
            std::visit(
                [&hash](const auto &typed) noexcept {
                    using Value = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<Value, UiStyleColor>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.red), sizeof(float));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.green), sizeof(float));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.blue), sizeof(float));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.alpha), sizeof(float));
                        hash ^= static_cast<std::uint8_t>(typed.role);
                    } else if constexpr (std::is_same_v<Value, UiStyleDimension>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                    } else if constexpr (std::is_same_v<Value, UiStyleTypography>) {
                        HashIdentity(hash, typed.family);
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.weight), sizeof(typed.weight));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.stretch), sizeof(typed.stretch));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.style), sizeof(typed.style));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.size), sizeof(typed.size));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.lineHeight), sizeof(typed.lineHeight));
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.letterSpacing), sizeof(typed.letterSpacing));
                    } else if constexpr (std::is_same_v<Value, UiStyleImage>) {
                        HashIdentity(hash, typed.asset);
                        hash ^= static_cast<std::uint8_t>(typed.fit);
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(typed.nineSlice.data()),
                                  sizeof(typed.nineSlice));
                        HashValue(hash, UiStyleValue{typed.tint});
                    } else if constexpr (std::is_same_v<Value, UiStyleShape>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed), sizeof(typed));
                    } else if constexpr (std::is_same_v<Value, UiStyleScalar>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                    } else if constexpr (std::is_same_v<Value, UiStyleEnumValue>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed.value), sizeof(typed.value));
                    } else if constexpr (std::is_same_v<Value, UiStyleMotionReference>) {
                        HashBytes(hash, reinterpret_cast<const std::uint8_t *>(&typed), sizeof(typed));
                    }
                },
                value);
        }

        void HashSource(std::uint64_t &hash, const UiStyleValueSource &source) noexcept {
            hash ^= source.referencesToken ? 1U : 0U;
            hash *= 1099511628211ULL;
            if (source.referencesToken) {
                HashIdentity(hash, source.token.asset);
                HashIdentity(hash, source.token.id);
            } else {
                HashValue(hash, source.literal);
            }
        }

        void HashAssignment(std::uint64_t &hash, const UiStyleAssignment &assignment) noexcept {
            HashIdentity(hash, assignment.property);
            hash ^= assignment.sealed ? 1U : 0U;
            hash *= 1099511628211ULL;
            HashSource(hash, assignment.value);
        }

        [[nodiscard]] std::uint64_t HashAssignments(const std::span<const UiStyleAssignment> assignments) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const auto &assignment : assignments)
                HashAssignment(hash, assignment);
            return hash;
        }

        [[nodiscard]] std::uint64_t HashElementContent(const UiStyleElementInput &input) noexcept {
            std::uint64_t hash = 1469598103934665603ULL;
            HashIdentity(hash, input.asset);
            HashIdentity(hash, input.typeClass.asset);
            HashIdentity(hash, input.typeClass.id);
            for (const auto &classReference : input.classes) {
                HashIdentity(hash, classReference.asset);
                HashIdentity(hash, classReference.id);
            }
            hash ^= HashAssignments(input.inlineProperties);
            hash *= 1099511628211ULL;
            hash ^= HashAssignments(input.policyProperties);
            hash *= 1099511628211ULL;
            return hash;
        }

        [[nodiscard]] std::uint64_t HashElementState(const UiVisualStateMask state) noexcept {
            return static_cast<std::uint64_t>(state.bits) * 1099511628211ULL;
        }

        [[nodiscard]] const UiStylePropertyDescriptor *FindProperty(const std::span<const UiStylePropertyDescriptor> properties,
                                                                     const UiStylePropertyId id) noexcept {
            const auto found = std::lower_bound(properties.begin(), properties.end(), id,
                                                [](const UiStylePropertyDescriptor &property, const UiStylePropertyId value) {
                                                    return property.id < value;
                                                });
            return found != properties.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const UiStyleAssetDefinition *FindAsset(const UiStyleRegistryDefinition &definition,
                                                               const RuntimeStyleAssetId id) noexcept {
            const auto found = std::lower_bound(definition.assets.begin(), definition.assets.end(), id,
                                                [](const UiStyleAssetDefinition &asset, const RuntimeStyleAssetId value) {
                                                    return asset.id < value;
                                                });
            return found != definition.assets.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const UiStyleAssetDefinition *FindAsset(const std::span<const UiStyleAssetDefinition> assets,
                                                               const RuntimeStyleAssetId id) noexcept {
            const auto found = std::lower_bound(assets.begin(), assets.end(), id,
                                                [](const UiStyleAssetDefinition &asset, const RuntimeStyleAssetId value) {
                                                    return asset.id < value;
                                                });
            return found != assets.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const UiStyleClassDefinition *FindClass(const UiStyleAssetDefinition &asset, const UiStyleClassId id) noexcept {
            const auto found = std::lower_bound(asset.classes.begin(), asset.classes.end(), id,
                                                [](const UiStyleClassDefinition &styleClass, const UiStyleClassId value) {
                                                    return styleClass.id < value;
                                                });
            return found != asset.classes.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const UiStyleTokenDefinition *FindToken(const UiStyleAssetDefinition &asset, const UiStyleTokenId id) noexcept {
            const auto found = std::lower_bound(asset.tokens.begin(), asset.tokens.end(), id,
                                                [](const UiStyleTokenDefinition &token, const UiStyleTokenId value) {
                                                    return token.id < value;
                                                });
            return found != asset.tokens.end() && found->id == id ? &*found : nullptr;
        }

        [[nodiscard]] const UiStyleClassDefinition *FindClass(const std::span<const UiStyleAssetDefinition> assets,
                                                               const UiStyleClassReference reference) noexcept {
            const auto *asset = FindAsset(assets, reference.asset);
            return asset == nullptr ? nullptr : FindClass(*asset, reference.id);
        }

        [[nodiscard]] const UiStyleTokenDefinition *FindToken(const std::span<const UiStyleAssetDefinition> assets,
                                                               const UiStyleTokenReference reference) noexcept {
            const auto *asset = FindAsset(assets, reference.asset);
            return asset == nullptr ? nullptr : FindToken(*asset, reference.id);
        }

        [[nodiscard]] Result<void> ValidateAssignmentShape(const UiStyleAssignment &assignment) {
            if (!assignment.property.IsValid() || !assignment.value.IsValid())
                return Failure(UiErrors::StyleInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStateShape(const UiStyleStateOverride &state) {
            if (!state.required.IsValid() || !state.forbidden.IsValid() ||
                (state.required.bits & state.forbidden.bits) != 0 || !IsKnownLayer(state.layer) ||
                state.assignments.empty() || state.assignments.size() > MaximumUiStyleProperties)
                return Failure(UiErrors::StyleStateInvalid);
            for (const auto &assignment : state.assignments)
                if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAssetAndClassShape(const UiStyleRegistryDefinition &definition) {
            if (definition.properties.empty() || definition.properties.size() > MaximumUiStyleProperties ||
                definition.assets.empty() || definition.assets.size() > MaximumUiStyleAssets)
                return Failure(UiErrors::StyleInvalid);

            for (std::size_t index = 0; index < definition.properties.size(); ++index) {
                const auto &property = definition.properties[index];
                if (!property.IsValid())
                    return Failure(UiErrors::StyleInvalid);
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (definition.properties[previous].id == property.id)
                        return Failure(UiErrors::StyleIdentityConflict);
            }

            std::size_t tokenCount = 0;
            std::size_t classCount = 0;
            std::size_t stateCount = 0;
            for (std::size_t assetIndex = 0; assetIndex < definition.assets.size(); ++assetIndex) {
                const auto &asset = definition.assets[assetIndex];
                if (!asset.id.IsValid() || asset.tokens.size() > MaximumUiStyleTokens ||
                    asset.classes.size() > MaximumUiStyleClasses || asset.states.size() > MaximumUiStyleStateBlocks ||
                    asset.assignments.size() > MaximumUiStyleProperties)
                    return Failure(UiErrors::StyleInvalid);
                for (std::size_t previous = 0; previous < assetIndex; ++previous)
                    if (definition.assets[previous].id == asset.id)
                        return Failure(UiErrors::StyleIdentityConflict);

                tokenCount += asset.tokens.size();
                classCount += asset.classes.size();
                stateCount += asset.states.size();
                if (tokenCount > MaximumUiStyleTokens || classCount > MaximumUiStyleClasses || stateCount > MaximumUiStyleStateBlocks)
                    return Failure(UiErrors::CapacityExceeded);
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
                for (const auto &assignment : asset.assignments)
                    if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                        return valid;
                for (const auto &state : asset.states) {
                    if (const auto valid = ValidateStateShape(state); valid.HasError())
                        return valid;
                }
                for (std::size_t classIndex = 0; classIndex < asset.classes.size(); ++classIndex) {
                    const auto &styleClass = asset.classes[classIndex];
                    if (!styleClass.id.IsValid() || styleClass.assignments.size() > MaximumUiStyleProperties ||
                        styleClass.states.size() > MaximumUiStyleStateBlocks)
                        return Failure(UiErrors::StyleInvalid);
                    if (styleClass.base.has_value() && !styleClass.base->IsValid())
                        return Failure(UiErrors::StyleReferenceInvalid);
                    for (std::size_t previous = 0; previous < classIndex; ++previous)
                        if (asset.classes[previous].id == styleClass.id)
                            return Failure(UiErrors::StyleIdentityConflict);
                    for (const auto &assignment : styleClass.assignments)
                        if (const auto valid = ValidateAssignmentShape(assignment); valid.HasError())
                            return valid;
                    for (const auto &state : styleClass.states) {
                        if (const auto valid = ValidateStateShape(state); valid.HasError())
                            return valid;
                    }
                    stateCount += styleClass.states.size();
                    if (stateCount > MaximumUiStyleStateBlocks)
                        return Failure(UiErrors::CapacityExceeded);
                }
            }

            for (const auto &asset : definition.assets) {
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
                for (const auto &styleClass : asset.classes) {
                    if (!styleClass.base.has_value())
                        continue;
                    if (FindClass(std::span<const UiStyleAssetDefinition>{definition.assets}, *styleClass.base) == nullptr)
                        return Failure(UiErrors::StyleReferenceInvalid);
                    std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> classChain{};
                    std::size_t classDepth = 0;
                    auto currentClass = UiStyleClassReference{asset.id, styleClass.id};
                    while (currentClass.IsValid()) {
                        if (classDepth == classChain.size())
                            return Failure(UiErrors::StyleCycle);
                        for (std::size_t previous = 0; previous < classDepth; ++previous)
                            if (classChain[previous] == currentClass)
                                return Failure(UiErrors::StyleCycle);
                        classChain[classDepth++] = currentClass;
                        const auto *currentDefinition =
                            FindClass(std::span<const UiStyleAssetDefinition>{definition.assets}, currentClass);
                        if (currentDefinition == nullptr)
                            return Failure(UiErrors::StyleReferenceInvalid);
                        if (currentClass != UiStyleClassReference{asset.id, styleClass.id} && currentDefinition->sealed)
                            return Failure(UiErrors::StyleReferenceInvalid);
                        currentClass = currentDefinition->base.value_or(UiStyleClassReference{});
                    }
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<UiStyleValue> ResolveTokenDefinition(const UiStyleRegistryDefinition &definition,
                                                                  const UiStyleTokenReference reference,
                                                                  std::vector<UiStyleTokenReference> &path,
                                                                  const std::size_t depth) {
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

        struct WorkingProperty final {
            UiComputedStyleProperty value;
            bool sealed{};
        };

        struct WorkingStyle final {
            std::array<WorkingProperty, MaximumUiStyleProperties> properties{};
            std::uint32_t count{};

            [[nodiscard]] WorkingProperty *Find(const UiStylePropertyId id) noexcept {
                for (std::uint32_t index = 0; index < count; ++index)
                    if (properties[index].value.property == id)
                        return &properties[index];
                return nullptr;
            }

            [[nodiscard]] const WorkingProperty *Find(const UiStylePropertyId id) const noexcept {
                for (std::uint32_t index = 0; index < count; ++index)
                    if (properties[index].value.property == id)
                        return &properties[index];
                return nullptr;
            }
        };

        [[nodiscard]] Result<void> ApplyValue(WorkingStyle &style, const UiStylePropertyDescriptor &descriptor,
                                               const UiStyleValue &value, const UiStyleProvenance &provenance,
                                               const bool sealed, const std::uint32_t propertyCapacity) {
            if (!IsValueCompatible(descriptor, value))
                return Failure(UiErrors::StyleTypeMismatch);
            auto *existing = style.Find(descriptor.id);
            if (existing != nullptr) {
                if (existing->sealed)
                    return Failure(UiErrors::StyleReferenceInvalid);
                existing->value.value = value;
                existing->value.provenance = provenance;
                existing->sealed = sealed;
                return Result<void>::Success();
            }
            if (style.count == propertyCapacity || style.count == MaximumUiStyleProperties)
                return Failure(UiErrors::CapacityExceeded);
            style.properties[style.count++] = {{descriptor.id, value, provenance}, sealed};
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyAssignment(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                   const UiStyleAssignment &assignment, const UiStyleOrigin origin,
                                                   const RuntimeStyleAssetId asset, const UiStyleClassId styleClass,
                                                   const std::uint32_t propertyCapacity) {
            const auto *descriptor = FindProperty(registry.Properties(), assignment.property);
            if (descriptor == nullptr)
                return Failure(UiErrors::StyleReferenceInvalid);
            UiStyleValue value{};
            UiStyleTokenId token{};
            RuntimeStyleAssetId provenanceAsset = asset;
            if (assignment.value.referencesToken) {
                if (!assignment.value.token.IsValid())
                    return Failure(UiErrors::StyleReferenceInvalid);
                const auto resolved = registry.ResolveToken(assignment.value.token);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                value = resolved.Value();
                token = assignment.value.token.id;
                provenanceAsset = assignment.value.token.asset;
            } else {
                value = assignment.value.literal;
            }
            const UiStyleProvenance provenance{origin, provenanceAsset, styleClass, token, registry.Generation()};
            return ApplyValue(style, *descriptor, value, provenance, assignment.sealed, propertyCapacity);
        }

        [[nodiscard]] Result<void> BuildAssetChain(const RuntimeStyleRegistry &registry, const RuntimeStyleAssetId asset,
                                                    std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> &chain,
                                                    std::uint32_t &count) {
            count = 0;
            auto current = asset;
            while (current.IsValid()) {
                if (count == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::uint32_t previous = 0; previous < count; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                const auto *definition = FindAsset(registry.Assets(), current);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                chain[count++] = current;
                current = definition->base.value_or(RuntimeStyleAssetId{});
            }
            for (std::uint32_t left = 0; left < count / 2; ++left)
                std::swap(chain[left], chain[count - left - 1]);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildClassChain(const RuntimeStyleRegistry &registry, const UiStyleClassReference classReference,
                                                    std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> &chain,
                                                    std::uint32_t &count) {
            count = 0;
            auto current = classReference;
            while (current.IsValid()) {
                if (count == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::uint32_t previous = 0; previous < count; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                const auto *definition = FindClass(registry.Assets(), current);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                chain[count++] = current;
                current = definition->base.value_or(UiStyleClassReference{});
            }
            for (std::uint32_t left = 0; left < count / 2; ++left)
                std::swap(chain[left], chain[count - left - 1]);
            return Result<void>::Success();
        }

        struct StateApplication final {
            const UiStyleStateOverride *state{};
            UiStyleOrigin origin{UiStyleOrigin::VisualState};
            RuntimeStyleAssetId asset;
            UiStyleClassId styleClass;
            std::uint32_t order{};
        };

        [[nodiscard]] Result<void> AddStateApplications(const UiStyleAssetDefinition &asset, const UiStyleOrigin origin,
                                                        const UiStyleClassId styleClass,
                                                        std::array<StateApplication, MaximumUiStyleStateBlocks> &applications,
                                                        std::uint32_t &count, std::uint32_t &order) {
            for (const auto &state : asset.states) {
                if (count == applications.size())
                    return Failure(UiErrors::CapacityExceeded);
                applications[count++] = {&state, origin, asset.id, styleClass, order++};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddClassStateApplications(
            const RuntimeStyleRegistry &registry, const UiStyleClassReference classReference, const UiStyleOrigin origin,
            std::array<StateApplication, MaximumUiStyleStateBlocks> &applications, std::uint32_t &count, std::uint32_t &order) {
            std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
            std::uint32_t chainCount = 0;
            if (const auto result = BuildClassChain(registry, classReference, chain, chainCount); result.HasError())
                return result;
            for (std::uint32_t index = 0; index < chainCount; ++index) {
                const auto *definition = FindClass(registry.Assets(), chain[index]);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                for (const auto &state : definition->states) {
                    if (count == applications.size())
                        return Failure(UiErrors::CapacityExceeded);
                    applications[count++] = {&state, origin, chain[index].asset, chain[index].id, order++};
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateElementInput(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
                                                         const std::uint32_t propertyCapacity) {
            if (!input.element.IsValid() || !input.asset.IsValid() || !registry.HasAsset(input.asset) || !input.state.IsValid() ||
                input.classes.size() > MaximumUiStyleClasses || input.inlineProperties.size() > propertyCapacity ||
                input.policyProperties.size() > propertyCapacity)
                return Failure(UiErrors::StyleInvalid);
            if (input.typeClass.IsValid() && !registry.HasClass(input.typeClass))
                return Failure(UiErrors::StyleReferenceInvalid);
            if (!input.typeClass.IsValid() && (input.typeClass.asset.IsValid() || input.typeClass.id.IsValid()))
                return Failure(UiErrors::StyleReferenceInvalid);
            for (const auto &classReference : input.classes)
                if (!classReference.IsValid() || !registry.HasClass(classReference))
                    return Failure(UiErrors::StyleReferenceInvalid);
            for (const auto &assignment : input.inlineProperties) {
                if (!assignment.value.IsValid() || assignment.value.referencesToken)
                    return Failure(UiErrors::StyleInvalid);
                const auto *property = FindProperty(registry.Properties(), assignment.property);
                if (property == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (!IsValueCompatible(*property, assignment.value.literal))
                    return Failure(UiErrors::StyleTypeMismatch);
            }
            for (const auto &assignment : input.policyProperties) {
                if (!assignment.value.IsValid() || assignment.value.referencesToken)
                    return Failure(UiErrors::StyleInvalid);
                const auto *property = FindProperty(registry.Properties(), assignment.property);
                if (property == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (!IsValueCompatible(*property, assignment.value.literal))
                    return Failure(UiErrors::StyleTypeMismatch);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<WorkingStyle> ResolveElement(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
                                                          const WorkingStyle *parent, const std::uint32_t propertyCapacity) {
            WorkingStyle style{};
            for (const auto &property : registry.Properties()) {
                const UiStyleProvenance provenance{UiStyleOrigin::RegisteredDefault,
                                                   RuntimeStyleAssetId{},
                                                   UiStyleClassId{},
                                                   UiStyleTokenId{},
                                                   registry.Generation()};
                if (const auto result = ApplyValue(style, property, property.defaultValue, provenance, false, propertyCapacity);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
            }

            std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> assetChain{};
            std::uint32_t assetCount = 0;
            if (const auto result = BuildAssetChain(registry, input.asset, assetChain, assetCount); result.HasError())
                return Result<WorkingStyle>::Failure(result.ErrorValue());
            for (std::uint32_t index = 0; index < assetCount; ++index) {
                const auto *asset = FindAsset(registry.Assets(), assetChain[index]);
                if (asset == nullptr)
                    return Failure<WorkingStyle>(UiErrors::StyleReferenceInvalid);
                for (const auto &assignment : asset->assignments) {
                    if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::StyleAsset, asset->id,
                                                            UiStyleClassId{}, propertyCapacity);
                        result.HasError())
                        return Result<WorkingStyle>::Failure(result.ErrorValue());
                }
            }

            if (parent != nullptr) {
                for (std::uint32_t index = 0; index < parent->count; ++index) {
                    const auto &parentProperty = parent->properties[index].value;
                    const auto *descriptor = FindProperty(registry.Properties(), parentProperty.property);
                    if (descriptor == nullptr || !descriptor->inheritsToChildren)
                        continue;
                    UiStyleProvenance provenance = parentProperty.provenance;
                    provenance.origin = UiStyleOrigin::Inherited;
                    provenance.generation = registry.Generation();
                    if (const auto result = ApplyValue(style, *descriptor, parentProperty.value, provenance, false, propertyCapacity);
                        result.HasError())
                        return Result<WorkingStyle>::Failure(result.ErrorValue());
                }
            }

            if (input.typeClass.IsValid()) {
                std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
                std::uint32_t count = 0;
                if (const auto result = BuildClassChain(registry, input.typeClass, chain, count); result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto *styleClass = FindClass(registry.Assets(), chain[index]);
                    if (styleClass == nullptr)
                        return Failure<WorkingStyle>(UiErrors::StyleReferenceInvalid);
                    for (const auto &assignment : styleClass->assignments) {
                        if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::ElementTypeClass,
                                                                chain[index].asset, chain[index].id, propertyCapacity);
                            result.HasError())
                            return Result<WorkingStyle>::Failure(result.ErrorValue());
                    }
                }
            }

            for (const auto classReference : input.classes) {
                std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
                std::uint32_t count = 0;
                if (const auto result = BuildClassChain(registry, classReference, chain, count); result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto *styleClass = FindClass(registry.Assets(), chain[index]);
                    if (styleClass == nullptr)
                        return Failure<WorkingStyle>(UiErrors::StyleReferenceInvalid);
                    for (const auto &assignment : styleClass->assignments) {
                        if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::AuthoredClass,
                                                                chain[index].asset, chain[index].id, propertyCapacity);
                            result.HasError())
                            return Result<WorkingStyle>::Failure(result.ErrorValue());
                    }
                }
            }

            for (const auto &assignment : input.inlineProperties) {
                if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::Inline, input.asset,
                                                        UiStyleClassId{}, propertyCapacity);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
            }

            std::array<StateApplication, MaximumUiStyleStateBlocks> applications{};
            std::uint32_t applicationCount = 0;
            std::uint32_t order = 0;
            for (std::uint32_t index = 0; index < assetCount; ++index) {
                const auto *asset = FindAsset(registry.Assets(), assetChain[index]);
                if (asset == nullptr)
                    return Failure<WorkingStyle>(UiErrors::StyleReferenceInvalid);
                if (const auto result = AddStateApplications(*asset, UiStyleOrigin::VisualState, UiStyleClassId{}, applications,
                                                              applicationCount, order);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
            }
            if (input.typeClass.IsValid())
                if (const auto result = AddClassStateApplications(registry, input.typeClass, UiStyleOrigin::VisualState, applications,
                                                                    applicationCount, order);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
            for (const auto classReference : input.classes)
                if (const auto result = AddClassStateApplications(registry, classReference, UiStyleOrigin::VisualState, applications,
                                                                    applicationCount, order);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());

            std::sort(applications.begin(), applications.begin() + applicationCount, [](const StateApplication &left,
                                                                                          const StateApplication &right) {
                if (left.state->layer != right.state->layer)
                    return left.state->layer < right.state->layer;
                const auto leftSpecificity = std::popcount(left.state->required.bits);
                const auto rightSpecificity = std::popcount(right.state->required.bits);
                if (leftSpecificity != rightSpecificity)
                    return leftSpecificity < rightSpecificity;
                return left.order < right.order;
            });
            for (std::uint32_t index = 0; index < applicationCount; ++index) {
                const auto &application = applications[index];
                if (!input.state.Contains(application.state->required) ||
                    (input.state.bits & application.state->forbidden.bits) != 0)
                    continue;
                for (const auto &assignment : application.state->assignments) {
                    if (const auto result = ApplyAssignment(style, registry, assignment, application.origin, application.asset,
                                                            application.styleClass, propertyCapacity);
                        result.HasError())
                        return Result<WorkingStyle>::Failure(result.ErrorValue());
                }
            }

            for (const auto &assignment : input.policyProperties) {
                if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::AccessibilityPolicy, input.asset,
                                                        UiStyleClassId{}, propertyCapacity);
                    result.HasError())
                    return Result<WorkingStyle>::Failure(result.ErrorValue());
            }
            return Result<WorkingStyle>::Success(std::move(style));
        }
    }  // namespace

    /** @copydoc UiStyleColor::IsValid */
    bool UiStyleColor::IsValid() const noexcept {
        return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha) && red >= 0.0F &&
               red <= 1.0F && green >= 0.0F && green <= 1.0F && blue >= 0.0F && blue <= 1.0F && alpha >= 0.0F && alpha <= 1.0F &&
               static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(UiStyleColorRole::Status);
    }

    /** @copydoc UiStyleTypography::IsValid */
    bool UiStyleTypography::IsValid() const noexcept {
        return family.IsValid() && weight <= 1000U && stretch >= 50U && stretch <= 200U && style <= 2U && size > 0 && lineHeight > 0;
    }

    /** @copydoc UiStyleImage::IsValid */
    bool UiStyleImage::IsValid() const noexcept {
        return asset.IsValid() && static_cast<std::uint8_t>(fit) <= static_cast<std::uint8_t>(UiStyleImageFit::Tile) &&
               std::all_of(nineSlice.begin(), nineSlice.end(), [](const std::int32_t value) { return value >= 0; }) && tint.IsValid();
    }

    /** @copydoc UiStyleShape::IsValid */
    bool UiStyleShape::IsValid() const noexcept {
        return radius >= 0 && borderWidth >= 0 && outlineWidth >= 0 && shadowOffsetX >= -1'000'000 &&
               shadowOffsetX <= 1'000'000 && shadowOffsetY >= -1'000'000 && shadowOffsetY <= 1'000'000 && shadowBlur >= 0;
    }

    /** @copydoc UiStyleScalar::IsValid */
    bool UiStyleScalar::IsValid() const noexcept {
        return std::isfinite(value);
    }

    /** @copydoc UiStyleValueCategoryOf */
    UiStyleValueCategory UiStyleValueCategoryOf(const UiStyleValue &value) noexcept {
        return std::visit(
            [](const auto &typed) noexcept {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value, UiStyleColor>)
                    return UiStyleValueCategory::Color;
                else if constexpr (std::is_same_v<Value, UiStyleDimension>)
                    return UiStyleValueCategory::Dimension;
                else if constexpr (std::is_same_v<Value, UiStyleTypography>)
                    return UiStyleValueCategory::Typography;
                else if constexpr (std::is_same_v<Value, UiStyleImage>)
                    return UiStyleValueCategory::Imagery;
                else if constexpr (std::is_same_v<Value, UiStyleShape>)
                    return UiStyleValueCategory::Shape;
                else if constexpr (std::is_same_v<Value, UiStyleScalar>)
                    return UiStyleValueCategory::Scalar;
                else if constexpr (std::is_same_v<Value, UiStyleEnumValue>)
                    return UiStyleValueCategory::Enum;
                else
                    return UiStyleValueCategory::Motion;
            },
            value);
    }

    /** @copydoc UiStyleValueSource::Literal */
    UiStyleValueSource UiStyleValueSource::Literal(UiStyleValue value) noexcept {
        UiStyleValueSource source;
        source.literal = std::move(value);
        source.token = {};
        source.referencesToken = false;
        return source;
    }

    /** @copydoc UiStyleValueSource::Token */
    UiStyleValueSource UiStyleValueSource::Token(const UiStyleTokenReference reference) noexcept {
        UiStyleValueSource source;
        source.token = reference;
        source.referencesToken = true;
        return source;
    }

    /** @copydoc UiStyleValueSource::IsValid */
    bool UiStyleValueSource::IsValid() const noexcept {
        return referencesToken ? token.IsValid() : IsValueValid(literal);
    }

    /** @copydoc UiStylePropertyDescriptor::IsValid */
    bool UiStylePropertyDescriptor::IsValid() const noexcept {
        if (!id.IsValid() || !IsKnownCategory(category) || UiStyleValueCategoryOf(defaultValue) != category ||
            !IsValueCompatible(*this, defaultValue))
            return false;
        if (category == UiStyleValueCategory::Scalar &&
            (!std::isfinite(minimumScalar) || !std::isfinite(maximumScalar) || minimumScalar > maximumScalar))
            return false;
        return true;
    }

    /** @copydoc UiVisualStateMask::IsValid */
    bool UiVisualStateMask::IsValid() const noexcept {
        return (bits & static_cast<std::uint16_t>(~KnownVisualStateBits)) == 0;
    }

    struct RuntimeStyleRegistry::Storage final {
        struct FlattenedToken final {
            UiStyleTokenReference reference;
            UiStyleValue value;
        };

        UiStyleRegistryDefinition definition;
        RuntimeStyleGeneration generation;
        RuntimeStyleRegistryState lifecycle{RuntimeStyleRegistryState::Active};
        std::vector<FlattenedToken> flattenedTokens;

        Storage(UiStyleRegistryDefinition source, const RuntimeStyleGeneration sourceGeneration)
            : definition(std::move(source)), generation(sourceGeneration) {}
    };

    /** @copydoc RuntimeStyleRegistry::Create */
    Result<RuntimeStyleRegistry> RuntimeStyleRegistry::Create(UiStyleRegistryDefinition definition,
                                                               const RuntimeStyleGeneration generation) {
        if (!generation.IsValid())
            return Failure<RuntimeStyleRegistry>(UiErrors::StyleInvalid);
        try {
            auto storage = std::make_unique<Storage>(std::move(definition), generation);
            std::ranges::sort(storage->definition.properties, {}, &UiStylePropertyDescriptor::id);
            std::ranges::sort(storage->definition.assets, {}, &UiStyleAssetDefinition::id);
            for (auto &asset : storage->definition.assets) {
                std::ranges::sort(asset.tokens, {}, &UiStyleTokenDefinition::id);
                std::ranges::sort(asset.classes, {}, &UiStyleClassDefinition::id);
            }
            if (const auto valid = ValidateAssetAndClassShape(storage->definition); valid.HasError())
                return Result<RuntimeStyleRegistry>::Failure(valid.ErrorValue());

            std::size_t tokenCount = 0;
            for (const auto &asset : storage->definition.assets)
                tokenCount += asset.tokens.size();
            storage->flattenedTokens.reserve(tokenCount);
            for (const auto &asset : storage->definition.assets) {
                for (const auto &token : asset.tokens) {
                    std::vector<UiStyleTokenReference> path;
                    path.reserve(MaximumUiStyleInheritanceDepth);
                    const auto resolved = ResolveTokenDefinition(storage->definition, {asset.id, token.id}, path, 0);
                    if (resolved.HasError())
                        return Result<RuntimeStyleRegistry>::Failure(resolved.ErrorValue());
                    storage->flattenedTokens.push_back({{asset.id, token.id}, std::move(resolved).Value()});
                }
            }
            return Result<RuntimeStyleRegistry>::Success(RuntimeStyleRegistry{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<RuntimeStyleRegistry>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc RuntimeStyleRegistry::RuntimeStyleRegistry */
    RuntimeStyleRegistry::RuntimeStyleRegistry(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc RuntimeStyleRegistry::~RuntimeStyleRegistry */
    RuntimeStyleRegistry::~RuntimeStyleRegistry() {
        Shutdown();
    }

    /** @copydoc RuntimeStyleRegistry::RuntimeStyleRegistry */
    RuntimeStyleRegistry::RuntimeStyleRegistry(RuntimeStyleRegistry &&) noexcept = default;

    /** @copydoc RuntimeStyleRegistry::operator= */
    RuntimeStyleRegistry &RuntimeStyleRegistry::operator=(RuntimeStyleRegistry &&) noexcept = default;

    /** @copydoc RuntimeStyleRegistry::Generation */
    RuntimeStyleGeneration RuntimeStyleRegistry::Generation() const noexcept {
        return storage_ ? storage_->generation : RuntimeStyleGeneration{};
    }

    /** @copydoc RuntimeStyleRegistry::Properties */
    std::span<const UiStylePropertyDescriptor> RuntimeStyleRegistry::Properties() const noexcept {
        return storage_ && storage_->lifecycle == RuntimeStyleRegistryState::Active ? storage_->definition.properties
                                                                                     : std::span<const UiStylePropertyDescriptor>{};
    }

    /** @copydoc RuntimeStyleRegistry::Assets */
    std::span<const UiStyleAssetDefinition> RuntimeStyleRegistry::Assets() const noexcept {
        return storage_ && storage_->lifecycle == RuntimeStyleRegistryState::Active ? storage_->definition.assets
                                                                                     : std::span<const UiStyleAssetDefinition>{};
    }

    /** @copydoc RuntimeStyleRegistry::HasAsset */
    bool RuntimeStyleRegistry::HasAsset(const RuntimeStyleAssetId asset) const noexcept {
        return FindAsset(Assets(), asset) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::HasClass */
    bool RuntimeStyleRegistry::HasClass(const UiStyleClassReference classReference) const noexcept {
        return classReference.IsValid() && FindClass(Assets(), classReference) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::HasToken */
    bool RuntimeStyleRegistry::HasToken(const UiStyleTokenReference tokenReference) const noexcept {
        return tokenReference.IsValid() && FindToken(Assets(), tokenReference) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::ResolveToken */
    Result<UiStyleValue> RuntimeStyleRegistry::ResolveToken(const UiStyleTokenReference tokenReference) const {
        if (!storage_ || storage_->lifecycle != RuntimeStyleRegistryState::Active)
            return Failure<UiStyleValue>(UiErrors::StyleLifecycleUnavailable);
        if (!tokenReference.IsValid())
            return Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        const auto found = std::find_if(storage_->flattenedTokens.begin(), storage_->flattenedTokens.end(),
                                       [tokenReference](const Storage::FlattenedToken &token) {
                                           return token.reference == tokenReference;
                                       });
        if (found == storage_->flattenedTokens.end())
            return Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        return Result<UiStyleValue>::Success(found->value);
    }

    /** @copydoc RuntimeStyleRegistry::BeginRetirement */
    Result<void> RuntimeStyleRegistry::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != RuntimeStyleRegistryState::Active)
            return Failure(UiErrors::StyleLifecycleUnavailable);
        storage_->lifecycle = RuntimeStyleRegistryState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeStyleRegistry::Shutdown */
    void RuntimeStyleRegistry::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == RuntimeStyleRegistryState::Stopped)
            return;
        storage_->lifecycle = RuntimeStyleRegistryState::Stopped;
        storage_->flattenedTokens.clear();
        storage_->definition.properties.clear();
        storage_->definition.assets.clear();
    }

    /** @copydoc RuntimeStyleRegistry::State */
    RuntimeStyleRegistryState RuntimeStyleRegistry::State() const noexcept {
        return storage_ ? storage_->lifecycle : RuntimeStyleRegistryState::Stopped;
    }

    /** @copydoc UiStyleSourceRevisions::IsValid */
    bool UiStyleSourceRevisions::IsValid() const noexcept {
        return document.IsValid() && tree.IsValid() && registry.IsValid() && content.IsValid() && policy.IsValid() && interaction.IsValid();
    }

    /** @copydoc UiStyleResolverDescriptor::IsValid */
    bool UiStyleResolverDescriptor::IsValid() const noexcept {
        return SameOwner(instance, canvas) && document.IsValid() && elementCapacity > 0 && elementCapacity <= MaximumUiStyleElements &&
               propertyCapacity > 0 && propertyCapacity <= MaximumUiStyleProperties && invalidationCapacity > 0 &&
               invalidationCapacity <= MaximumUiStructuralCommands && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiStyleSnapshotsInFlight && initialRegistryGeneration.IsValid() &&
               initialPublication.IsValid();
    }

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

        Storage(const std::uint32_t elementCapacity, const std::uint32_t propertyCapacity) {
            records.reserve(elementCapacity);
            lookup.reserve(elementCapacity);
            styles.reserve(elementCapacity);
            properties.reserve(static_cast<std::size_t>(elementCapacity) * propertyCapacity);
        }
    };

    /** @copydoc UiComputedStyleSnapshot::Retain */
    void UiComputedStyleSnapshot::Retain() const noexcept {
        if (storage_)
            storage_->leases.fetch_add(1);
    }

    /** @copydoc UiComputedStyleSnapshot::Release */
    void UiComputedStyleSnapshot::Release() noexcept {
        if (storage_) {
            storage_->leases.fetch_sub(1);
            storage_.reset();
        }
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiComputedStyleSnapshot::~UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::~UiComputedStyleSnapshot() {
        Release();
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(const UiComputedStyleSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiComputedStyleSnapshot::operator= */
    UiComputedStyleSnapshot &UiComputedStyleSnapshot::operator=(const UiComputedStyleSnapshot &other) noexcept {
        if (this != &other) {
            Release();
            storage_ = other.storage_;
            Retain();
        }
        return *this;
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(UiComputedStyleSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiComputedStyleSnapshot::operator= */
    UiComputedStyleSnapshot &UiComputedStyleSnapshot::operator=(UiComputedStyleSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiComputedStyleSnapshot::Descriptor */
    const UiComputedStyleSnapshotDescriptor &UiComputedStyleSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiComputedStyleSnapshot::Records */
    std::span<const UiComputedStyleRecord> UiComputedStyleSnapshot::Records() const noexcept {
        return storage_ ? std::span<const UiComputedStyleRecord>{storage_->records} : std::span<const UiComputedStyleRecord>{};
    }

    /** @copydoc UiComputedStyleSnapshot::Properties */
    std::span<const UiComputedStyleProperty> UiComputedStyleSnapshot::Properties(const UiComputedStyleRecord &record) const noexcept {
        if (!storage_ || record.firstProperty > storage_->properties.size() ||
            record.propertyCount > storage_->properties.size() - record.firstProperty)
            return {};
        return {storage_->properties.data() + record.firstProperty, record.propertyCount};
    }

    /** @copydoc UiComputedStyleSnapshot::Get */
    Result<UiComputedStyleRecord> UiComputedStyleSnapshot::Get(const UiElementHandle element) const {
        if (!storage_ || !element.IsValid())
            return Failure<UiComputedStyleRecord>(UiErrors::StyleInvalid);
        const auto found = std::lower_bound(storage_->lookup.begin(), storage_->lookup.end(), element,
                                            [this](const std::uint32_t index, const UiElementHandle value) {
                                                return storage_->records[index].element < value;
                                            });
        if (found == storage_->lookup.end() || storage_->records[*found].element != element)
            return Failure<UiComputedStyleRecord>(UiErrors::HandleStale);
        return Result<UiComputedStyleRecord>::Success(storage_->records[*found]);
    }

    struct UiStyleResolver::Storage final {
        struct Node final {
            UiElementHandle element;
            std::int32_t parent{-1};
            WorkingStyle style;
            std::uint64_t contentHash{};
            std::uint64_t stateHash{};
            bool dirty{};
        };

        UiStyleResolverDescriptor descriptor;
        UiStyleResolverState lifecycle{UiStyleResolverState::Active};
        std::vector<Node> activeNodes;
        std::vector<Node> candidateNodes;
        std::vector<UiElementHandle> traversalScratch;
        std::vector<UiStyleInvalidation> invalidations;
        std::vector<std::shared_ptr<UiComputedStyleSnapshot::Storage>> slots;
        std::shared_ptr<UiComputedStyleSnapshot::Storage> current;
        UiStyleSourceRevisions sources;
        UiStylePublicationRevision publication;
        bool hasPublication{};
        std::size_t nextSlot{};

        explicit Storage(const UiStyleResolverDescriptor &source)
            : descriptor(source), publication(source.initialPublication) {
            activeNodes.reserve(source.elementCapacity);
            candidateNodes.reserve(source.elementCapacity);
            traversalScratch.resize(source.elementCapacity);
            invalidations.reserve(source.invalidationCapacity);
            slots.reserve(source.concurrentSnapshots);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiComputedStyleSnapshot::Storage>(source.elementCapacity, source.propertyCapacity));
        }

        ~Storage() {
            ReleaseCurrent();
        }

        void MarkAll() noexcept {
            for (auto &node : candidateNodes)
                node.dirty = true;
        }

        void MarkSubtree(const std::uint32_t root) noexcept {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                auto ancestor = static_cast<std::int32_t>(index);
                while (ancestor >= 0 && static_cast<std::uint32_t>(ancestor) != root)
                    ancestor = candidateNodes[static_cast<std::uint32_t>(ancestor)].parent;
                if (ancestor >= 0)
                    candidateNodes[index].dirty = true;
            }
        }

        [[nodiscard]] std::uint32_t FindNode(const UiElementHandle element) const noexcept {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index)
                if (candidateNodes[index].element == element)
                    return index;
            return std::numeric_limits<std::uint32_t>::max();
        }

        [[nodiscard]] Result<void> BuildTopology(const UiElementTree &tree) {
            const auto preorder = tree.Preorder(std::span<UiElementHandle>{traversalScratch.data(), tree.Size()});
            if (preorder.HasError())
                return Result<void>::Failure(preorder.ErrorValue());
            const auto count = static_cast<std::uint32_t>(preorder.Value());
            if (count == 0 || count > descriptor.elementCapacity)
                return Failure(UiErrors::CapacityExceeded);
            candidateNodes.clear();
            candidateNodes.resize(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                candidateNodes[index].element = traversalScratch[index];
                const auto record = tree.Get(traversalScratch[index]);
                if (record.HasError())
                    return Result<void>::Failure(record.ErrorValue());
                if (!record.Value().parent.IsValid()) {
                    if (index != 0)
                        return Failure(UiErrors::StyleSourceStale);
                } else {
                    const auto parent = std::find(traversalScratch.begin(), traversalScratch.begin() + index, record.Value().parent);
                    if (parent == traversalScratch.begin() + index)
                        return Failure(UiErrors::StyleSourceStale);
                    candidateNodes[index].parent = static_cast<std::int32_t>(parent - traversalScratch.begin());
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyInvalidations(const UiRuntimeTreeRevision treeRevision) {
            for (const auto &invalidation : invalidations) {
                if (!invalidation.tree.IsValid() || invalidation.tree != treeRevision)
                    return Failure(UiErrors::StyleSourceStale);
                switch (invalidation.kind) {
                    case UiStyleInvalidationKind::All:
                        MarkAll();
                        break;
                    case UiStyleInvalidationKind::Subtree: {
                        const auto index = FindNode(invalidation.element);
                        if (index == std::numeric_limits<std::uint32_t>::max())
                            return Failure(UiErrors::StyleSourceStale);
                        MarkSubtree(index);
                        break;
                    }
                    case UiStyleInvalidationKind::Paint:
                    case UiStyleInvalidationKind::Measure: {
                        const auto index = FindNode(invalidation.element);
                        if (index == std::numeric_limits<std::uint32_t>::max())
                            return Failure(UiErrors::StyleSourceStale);
                        candidateNodes[index].dirty = true;
                        break;
                    }
                    default:
                        return Failure(UiErrors::StyleInvalid);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PrepareCandidate(const UiElementTree &tree, const UiStyleUpdateRequest &request) {
            const bool topologyChanged = !hasPublication || sources.tree != request.sources.tree;
            if (topologyChanged) {
                if (const auto result = BuildTopology(tree); result.HasError())
                    return result;
                MarkAll();
            } else {
                candidateNodes = activeNodes;
                for (auto &node : candidateNodes)
                    node.dirty = false;
            }
            const bool registryChanged = !hasPublication || sources.registry != request.sources.registry;
            const bool contentChanged = !hasPublication || sources.content != request.sources.content;
            const bool policyChanged = !hasPublication || sources.policy != request.sources.policy;
            if (registryChanged || contentChanged || policyChanged)
                MarkAll();
            for (std::uint32_t index = 0; index < request.elements.size(); ++index) {
                auto &node = candidateNodes[index];
                const auto &input = request.elements[index];
                const auto contentHash = HashElementContent(input);
                const auto stateHash = HashElementState(input.state);
                if (!topologyChanged && contentHash != activeNodes[index].contentHash)
                    MarkSubtree(index);
                else if (!topologyChanged && stateHash != activeNodes[index].stateHash)
                    node.dirty = true;
                node.contentHash = contentHash;
                node.stateHash = stateHash;
            }
            return ApplyInvalidations(request.sources.tree);
        }

        [[nodiscard]] Result<void> ResolveCandidate(const RuntimeStyleRegistry &registry,
                                                     const UiStyleUpdateRequest &request) {
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                auto &node = candidateNodes[index];
                if (!node.dirty)
                    continue;
                const WorkingStyle *parent = node.parent < 0 ? nullptr : &candidateNodes[static_cast<std::uint32_t>(node.parent)].style;
                auto resolved = ResolveElement(registry, request.elements[index], parent, descriptor.propertyCapacity);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                node.style = std::move(resolved).Value();
                node.dirty = false;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::shared_ptr<UiComputedStyleSnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                    continue;
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        void ReleaseCurrent() noexcept {
            if (current) {
                current->leases.fetch_sub(1);
                current.reset();
            }
        }

        [[nodiscard]] Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>> Publish(
            const UiStyleUpdateRequest &request) {
            UiStylePublicationRevision nextPublication = descriptor.initialPublication;
            if (hasPublication) {
                const auto next = publication.Next();
                if (next.HasError())
                    return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Failure(next.ErrorValue());
                nextPublication = next.Value();
            }
            auto slot = TryAcquire();
            if (!slot)
                return Failure<std::shared_ptr<UiComputedStyleSnapshot::Storage>>(UiErrors::StyleSnapshotStorageExhausted);
            const auto releaseSlotOnFailure = [&slot]() noexcept {
                if (slot)
                    slot->leases.fetch_sub(1);
            };
            slot->descriptor = {descriptor.instance, descriptor.canvas, descriptor.document, request.sources, nextPublication};
            slot->records.resize(candidateNodes.size());
            slot->lookup.resize(candidateNodes.size());
            slot->properties.clear();
            slot->styles.clear();
            for (std::uint32_t index = 0; index < candidateNodes.size(); ++index) {
                const auto &working = candidateNodes[index].style;
                UiComputedStyleSnapshot::Storage::StyleRange *range = nullptr;
                for (auto &existing : slot->styles) {
                    if (existing.propertyCount != working.count)
                        continue;
                    const auto existingBegin = slot->properties.begin() + existing.firstProperty;
                    bool equal = true;
                    for (std::uint32_t property = 0; property < working.count; ++property) {
                        if (existingBegin[property].property != working.properties[property].value.property ||
                            existingBegin[property].value != working.properties[property].value.value ||
                            existingBegin[property].provenance != working.properties[property].value.provenance) {
                            equal = false;
                            break;
                        }
                    }
                    if (equal) {
                        range = &existing;
                        break;
                    }
                }
                if (range == nullptr) {
                    if (slot->styles.size() >= descriptor.elementCapacity || slot->properties.size() + working.count >
                                                                             static_cast<std::size_t>(descriptor.elementCapacity) *
                                                                                 descriptor.propertyCapacity)
                    {
                        releaseSlotOnFailure();
                        return Failure<std::shared_ptr<UiComputedStyleSnapshot::Storage>>(UiErrors::CapacityExceeded);
                    }
                    const auto styleIndex = static_cast<std::uint32_t>(slot->styles.size());
                    const auto styleId = UiComputedStyleId::Create(static_cast<std::uint64_t>(styleIndex + 1));
                    if (styleId.HasError()) {
                        releaseSlotOnFailure();
                        return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Failure(styleId.ErrorValue());
                    }
                    const auto firstProperty = static_cast<std::uint32_t>(slot->properties.size());
                    for (std::uint32_t property = 0; property < working.count; ++property)
                        slot->properties.push_back(working.properties[property].value);
                    slot->styles.push_back({styleId.Value(), firstProperty, working.count});
                    range = &slot->styles.back();
                }
                slot->records[index] = {candidateNodes[index].element,
                                        range->id,
                                        request.elements[index].state,
                                        range->firstProperty,
                                        range->propertyCount};
                slot->lookup[index] = index;
            }
            std::ranges::sort(slot->lookup, {}, [&records = slot->records](const std::uint32_t index) {
                return records[index].element;
            });
            ReleaseCurrent();
            current = slot;
            activeNodes.swap(candidateNodes);
            sources = request.sources;
            publication = nextPublication;
            hasPublication = true;
            invalidations.clear();
            slot->leases.fetch_add(1);
            return Result<std::shared_ptr<UiComputedStyleSnapshot::Storage>>::Success(std::move(slot));
        }
    };

    /** @copydoc UiStyleResolver::Create */
    Result<UiStyleResolver> UiStyleResolver::Create(const UiStyleResolverDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiStyleResolver>(UiErrors::StyleInvalid);
        try {
            return Result<UiStyleResolver>::Success(UiStyleResolver{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiStyleResolver>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiStyleResolver::~UiStyleResolver */
    UiStyleResolver::~UiStyleResolver() {
        Shutdown();
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(UiStyleResolver &&) noexcept = default;

    /** @copydoc UiStyleResolver::operator= */
    UiStyleResolver &UiStyleResolver::operator=(UiStyleResolver &&) noexcept = default;

    /** @copydoc UiStyleResolver::Invalidate */
    Result<void> UiStyleResolver::Invalidate(const UiStyleInvalidation &invalidation) {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return Failure(UiErrors::StyleLifecycleUnavailable);
        if (!invalidation.tree.IsValid())
            return Failure(UiErrors::StyleInvalid);
        if (invalidation.kind != UiStyleInvalidationKind::All && !invalidation.element.IsValid())
            return Failure(UiErrors::StyleInvalid);
        if (invalidation.kind == UiStyleInvalidationKind::All) {
            storage_->invalidations.clear();
            storage_->invalidations.push_back(invalidation);
            return Result<void>::Success();
        }
        const auto existing = std::find_if(storage_->invalidations.begin(), storage_->invalidations.end(),
                                           [&invalidation](const UiStyleInvalidation &queued) {
                                               return queued.tree == invalidation.tree && queued.element == invalidation.element;
                                           });
        if (existing != storage_->invalidations.end()) {
            const auto strength = [](const UiStyleInvalidationKind kind) {
                switch (kind) {
                    case UiStyleInvalidationKind::Paint:
                        return 0;
                    case UiStyleInvalidationKind::Measure:
                        return 1;
                    case UiStyleInvalidationKind::Subtree:
                        return 2;
                    case UiStyleInvalidationKind::All:
                        return 3;
                }
                return 0;
            };
            if (strength(invalidation.kind) > strength(existing->kind))
                existing->kind = invalidation.kind;
            return Result<void>::Success();
        }
        if (storage_->invalidations.size() == storage_->descriptor.invalidationCapacity)
            return Failure(UiErrors::CapacityExceeded);
        storage_->invalidations.push_back(invalidation);
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Update */
    Result<UiComputedStyleSnapshot> UiStyleResolver::Update(const UiElementTree &tree, const RuntimeStyleRegistry &registry,
                                                            const UiStyleUpdateRequest &request) {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleLifecycleUnavailable);
        if (registry.State() != RuntimeStyleRegistryState::Active)
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleLifecycleUnavailable);
        if (!request.sources.IsValid() || request.elements.empty() || request.elements.size() > storage_->descriptor.elementCapacity)
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleInvalid);
        if (request.sources.registry != registry.Generation())
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        if (!storage_->hasPublication && request.sources.registry != storage_->descriptor.initialRegistryGeneration)
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        if (tree.State() != UiElementTreeState::Active || tree.Instance() != storage_->descriptor.instance ||
            tree.Canvas() != storage_->descriptor.canvas || tree.SourceDocument() != storage_->descriptor.document ||
            tree.SourceDocumentRevision() != request.sources.document || tree.Revision() != request.sources.tree ||
            tree.Size() != request.elements.size() || tree.Size() > storage_->descriptor.elementCapacity)
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        for (std::size_t index = 0; index < request.elements.size(); ++index)
            if (const auto valid = ValidateElementInput(registry, request.elements[index], storage_->descriptor.propertyCapacity);
                valid.HasError())
                return Result<UiComputedStyleSnapshot>::Failure(valid.ErrorValue());

        const auto preorder = tree.Preorder(std::span<UiElementHandle>{storage_->traversalScratch.data(), tree.Size()});
        if (preorder.HasError())
            return Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        for (std::size_t index = 0; index < request.elements.size(); ++index)
            if (request.elements[index].element != storage_->traversalScratch[index])
                return Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);

        if (const auto prepared = storage_->PrepareCandidate(tree, request); prepared.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(prepared.ErrorValue());
        const bool sourcesChanged = !storage_->hasPublication || storage_->sources != request.sources;
        const bool anyDirty = std::ranges::any_of(storage_->candidateNodes, [](const Storage::Node &node) { return node.dirty; });
        if (!sourcesChanged && !anyDirty && storage_->invalidations.empty()) {
            storage_->current->leases.fetch_add(1);
            return Result<UiComputedStyleSnapshot>::Success(UiComputedStyleSnapshot{storage_->current});
        }
        if (const auto resolved = storage_->ResolveCandidate(registry, request); resolved.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(resolved.ErrorValue());
        auto published = storage_->Publish(request);
        if (published.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(published.ErrorValue());
        return Result<UiComputedStyleSnapshot>::Success(UiComputedStyleSnapshot{std::move(published).Value()});
    }

    /** @copydoc UiStyleResolver::BeginRetirement */
    Result<void> UiStyleResolver::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return Failure(UiErrors::StyleLifecycleUnavailable);
        storage_->lifecycle = UiStyleResolverState::Retiring;
        storage_->invalidations.clear();
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Shutdown */
    void UiStyleResolver::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiStyleResolverState::Stopped)
            return;
        storage_->lifecycle = UiStyleResolverState::Stopped;
        storage_->invalidations.clear();
        storage_->activeNodes.clear();
        storage_->candidateNodes.clear();
        storage_->traversalScratch.clear();
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiStyleResolver::State */
    UiStyleResolverState UiStyleResolver::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiStyleResolverState::Stopped;
    }

    /** @copydoc UiStyleResolver::IsDrained */
    bool UiStyleResolver::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [&storage = *storage_](const std::shared_ptr<UiComputedStyleSnapshot::Storage> &slot) {
            const auto leases = slot->leases.load();
            return leases == 0 || (slot == storage.current && leases == 1);
        });
    }
}  // namespace Horo::Runtime::Ui
