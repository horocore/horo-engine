#pragma once
/** @file ProjectAssetImportCommitter.h @brief Host-composed durable initial asset import publication. */
#include "Horo/Assets/AssetImportOperation.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectMutation.h"

namespace Horo::Application {
    /** @brief Observable outcome independent from a typed operation failure. */
    enum class ProjectAssetImportStatus : std::uint8_t {
        NotPublished,
        Published,
        RolledBack,
        ReconciliationRequired
    };

    /** @brief Bounded scalar native publication evidence; no retained document, service or filesystem handle. */
    struct ProjectAssetImportReceipt final {
        ProjectAssetImportStatus status{ProjectAssetImportStatus::NotPublished};
        std::size_t promotedFiles{}, rolledBackFiles{};
        bool registryPublished{};
    };

    /** @brief Single owner-thread initial-import committer using explicit shared project and native filesystem authorities.
     * @details At most 64 items/64 MiB are prepared, then every payload and requested sidecar is staged and validated before promotion.
     * Terrain requires its identity/settings sidecar; other importer metadata-optional flags retain payload-only behavior.
     * Existing destinations are rejected. The registry is published only after the complete durable batch; prepublication failures
     * compensate only proven owned new files while writer leases remain held. Reentrant calls are rejected.
     * This is consumer-atomic registry visibility, not a cross-crash filesystem pair transaction: a crash can leave an incomplete
     * pair requiring reconciliation. Tracked consumers require both payload and matching valid identity sidecar; explicitly
     * metadata-free generic payloads remain untracked and are excluded by ordinary registry rebuilding.
     * Cooperating writers must share the project coordinator and per-source leases; native rename is not a content CAS against
     * noncooperating mutation. Storage/provider callback exceptions are contained as typed failures.
     */
    class ProjectAssetImportCommitter final : public Assets::IAssetImportCommitter {
    public:
        /** @brief Binds borrowed host authorities; all must outlive this committer and each synchronous Commit.
         * @param mutations Shared project writer admission. @param files Host durable filesystem authority.
         * @param registry Optional registry to rebuild only after all pairs are durable.
         */
        ProjectAssetImportCommitter(Editor::ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                    Assets::AssetRegistry *registry = nullptr) noexcept;
        /** @brief Commits one complete initial import without overwriting existing source generations.
         * @param batch Detached prepared items and authorized destination. @param ids Host identity generator.
         * @param cancellation Checked before staging and each native publication.
         * @return Success after complete durable publication, or typed failure; inspect LastReceipt for irreversible native evidence.
         */
        [[nodiscard]] Result<void> Commit(const Assets::PreparedAssetImportBatch &batch, Assets::IAssetIdGenerator &ids,
                                          const CancellationToken &cancellation) override;

        /** @brief Returns the latest owner-thread scalar publication evidence, including failure compensation uncertainty.
         * @return Borrow valid until the next Commit or destruction.
         */
        [[nodiscard]] const ProjectAssetImportReceipt &LastReceipt() const noexcept {
            return receipt_;
        }

    private:
        Editor::ProjectMutationCoordinator &mutations_;
        DurableFileSystem &files_;
        Assets::AssetRegistry *registry_{};
        ProjectAssetImportReceipt receipt_;
        bool busy_{};
    };
}  // namespace Horo::Application
