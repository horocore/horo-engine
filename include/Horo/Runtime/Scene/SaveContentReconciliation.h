#pragma once

/** @file SaveContentReconciliation.h
 * @brief Installed-content ownership and pre-world reconciliation of validated saves.
 */
#include "Horo/Gameplay/PersistenceInstallation.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"
#include "Horo/Runtime/Scene/SaveContentRequirements.h"

namespace Horo::Runtime {
    namespace SaveContentDetail {
        struct InstalledState;
        struct ReconciliationState;
    }  // namespace SaveContentDetail
    /** @brief Explicit trusted host policy for a save predating the built-in declaration. */
    enum class SaveLegacyContentPolicy : std::uint8_t {
        Reject,
        ValidateBaselineOnly
    };
    /** @brief Project-approved behavior when declared optional content is unavailable. */
    enum class SaveOptionalContentAbsence : std::uint8_t {
        Reject,
        Preserve,
        Quarantine
    };

    /** @brief Explicit optional semantic-owner disposition, independent of absence itself. */
    struct SaveOptionalContentPolicy final {
        SaveParticipantId owner;
        SaveOptionalContentAbsence absence{SaveOptionalContentAbsence::Reject};
    };

    /** @brief Explicit same-type compatible cooked replacement; logical asset identity remains unchanged.
     * @details Re-save retains the original logical declaration and expected source digest. This policy resolves the
     *          physical replacement envelope again on every admission; it does not migrate persistent asset or base-scene IDs.
     */
    struct SaveAssetContentSubstitution final {
        Assets::AssetId original;
        Assets::AssetId replacement;
        Assets::AssetTypeId type;
        Sha256Digest replacementEnvelopeDigest;
    };

    /** @brief Trusted finite project/release reconciliation policy. */
    struct SaveContentProjectPolicy final {
        SaveCompatibilityPolicy compatibility;
        SaveLegacyContentPolicy legacy{SaveLegacyContentPolicy::Reject};
        std::vector<SaveOptionalContentPolicy> optionalOwners;
        std::vector<SaveAssetContentSubstitution> substitutions;
        std::uint64_t maximumPreservedBytes{64ULL << 20U};
        std::uint64_t maximumContentReadBytes{64ULL << 20U};
    };
    /** @brief Successful content states; unavailable required content always returns a failure instead. */
    enum class SaveContentDisposition : std::uint8_t {
        Required,
        Optional,
        Substituted,
        Quarantined,
        Preservable
    };

    /** @brief Typed host presentation guidance; no action is performed implicitly. */
    enum class SaveContentRemedy : std::uint8_t {
        None,
        InstallCompatibleContent,
        ReviewApprovedSubstitution,
        PreserveOpaqueData
    };

    /** @brief Value-only actionable decision retaining the exact declared semantic identity. */
    struct SaveContentDiagnostic final {
        SaveContentRequirement requirement;
        SaveContentDisposition disposition;
        std::optional<Assets::AssetId> replacement;
        SaveContentRemedy remedy{SaveContentRemedy::None};
    };
    class ReconciledSaveContent;
    class PreparedSavedSceneBootstrap;
    class ISavedSceneBaselineDecoder;
    struct SavedSceneBootstrapDescriptor;

    /** @brief Owner-thread installation authority; replacing/closing it revokes prior reconciliation admission.
     * @details The owner serializes mutation and admission with module reload/shutdown. Accepted immutable bytes and module
     *          pins may retire later; observation is not an atomic cross-thread callback lease.
     */
    class InstalledSaveContent final {
        /** @brief Factory-only nonaggregate key; callers cannot fabricate installation authority. */
        class ValidatedConstruction final {
            friend class InstalledSaveContent;
            ValidatedConstruction() = default;

        public:
            ValidatedConstruction(const ValidatedConstruction &) = default;
        };

