#pragma once
/** @file NavigationSettingsWorkflow.h
 * @brief Revision-fenced definition editing and complete-project reference review over canonical document owners.
 */
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/NavigationDefinitionDocument.h"
#include "Horo/Editor/SceneDocumentTransaction.h"

#include <variant>

namespace Horo::Editor {
    /** @brief Typed create/update operations; names are profile presentation, never identity. */
    struct PutNavigationProfile final {
        Navigation::NavigationAgentProfileDescriptor value;
    };

    struct PutNavigationArea final {
        Navigation::NavigationAreaDescriptor value;
    };

    struct PutNavigationFilter final {
        Navigation::NavigationQueryFilterDescriptor value;
    };

    /** @brief Explicit destructive changes; an absent replacement requires zero affected references. */
    struct RemoveNavigationProfile final {
        Navigation::NavigationAgentProfileId id;
        std::optional<Navigation::NavigationAgentProfileId> replacement;
    };

    struct RemoveNavigationArea final {
        Navigation::NavigationAreaId id;
        std::optional<Navigation::NavigationAreaId> replacement;
    };

    struct RemoveNavigationFilter final {
        Navigation::NavigationFilterId id;
        std::optional<Navigation::NavigationFilterId> replacement;
    };

    using NavigationDefinitionEdit = std::variant<PutNavigationProfile, PutNavigationArea, PutNavigationFilter, RemoveNavigationProfile,
                                                  RemoveNavigationArea, RemoveNavigationFilter>;
    /** @brief Exact occurrence vocabulary displayed before destructive commit. */
    enum class NavigationReferenceField : std::uint8_t {
        SurfaceProfile,
        LinkProfile,
        AgentProfile,
        AgentFilter,
        ModifierArea,
        FilterAreaCost
    };

    /** @brief Owned review item; FilterAreaCost uses the filter identity in object, otherwise the Scene object identity. */
    struct NavigationReferenceImpact final {
        Assets::AssetId asset;
        std::uint64_t object{};
        NavigationReferenceField field;
        std::uint64_t identity{};
        std::optional<std::uint64_t> replacement;
    };
    /** @brief Canonical owner types which cannot participate in this staged Scene/definition transaction. */
    enum class NavigationUnsupportedOwnerKind : std::uint8_t {
        PrefabSource,
        ScenePrefabInstances
    };

    /** @brief Exact registry identity requiring another authoritative transaction port; references are unknown, never zero. */
    struct NavigationUnsupportedOwner final {
        Assets::AssetId asset;
        NavigationUnsupportedOwnerKind kind;
    };

    /** @brief Construction-guarded preview; callers can inspect but cannot replace captured evidence or staged changes. */
    class NavigationSettingsReview final {
    public:
        /** @brief Enumerates every affected authored occurrence. @return Borrowed immutable review rows. */
        [[nodiscard]] std::span<const NavigationReferenceImpact> Impacts() const noexcept {
            return impacts_;
        }

        /** @brief Enumerates every known unsupported owner before any destructive commit can be offered.
         * @return Borrowed exact asset identities; nonempty always disables commit.
         */
        [[nodiscard]] std::span<const NavigationUnsupportedOwner> UnsupportedOwners() const noexcept {
            return unsupportedOwners_;
        }

        /** @brief Reports whether every affected occurrence has an explicit surviving replacement. @return Commit eligibility. */
        [[nodiscard]] bool CanCommit() const noexcept {
            return repairable_;
        }

    private:
        friend class NavigationSettingsWorkflow;

        struct SceneEdit final {
            std::weak_ptr<SceneDocumentTransactions::Lifetime> owner;
            SceneTransactionSnapshot snapshot;
            std::vector<SceneObjectReplacement> changes;
        };

        const NavigationSettingsWorkflow *workflow_{};
        Assets::AssetRegistryRevision registryRevision_;
        NavigationDefinitionSnapshot definition_;
        Navigation::NavigationDefinitionInput candidate_;
        std::vector<NavigationDefinitionSnapshot> definitions_;
        std::vector<SceneEdit> scenes_;
        std::vector<NavigationReferenceImpact> impacts_;
        std::vector<NavigationUnsupportedOwner> unsupportedOwners_;
        bool repairable_{true};
    };

