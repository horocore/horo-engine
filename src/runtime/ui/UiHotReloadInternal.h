#pragma once

#include "Horo/Runtime/Ui/UiHotReload.h"

namespace Horo::Runtime::Ui::UiReloadDetail {
    /** @brief Validates actual cooked provenance, owner evidence and complete canvas topology. */
    [[nodiscard]] Result<void> Validate(const UiRuntimeInstance &instance, std::span<const UiReloadCanvas> canvases);
    /** @brief Returns the complete tree namespace envelope, rejecting overlapping reserved ranges. */
    [[nodiscard]] Result<std::pair<std::uint64_t, std::uint64_t>> Namespace(std::span<const UiReloadCanvas> canvases);
    /** @brief Reconciles only compatible stable authored state in privately owned replacement owners. */
    [[nodiscard]] Result<UiReloadReconciliation> Reconcile(const UiReloadGeneration &old, UiReloadGeneration &replacement);

    /** @brief Copied actual owner evidence; used only for prepared-candidate admission, never as an alternate state owner. */
    struct CanvasStamp final {
        UiCanvasId id;
        UiRuntimeTreeRevision tree;
        std::optional<UiFocusSnapshot> focus;
        std::optional<UiRouteStackRevision> routes;
        std::optional<UiBindingApplyResult> bindings;
        std::optional<UiLayoutSnapshotDescriptor> layout;
        std::optional<UiLayoutClipSnapshotDescriptor> clipped;
        std::vector<UiLayoutScrollRecord> scrolls;
        std::vector<UiLayoutClipDescriptor> policies;
        std::vector<std::pair<UiElementId, UiControlReloadStamp>> controls;
    };

    /** @brief Copies bounded actual owner revisions/state at load time, before reconciliation. */
    [[nodiscard]] Result<std::vector<CanvasStamp>> CaptureSource(const UiReloadGeneration &generation);
    /** @brief Rejects any changed actual owner source before publishing a prepared generation. */
    [[nodiscard]] bool MatchesSource(const UiReloadGeneration &generation, std::span<const CanvasStamp> stamps) noexcept;
    /** @brief Closes every real old owner's admission without destroying retained storage. */
    void Retire(UiReloadGeneration &generation) noexcept;
    /** @brief Abandons deferred authority reservations outside frame work while retaining every generation/module pin. */
    [[nodiscard]] Result<void> DrainBindings(UiReloadGeneration &generation);
    /** @brief Releases generation-owned snapshots and checks external typed-owner drain before reclamation. */
    [[nodiscard]] bool Drained(UiReloadGeneration &generation) noexcept;
}  // namespace Horo::Runtime::Ui::UiReloadDetail

namespace Horo::Runtime::Ui {
    struct UiReloadGeneration::Storage final {
        Storage(UiRuntimeInstance value, Assets::AssetId asset, Assets::AssetRegistryRevision revision, std::vector<UiReloadCanvas> owners)
            : instance(std::move(value)), rootAsset(asset), registryRevision(revision), canvases(std::move(owners)) {}

        UiRuntimeInstance instance;
        Assets::AssetId rootAsset;
        Assets::AssetRegistryRevision registryRevision;
        std::vector<UiReloadCanvas> canvases;
    };

    struct UiHotReload::Storage final {
        explicit Storage(UiHotReloadLimits bounds) : limits(bounds), retired(bounds.maximumRetiredGenerations + 1) {}

        UiHotReloadLimits limits;
        std::shared_ptr<UiReloadGeneration> current;
        std::vector<std::shared_ptr<UiReloadGeneration>> retired;
        std::uint64_t issuedNamespaceEnd{};
        std::uint32_t preparedCount{};
        bool stopped{};
        bool collecting{};
    };

    struct UiHotReload::Prepared::Storage final {
        Storage() = default;
        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        ~Storage() {
            if (admitted)
                --publisher->preparedCount;
        }

        std::shared_ptr<UiHotReload::Storage> publisher;
        std::shared_ptr<UiReloadGeneration> source;
        std::shared_ptr<UiReloadGeneration> replacement;
        CancellationToken cancellation;
        UiReloadReconciliation reconciliation;
        std::vector<UiReloadDetail::CanvasStamp> sourceStamps;
        bool consumed{};
        bool sceneRebind{};
        bool removingScene{};
        bool admitted{};
    };
}  // namespace Horo::Runtime::Ui
