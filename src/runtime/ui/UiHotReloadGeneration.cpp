#include "UiHotReloadInternal.h"

#include <algorithm>
#include <memory>
#include <new>

namespace Horo::Runtime::Ui {
    /** @copydoc UiReloadGeneration::Create */
    Result<UiReloadGeneration> UiReloadGeneration::Create(UiRuntimeAssetLoadResult loaded, RuntimeUiInstanceId instance,
                                                          std::vector<UiReloadCanvas> canvases) {
        if (!loaded.rootAsset.IsValid() || loaded.registryRevision.value == 0)
            return Result<UiReloadGeneration>::Failure(MakeError(UiErrors::AssetIdentityMismatch));
        const auto asset = loaded.rootAsset;
        const auto revision = loaded.registryRevision;
        auto runtime = std::move(loaded).CreateInstance(instance);
        if (runtime.HasError())
            return Result<UiReloadGeneration>::Failure(runtime.ErrorValue());
        if (const auto valid = UiReloadDetail::Validate(runtime.Value(), canvases); valid.HasError())
            return Result<UiReloadGeneration>::Failure(valid.ErrorValue());
        try {
            return Result<UiReloadGeneration>::Success(
                UiReloadGeneration{std::make_unique<Storage>(std::move(runtime).Value(), asset, revision, std::move(canvases))});
        } catch (const std::bad_alloc &) {
            return Result<UiReloadGeneration>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiReloadGeneration::UiReloadGeneration */
    UiReloadGeneration::UiReloadGeneration(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiReloadGeneration::~UiReloadGeneration */
    UiReloadGeneration::~UiReloadGeneration() = default;
    /** @copydoc UiReloadGeneration::UiReloadGeneration */
    UiReloadGeneration::UiReloadGeneration(UiReloadGeneration &&) noexcept = default;
    /** @copydoc UiReloadGeneration::operator= */
    UiReloadGeneration &UiReloadGeneration::operator=(UiReloadGeneration &&) noexcept = default;

    /** @copydoc UiReloadGeneration::Instance */
    const UiRuntimeInstance &UiReloadGeneration::Instance() const noexcept {
        return storage_->instance;
    }

    /** @copydoc UiReloadGeneration::Asset */
    Assets::AssetId UiReloadGeneration::Asset() const noexcept {
        return storage_->rootAsset;
    }

    /** @copydoc UiReloadGeneration::RegistryRevision */
    Assets::AssetRegistryRevision UiReloadGeneration::RegistryRevision() const noexcept {
        return storage_->registryRevision;
    }

    /** @copydoc UiReloadGeneration::Canvases */
    std::span<const UiReloadCanvas> UiReloadGeneration::Canvases() const noexcept {
        return storage_->canvases;
    }

    /** @copydoc UiReloadGeneration::Canvas */
    UiReloadCanvas *UiReloadGeneration::Canvas(UiCanvasId id) noexcept {
        const auto found = std::ranges::find(storage_->canvases, id, &UiReloadCanvas::id);
        return found == storage_->canvases.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc UiHotReloadLimits::IsValid */
    bool UiHotReloadLimits::IsValid() const noexcept {
        return maximumRetiredGenerations > 0 && maximumRetiredGenerations <= 64 && maximumPreparedGenerations > 0 &&
               maximumPreparedGenerations <= 64;
    }

    /** @copydoc UiReloadLease::UiReloadLease */
    UiReloadLease::UiReloadLease(std::shared_ptr<UiReloadGeneration> generation) noexcept : generation_(std::move(generation)) {}

    /** @copydoc UiReloadLease::Get */
    const UiReloadGeneration *UiReloadLease::Get() const noexcept {
        return generation_.get();
    }
}  // namespace Horo::Runtime::Ui