    /** @brief Owner-thread application workflow over actual registered project inputs and canonical documents.
     * @details Missing/unloaded registry Scenes/definitions reject inventory capture. A commit stages every history/root
     * before any publish, rechecks every revision, then swaps without callbacks. File saves remain separate per-file
     * atomic publications; this contract does not claim an atomic transaction across filesystem destinations.
     */
    class NavigationSettingsWorkflow final {
    public:
        /** @brief Captures weak owner-qualified handles; the registry must outlive this workflow.
         * @pre Every operation runs on the document owner thread; close/move/reload never occurs concurrently.
         * @param registry Authoritative project Asset Registry. @param definitions Every registered definition opening.
         * @param scenes Every registered Scene opening, including inactive project sources.
         */
        NavigationSettingsWorkflow(const Assets::AssetRegistry &registry, std::span<NavigationDefinitionDocument *const> definitions,
                                   std::span<SceneDocumentTransactions *const> scenes);
        /** @brief Prepares an exhaustive immutable impact review and validated detached successor without mutations.
         * @param definition Exact asset to edit. @param edits Bounded complete typed gesture. @return Review or typed
         * incomplete/stale/invalid failure.
         */
        [[nodiscard]] Result<NavigationSettingsReview> Preview(Assets::AssetId definition,
                                                               std::span<const NavigationDefinitionEdit> edits) const;
        /** @brief Commits only this exact reviewed gesture after all document/registry fences and preparation succeed.
         * @param review Captured preview. @param canEdit Explicit host authority. @param cancellation Cooperative owner-phase cancellation.
         * @return Success or zero-mutation typed failure; a stale confirmation never silently recaptures references.
         */
        [[nodiscard]] Result<void> Commit(const NavigationSettingsReview &review, bool canEdit, const CancellationToken &cancellation);
        /** @brief Replays one whole correlated group, never an independent Scene or definition history entry.
         * @param redo False for undo, true for redo. @param cancellation Owner-phase cancellation.
         * @return Success or zero-mutation stale/closed/preparation failure; all retained owners are weak/session-qualified.
         */
        [[nodiscard]] Result<void> Replay(bool redo, const CancellationToken &cancellation);

    private:
        struct SceneFence final {
            std::weak_ptr<SceneDocumentTransactions::Lifetime> owner;
            SceneTransactionContext context;
        };

        struct Group final {
            std::weak_ptr<NavigationDefinitionDocument::Lifetime> definition;
            std::vector<std::size_t> changedScenes;
        };
        struct ScenePreparation;
        /** @brief Captures actual typed owner inventory without changing any source. */
        [[nodiscard]] Result<void> CaptureInventory(NavigationSettingsReview &review, Assets::AssetId definition, bool destructive) const;
        /** @brief Completes Scene/unsupported-owner inventory against one captured registry. */
        [[nodiscard]] Result<void> CaptureSceneOwners(NavigationSettingsReview &review, const Assets::AssetRegistrySnapshot &registry,
                                                      bool destructive) const;
        /** @brief Rechecks all correlated definition history fences before a grouped replay. */
        [[nodiscard]] Result<void> ValidateReplayDefinitions() const;
        /** @brief Checks every definition and prior history fence before reserving a successor. */
        [[nodiscard]] Result<NavigationDefinitionDocument *> FindDefinition(const NavigationSettingsReview &review) const;
        /** @brief Reserves all Scene roots/history and owner-qualified result fences before publication. */
        [[nodiscard]] Result<std::unique_ptr<ScenePreparation>> PrepareSceneCommit(const NavigationSettingsReview &review, bool canEdit,
                                                                                   const CancellationToken &cancellation,
                                                                                   bool coordinated) const;
        /** @brief Publishes only already fenced, fully reserved roots and history metadata without allocation. */
        void PublishCommit(NavigationDefinitionDocument &definition, NavigationDefinitionDocument::Prepared &prepared,
                           ScenePreparation &scenes, std::vector<NavigationDefinitionSnapshot> &definitions, std::vector<Group> &history,
                           Assets::AssetRegistryRevision registry) noexcept;
        /** @brief Reserves the complete Scene inverse/redo while retaining caller-owned cancellation state. */
        [[nodiscard]] Result<std::unique_ptr<ScenePreparation>> PrepareSceneReplay(const Group &group,
                                                                                   const CancellationToken &cancellation, bool redo) const;
        const Assets::AssetRegistry &registry_;
        std::vector<std::weak_ptr<NavigationDefinitionDocument::Lifetime>> definitions_;
        std::vector<std::weak_ptr<SceneDocumentTransactions::Lifetime>> scenes_;
        std::vector<NavigationDefinitionSnapshot> expectedDefinitions_;
        std::vector<SceneFence> expectedScenes_;
        std::vector<Group> groups_;
        std::size_t cursor_{};
        Assets::AssetRegistryRevision expectedRegistry_;
    };
}  // namespace Horo::Editor
