#include "Horo/Runtime/Ui/UiDocument.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool HasDependency(const std::span<const UiAssetDependency> dependencies, const UiRuntimeAsset &asset) noexcept {
            return std::ranges::any_of(dependencies, [&](const UiAssetDependency &dependency) {
                return dependency == asset.dependency;
            });
        }

        [[nodiscard]] bool HasResolvedAsset(const std::vector<UiRuntimeAsset> &assets, const Assets::AssetId id) noexcept {
            return std::ranges::any_of(assets, [&](const UiRuntimeAsset &asset) {
                return asset.dependency.asset == id;
            });
        }

        [[nodiscard]] Result<void> ValidateRuntimeAssets(const CookedUiDocument &document, const std::vector<UiRuntimeAsset> &assets) {
            for (const UiRuntimeAsset &asset : assets) {
                if (!asset.dependency.asset.IsValid() || asset.dependency.expectedType.Value().empty() || !asset.payload ||
                    asset.payload->empty() || !HasDependency(document.Dependencies(), asset))
                    return Failure(UiErrors::DependencyInvalid);
            }
            for (std::size_t index = 0; index < assets.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (assets[previous].dependency.asset == assets[index].dependency.asset)
                        return Failure(UiErrors::DependencyInvalid);
            for (const UiAssetDependency &dependency : document.Dependencies())
                if (dependency.required && !HasResolvedAsset(assets, dependency.asset))
                    return Failure(UiErrors::DependencyInvalid);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiRuntimeInstance::UiRuntimeInstance */
    UiRuntimeInstance::UiRuntimeInstance(InitialState initialState) noexcept
        : document_(std::move(initialState.document)), assets_(std::move(initialState.assets)), instance_(initialState.instance) {}

    /** @copydoc UiRuntimeInstance::Create */
    Result<UiRuntimeInstance> UiRuntimeInstance::Create(CookedUiDocument document, RuntimeUiInstanceId instance) {
        if (!instance.IsValid())
            return Failure<UiRuntimeInstance>(UiErrors::HandleMalformed);
        if (!document.Id().IsValid() || !document.SourceRevision().IsValid() || document.Payload().empty())
            return Failure<UiRuntimeInstance>(UiErrors::PayloadInvalid);
        return Result<UiRuntimeInstance>::Success(UiRuntimeInstance{InitialState{std::move(document), {}, instance}});
    }

    /** @copydoc UiRuntimeInstance::Create */
    Result<UiRuntimeInstance> UiRuntimeInstance::Create(CookedUiDocument document, RuntimeUiInstanceId instance,
                                                        std::vector<UiRuntimeAsset> assets) {
        if (!instance.IsValid())
            return Failure<UiRuntimeInstance>(UiErrors::HandleMalformed);
        if (!document.Id().IsValid() || !document.SourceRevision().IsValid() || document.Payload().empty())
            return Failure<UiRuntimeInstance>(UiErrors::PayloadInvalid);
        if (const auto validated = ValidateRuntimeAssets(document, assets); validated.HasError())
            return Result<UiRuntimeInstance>::Failure(validated.ErrorValue());
        std::ranges::sort(assets, {}, [](const UiRuntimeAsset &asset) {
            return asset.dependency.asset;
        });
        return Result<UiRuntimeInstance>::Success(UiRuntimeInstance{InitialState{std::move(document), std::move(assets), instance}});
    }
}  // namespace Horo::Runtime::Ui
