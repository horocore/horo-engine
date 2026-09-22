#pragma once

#include "Horo/Runtime/Ui/UiAssetLoading.h"

#include <atomic>
#include <mutex>

namespace Horo::Runtime::Ui {
    struct UiRuntimeAssetLoadHandle::Request {
        struct DependencyEntry final {
            UiAssetDependency dependency;
            std::shared_ptr<Assets::AssetLoadHandle> load;
            std::shared_ptr<const std::vector<std::uint8_t>> payload;
            bool skipped{};
        };

        std::weak_ptr<UiRuntimeAssetLoadService::State> owner;
        UiRuntimeAssetLoadRequest request;
        Assets::AssetRegistrySnapshot snapshot;
        CancellationToken parentCancellation;
        std::shared_ptr<Assets::AssetLoadHandle> rootLoad;
        std::optional<CookedUiDocument> document;
        std::vector<DependencyEntry> dependencies;
        std::size_t activeLoads{};
        std::size_t residentBytes{};
        std::atomic<UiRuntimeAssetLoadState> state{UiRuntimeAssetLoadState::Queued};
        std::mutex mutex;
        std::optional<Result<UiRuntimeAssetLoadResult>> result;
        bool consumed{};
    };

    struct UiRuntimeAssetLoadService::State {
        State(Assets::AssetRegistry &assetRegistry, Assets::AssetLoadService &assetLoadService, UiRuntimeAssetLoadLimits loadLimits)
            : registry(assetRegistry), loads(assetLoadService), limits(loadLimits) {}

        Assets::AssetRegistry &registry;
        Assets::AssetLoadService &loads;
        UiRuntimeAssetLoadLimits limits;
        std::mutex mutex;
        std::vector<std::shared_ptr<UiRuntimeAssetLoadHandle::Request>> requests;
        bool accepting{true};
    };

    namespace UiAssetLoadingDetail {
        [[nodiscard]] bool IsTerminal(UiRuntimeAssetLoadState state) noexcept;

        [[nodiscard]] Error TranslateLoadError(const Error &error);

        void CancelHandles(UiRuntimeAssetLoadHandle::Request &request) noexcept;

        void CompleteFailure(UiRuntimeAssetLoadHandle::Request &request, Error error, bool cancelled = false);

        [[nodiscard]] Result<void> AdvanceRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request,
                                                  UiRuntimeAssetLoadService::State &state);

        void DrainRequest(const std::shared_ptr<UiRuntimeAssetLoadHandle::Request> &request) noexcept;
    }  // namespace UiAssetLoadingDetail
}  // namespace Horo::Runtime::Ui