    public:
        /** @brief Installs already validated ownership; only Create can construct the key.
         * @param key Factory validation evidence.
         * @param state Private installed-generation owner state.
         */
        InstalledSaveContent(ValidatedConstruction key, std::shared_ptr<SaveContentDetail::InstalledState> state) noexcept;
        InstalledSaveContent(const InstalledSaveContent &) = delete;
        InstalledSaveContent &operator=(const InstalledSaveContent &) = delete;
        ~InstalledSaveContent();
        /** @brief Creates an installation from actual archive verification and native host receipts.
         * @param provider Immutable verified mounted archive provider.
         * @param modules Actual privately issued persistence registrations for this installation.
         * @return Owner, or typed invalid/conflicting/revoked evidence failure; no project source callback runs.
         */
        [[nodiscard]] static Result<std::unique_ptr<InstalledSaveContent>> Create(
            std::shared_ptr<const Assets::AssetArchiveProvider> provider, std::vector<GameplayPersistenceInstallation> modules);
        /** @brief Replaces installation at the owner quiescent boundary; failure retains the previous authority.
         * @param provider New verified archive provider.
         * @param modules New exact-generation native receipts.
         * @return Success or typed identity/capacity/revocation error.
         */
        [[nodiscard]] Result<void> Replace(std::shared_ptr<const Assets::AssetArchiveProvider> provider,
                                           std::vector<GameplayPersistenceInstallation> modules);
        /** @brief Closes future admission on the owner thread; existing native/storage pins retire with accepted operations.
         * @pre Called on the acquiring runtime owner thread, serialized with module reload/shutdown and content replacement.
         */
        void Close() noexcept;

    private:
        friend class ReconciledSaveContent;
        std::shared_ptr<SaveContentDetail::InstalledState> state_;
    };

    /** @brief Unforgeable owned source/install proof checked before any saved-scene preparation callback. */
    class ReconciledSaveContent final {
    public:
        ReconciledSaveContent(const ReconciledSaveContent &) = delete;
        ReconciledSaveContent &operator=(const ReconciledSaveContent &) = delete;
        ReconciledSaveContent(ReconciledSaveContent &&) noexcept = default;
        ReconciledSaveContent &operator=(ReconciledSaveContent &&) noexcept = default;
        /** @brief Reads declared requirements and resolves actual content before world construction.
         * @param installed Runtime-owned installed-generation authority.
         * @param archive Immutable owned archive; borrowed reader sources are never retained.
         * @param policy Explicit trusted release/project policy.
         * @param cancellation Operation cancellation checked throughout bounded content reads.
         * @return Owned proof or contextual required-content/schema/policy/cancellation failure; no callback has run.
         */
        [[nodiscard]] static Result<ReconciledSaveContent> Prepare(InstalledSaveContent &installed, ImmutableSaveArchive archive,
                                                                   SaveContentProjectPolicy policy, CancellationToken cancellation);
        /** @brief Revalidates installation/module admission on the runtime owner thread.
         * @return Success, or typed moved/stale/revoked/cancelled error. Does not consume this proof.
         */
        [[nodiscard]] Result<void> ValidateAdmission() const;
        /** @brief Returns immutable preflight decisions. @return Owned-lifetime diagnostic view; empty after move. */
        [[nodiscard]] std::span<const SaveContentDiagnostic> Diagnostics() const noexcept;
        /** @brief Returns whether optional absence/substitution changed this world’s content composition. @return Degraded status. */
        [[nodiscard]] bool IsDegraded() const noexcept;

    private:
        friend class PreparedSavedSceneBootstrap;
        friend class QueuedSavedSceneBootstrap;
        friend Result<PreparedSavedSceneBootstrap> PrepareSavedSceneBootstrap(SavedSceneBootstrapDescriptor, const Assets::AssetTypeId &,
                                                                              ReconciledSaveContent, ISavedSceneBaselineDecoder *);
        friend class SaveContentWorld;
        explicit ReconciledSaveContent(std::shared_ptr<SaveContentDetail::ReconciliationState> state) noexcept;
        std::shared_ptr<SaveContentDetail::ReconciliationState> state_;
    };
}  // namespace Horo::Runtime
