#pragma once
/** @file UiStyleWorkspace.h
 * @brief Narrow Assets/Runtime UI style preparation; no Editor, GUI, renderer or native backend authority.
 */
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"

#include <thread>

namespace Horo::Application {
    inline constexpr std::uint64_t MaximumUiStyleWorkspaceBytes = 32U * 1024U * 1024U;

    /** @brief One actually registered style namespace available for explicit selection before any active theme. */
    struct UiStyleWorkspaceTheme final {
        Runtime::Ui::RuntimeStyleAssetId id;
        std::string label;
    };

    /** @brief Explicit host authorities retained through one joined tooling operation. */
    struct UiStyleWorkspaceConfig final {
        Assets::AssetCookRequest cook;
        std::vector<Assets::CookerContribution> resourceCookers;
        std::function<Result<std::shared_ptr<const void>>()> acquireSourceLease;
        std::function<Assets::AssetRegistryRevision()> currentAssets;
        std::uint64_t maximumCapturedBytes{MaximumUiStyleWorkspaceBytes};
    };

    /** @brief Immutable complete candidate with real resource/envelope leases; no mutable document references. */
    struct UiStyleWorkspaceCandidate final {
        Assets::AssetId workspace;
        std::uint64_t request{};
        Assets::AssetRegistryRevision assets;
        Assets::AssetCookGeneration generation;
        Runtime::Ui::UiStyleSchema schema;
        std::shared_ptr<const Runtime::Ui::RuntimeStyleRegistry> registry;
        Runtime::Ui::RuntimeStyleAssetId theme;
        std::vector<Assets::AssetLoadResult> loaded;
        std::shared_ptr<const void>
            sourceLease;            /**< Retained through owner-thread publication, then released separately from byte leases. */
        std::size_t cookedAssets{}; /**< Fresh cook count from generic Assets orchestration. */
        std::size_t cacheHits{};    /**< Verified cache hit count from the same operation. */
    };

    /** @brief Build an inert exact-target cooker using one complete validated source/schema closure.
     * @param documents Captured owned namespace set. @param schema One core provider/default/resource policy.
     * @param target Explicit validation target. @param generation Host-issued candidate generation. @return Contribution or original domain
     * admission failure; no ambient registration.
     */
    [[nodiscard]] Result<Assets::CookerContribution> MakeUiStyleCookerContribution(std::vector<Runtime::Ui::UiStyleAssetDocument> documents,
                                                                                   Runtime::Ui::UiStyleSchema schema,
                                                                                   AssetCookTargetId target,
                                                                                   Runtime::Ui::RuntimeStyleGeneration generation);
    /** @brief Enumerate registered style IDs without reading source or choosing a default.
     * @param assets Real immutable project registry. @return Stable ordered labels and exact IDs, or a typed capacity/identity failure.
     */
    [[nodiscard]] Result<std::vector<UiStyleWorkspaceTheme>> UiStyleWorkspaceThemes(const Assets::AssetRegistrySnapshot &assets);
    /** @brief Execute a bounded, joined tooling preparation using generic capture/cook/cache/manifest/provider services.
     * @param jobs Process scheduler. @param config Explicit host storage/publication/read authorities.
     * @param assets Exact original registry. @param workspace Never-reused workspace incarnation.
     * @param request Never-reused request. @param theme Explicit selected real root, owning the candidate's single default policy.
     * @param generation Exact new style generation. @param cancellation Cooperative operation ancestry.
     * @return Complete unpublished candidate or typed failure; no Editor or source mutation occurs.
     */
    [[nodiscard]] Result<UiStyleWorkspaceCandidate> PrepareUiStyleWorkspace(JobSystem &jobs, const UiStyleWorkspaceConfig &config,
                                                                            Assets::AssetRegistrySnapshot assets, Assets::AssetId workspace,
                                                                            std::uint64_t request, Runtime::Ui::RuntimeStyleAssetId theme,
                                                                            Runtime::Ui::RuntimeStyleGeneration generation,
                                                                            const CancellationToken &cancellation = {});

    /** @brief Owner-thread handoff over one worker-owned preparation; workers never capture this or GUI/document owners.
     * @details The scheduler and injected host services outlive Shutdown. Poll is nonblocking and performs no filesystem work.
     * Teardown cancels/joins before releasing borrows. Failed/stale work never replaces a caller's last-good registry.
     */
    class UiStyleWorkspaceProducer final {
    public:
        /** @brief Capture explicit host authorities without I/O. @param jobs Scheduler. @param config Tooling policy. @param workspace
         * Owner. */
        UiStyleWorkspaceProducer(JobSystem &jobs, UiStyleWorkspaceConfig config, Assets::AssetId workspace);
        /** @brief Cancel/drain fallback; explicit Shutdown is the normal host path. */
        ~UiStyleWorkspaceProducer();
        UiStyleWorkspaceProducer(const UiStyleWorkspaceProducer &) = delete;
        UiStyleWorkspaceProducer &operator=(const UiStyleWorkspaceProducer &) = delete;
        /** @brief Admit one selected theme without blocking the GUI owner.
         * @param assets Actual snapshot. @param theme Explicit namespace. @return Request identity or typed in-progress/lifecycle failure.
         */
        [[nodiscard]] Result<std::uint64_t> Submit(Assets::AssetRegistrySnapshot assets, Runtime::Ui::RuntimeStyleAssetId theme);
        /** @brief Take one terminal result after current-owner and registry fences.
         * @param assets Current real revision. @return Empty while running, complete candidate or original failure; no file reads.
         */
        [[nodiscard]] Result<std::optional<UiStyleWorkspaceCandidate>> TakeCompleted(Assets::AssetRegistryRevision assets);
        /** @brief Explicit bounded tooling/test wait. @param options Host-approved affinity/deadline. @return Join outcome. */
        [[nodiscard]] Result<void> Join(const JoinOptions &options) const;
        /** @brief Close admission and cancel/drain before host borrow destruction. @param options Teardown policy. @return Drain outcome.
         */
        [[nodiscard]] Result<void> Shutdown(const JoinOptions &options);

    private:
        struct Work;
        JobSystem &jobs_;
        UiStyleWorkspaceConfig config_;
        Assets::AssetId workspace_;
        const std::thread::id owner_;
        std::shared_ptr<Work> work_;
        JobHandle handle_;
        std::uint64_t issued_{};
        bool closed_{};
    };
}  // namespace Horo::Application
