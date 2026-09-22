#include "UiStyleInternal.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
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
            return StyleInternal::Failure<RuntimeStyleRegistry>(UiErrors::StyleInvalid);
        try {
            auto storage = std::make_unique<Storage>(std::move(definition), generation);
            std::ranges::sort(storage->definition.properties, {}, &UiStylePropertyDescriptor::id);
            std::ranges::sort(storage->definition.assets, {}, &UiStyleAssetDefinition::id);
            for (auto &asset : storage->definition.assets) {
                std::ranges::sort(asset.tokens, {}, &UiStyleTokenDefinition::id);
                std::ranges::sort(asset.classes, {}, &UiStyleClassDefinition::id);
            }
            if (const auto valid = StyleInternal::ValidateAssetAndClassShape(storage->definition); valid.HasError())
                return Result<RuntimeStyleRegistry>::Failure(valid.ErrorValue());

            std::size_t tokenCount = 0;
            for (const auto &asset : storage->definition.assets)
                tokenCount += asset.tokens.size();
            storage->flattenedTokens.reserve(tokenCount);
            for (const auto &asset : storage->definition.assets) {
                for (const auto &token : asset.tokens) {
                    std::vector<UiStyleTokenReference> path;
                    path.reserve(MaximumUiStyleInheritanceDepth);
                    const auto resolved = StyleInternal::ResolveTokenDefinition(storage->definition, {asset.id, token.id}, path, 0);
                    if (resolved.HasError())
                        return Result<RuntimeStyleRegistry>::Failure(resolved.ErrorValue());
                    storage->flattenedTokens.push_back({{asset.id, token.id}, std::move(resolved).Value()});
                }
            }
            return Result<RuntimeStyleRegistry>::Success(RuntimeStyleRegistry{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return StyleInternal::Failure<RuntimeStyleRegistry>(UiErrors::CapacityExceeded);
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
        return StyleInternal::FindAsset(Assets(), asset) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::HasClass */
    bool RuntimeStyleRegistry::HasClass(const UiStyleClassReference classReference) const noexcept {
        return classReference.IsValid() && StyleInternal::FindClass(Assets(), classReference) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::HasToken */
    bool RuntimeStyleRegistry::HasToken(const UiStyleTokenReference tokenReference) const noexcept {
        return tokenReference.IsValid() && StyleInternal::FindToken(Assets(), tokenReference) != nullptr;
    }

    /** @copydoc RuntimeStyleRegistry::ResolveToken */
    Result<UiStyleValue> RuntimeStyleRegistry::ResolveToken(const UiStyleTokenReference tokenReference) const {
        if (!storage_ || storage_->lifecycle != RuntimeStyleRegistryState::Active)
            return StyleInternal::Failure<UiStyleValue>(UiErrors::StyleLifecycleUnavailable);
        if (!tokenReference.IsValid())
            return StyleInternal::Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        const auto found = std::find_if(storage_->flattenedTokens.begin(), storage_->flattenedTokens.end(),
                                        [tokenReference](const Storage::FlattenedToken &token) {
            return token.reference == tokenReference;
        });
        if (found == storage_->flattenedTokens.end())
            return StyleInternal::Failure<UiStyleValue>(UiErrors::StyleReferenceInvalid);
        return Result<UiStyleValue>::Success(found->value);
    }

    /** @copydoc RuntimeStyleRegistry::BeginRetirement */
    Result<void> RuntimeStyleRegistry::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != RuntimeStyleRegistryState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
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
}  // namespace Horo::Runtime::Ui
