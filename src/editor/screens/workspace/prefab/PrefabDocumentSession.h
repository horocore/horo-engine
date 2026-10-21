#pragma once
/** @file PrefabDocumentSession.h @brief Persistent prefab source authority and disposable runtime stage. */
#include "EditorPlaySceneHost.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Editor/SemanticDocumentHistory.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <thread>
#include <variant>

namespace Horo::Editor {
    /** @brief Exact source command admission; preview generations never authorize edits. */
    struct PrefabDocumentFence final {
        Assets::AssetId owner;
        DocumentInstanceId instance;
        std::uint64_t revision{};
    };

    /** @brief Semantic name edit of one source-owned local object. */
    struct RenamePrefabObject final {
        Prefab::LocalObjectId object;
        std::string name;
    };

    /** @brief Semantic local transform edit; derived scene IDs never enter source history. */
    struct TransformPrefabObject final {
        Prefab::LocalObjectId object;
        Math::Transform transform;
    };

    using PrefabSourceEdit = std::variant<RenamePrefabObject, TransformPrefabObject>;

    /** @brief Immutable source/session projection; dirty/saved/recovery and runtime state are independent. */
    struct PrefabDocumentSnapshot final {
        DocumentIdentity identity;
        PrefabDocumentFence fence;
        std::shared_ptr<const Prefab::PrefabDocument> source;
        Prefab::PrefabSourceRevision sourceRevision;
        Assets::AssetSourceIdentity acceptedSource;
        Sha256Digest savedState;
        std::uint64_t recoveryRevision{};
        std::uint64_t previewRevision{};
        bool dirty{};
        bool externalConflict{};
        bool canUndo{};
        bool canRedo{};
        bool stageActive{};
    };

    /** @brief Owner-thread session. Source edits use exact semantic history; the stage owns no save authority.
     * @details Load/command time only. Caller serializes requests on the creating thread. The shared
     * project lease and canonical source publisher protect persistence. Destruction only releases preview
     * and memory; explicit recovery/close decisions happen before destruction. No gameplay callbacks run.
     */
    class PrefabDocumentSession final {
    public:
        /** @brief Cancels pending preparation and retires all isolated native resources. */
        ~PrefabDocumentSession();
        /** @brief Opens a validated canonical source without mutation.
         * @param root Canonical project root. @param path Canonical contained source path.
         * @param asset Registry-authoritative identity. @param identity Shared document registry identity.
         * @param owner Never-reused workspace incarnation. @param limits Captured prefab policy.
         * @return Session or typed path/parser/identity error.
         */
        [[nodiscard]] static Result<std::unique_ptr<PrefabDocumentSession>> Open(const std::filesystem::path &root,
                                                                                 const std::filesystem::path &path, Assets::AssetId asset,
                                                                                 DocumentIdentity identity, Assets::AssetId owner,
                                                                                 Prefab::PrefabLimitProfile limits);
        /** @brief Captures immutable presentation state. @return Owned source lease and distinct revisions. */
        [[nodiscard]] PrefabDocumentSnapshot Snapshot() const;

        /** @brief Reads session-local instance identity without copying a source path. @return Shared registry-issued identity. */
        [[nodiscard]] DocumentInstanceId Instance() const noexcept {
            return identity_.instance;
        }

        /** @brief Applies one semantic transaction. @param fence Exact source admission. @param edit Source-owned edit.
         * @return Success or zero-mutation validation/history/stale error. */
        [[nodiscard]] Result<void> Execute(PrefabDocumentFence fence, const PrefabSourceEdit &edit);
        /** @brief Replays exact semantic values. @param fence Current source admission. @param redo Replay direction.
         * @return Success or zero-mutation rejection. */
        [[nodiscard]] Result<void> Replay(PrefabDocumentFence fence, bool redo);
        /** @brief Saves through the existing project and asset publication authorities.
         * @param fence Current source admission. @param mutations Shared project writer. @param files Native durable service.
         * @return Tracked publication; uncertain durability retains dirty state and raises conflict. */
        [[nodiscard]] Result<Assets::AssetSourcePublication> Save(PrefabDocumentFence fence, const ProjectMutationCoordinator &mutations,
                                                                  DurableFileSystem &files);
        /** @brief Observes external bytes without changing source/history. @return Capture result. */
        [[nodiscard]] Result<void> InspectExternal();
        /** @brief Replaces accepted source only after explicit dirty-state decision and complete validation.
         * @param fence Exact admission. @param discardDirty Explicit discard decision. @return Success or retained session. */
        [[nodiscard]] Result<void> Reload(PrefabDocumentFence fence, bool discardDirty);
        /** @brief Writes dirty source to shared recovery storage, never canonical source.
         * @param mutations Shared writer. @param files Native durable service. @return Durable recovery or retained prior record. */
        [[nodiscard]] Result<void> Autosave(const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Explicitly accepts an exact shared recovery candidate only against its unchanged source base.
         * @param fence Exact admission. @param record User-inspected candidate. @param mutations Shared writer.
         * @param files Native durable service. @return Success or zero-mutation conflict/corruption error. */
        [[nodiscard]] Result<void> Recover(PrefabDocumentFence fence, const DocumentRecoveryRecord &record,
                                           const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Discards only this session's recovery record or its explicitly accepted exact candidate.
         * @param mutations Shared writer. @param files Durable service. @return Success or retained candidate on failure.
         */
        [[nodiscard]] Result<void> DiscardOwnedRecovery(const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Discards an exact inspected candidate after an explicit user decision, including a foreign/stale session.
         * @param fence Current source admission. @param record Exact displayed candidate, never a guessed record path.
         * @param mutations Shared writer. @param files Durable service.
         * @return Success or stale/authority failure preserving newer records; source/history/dirty state never changes.
         */
        [[nodiscard]] Result<void> DiscardRecoveryCandidate(PrefabDocumentFence fence, const DocumentRecoveryRecord &record,
                                                            const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
        /** @brief Resolves current in-memory source, projects typed components and creates a real isolated runtime scene.
         * @param registry Pinned registry. @param dependencies Canonical captured dependency sources, root replaced by this session.
         * @param runtimeId Host-reserved unique runtime incarnation. @param factory Explicit native/assets composition or assetless tests.
         * @return Accepted complete preparation or typed zero-publication failure.
         */
        [[nodiscard]] Result<void> ActivateStage(const Assets::AssetRegistrySnapshot &registry,
                                                 const Prefab::PrefabSourceResolverSnapshot &dependencies,
                                                 Runtime::SceneRuntimeId runtimeId, const EditorPlaySceneFactory &factory = {});
        /** @brief Advances one bounded owner-lane preparation frame without simulation/gameplay callbacks.
         * @return Whether a complete runtime scene is ready, or typed preparation failure.
         */
        [[nodiscard]] Result<bool> AdvanceStage();
        /** @brief Borrows the completely activated stage. @return Current view or no ready stage. */
        [[nodiscard]] std::optional<Runtime::RuntimeSceneView> StageView() const noexcept;

        /** @brief Reports pending/active resource ownership. @return Whether a stage is owned. */
        [[nodiscard]] bool HasStage() const noexcept {
            return preview_.scene || preview_.host;
        }

        /** @brief Releases every preview/runtime resource without changing source, history or saved/recovery state. */
        void ReleaseStage() noexcept;

        /** @brief Borrows active runtime for immediate extraction. @return Stage or null. */
        [[nodiscard]] const Runtime::RuntimeScene *Stage() const noexcept {
            return preview_.scene.get();
        }

        /** @brief Closes after explicit dirty decision. @param fence Exact admission. @param discardDirty User decision.
         * @return Success or retained live session. */
        [[nodiscard]] Result<void> Close(PrefabDocumentFence fence, bool discardDirty);

    private:
        struct Delta final {
            Prefab::LocalObjectId object;
            PrefabSourceEdit before;
            PrefabSourceEdit after;
            std::size_t bytes{};
        };

        PrefabDocumentSession(std::filesystem::path root, std::filesystem::path path, DocumentIdentity identity, Assets::AssetId owner,
                              Prefab::PrefabLimitProfile limits);
        [[nodiscard]] Result<void> Admit(PrefabDocumentFence fence) const;
        [[nodiscard]] Result<std::shared_ptr<const Prefab::PrefabDocument>> Apply(const PrefabSourceEdit &edit) const;
        [[nodiscard]] Result<void> Replace(const Assets::AssetSourceCapture &capture);
        /** @brief Installs only a complete definition, retaining the old stage on preparation rejection. */
        [[nodiscard]] Result<void> InstallStage(Runtime::RuntimeSceneDefinition definition, Runtime::SceneRuntimeId id,
                                                const EditorPlaySceneFactory &factory);
        std::filesystem::path root_, path_;
        DocumentIdentity identity_;
        Assets::AssetId owner_;
        Prefab::PrefabLimitProfile limits_;
        std::thread::id thread_{std::this_thread::get_id()};
        std::shared_ptr<const Prefab::PrefabDocument> source_;
        Assets::AssetSourceIdentity accepted_;
        Sha256Digest state_, saved_;
        SemanticDocumentHistory<Delta> history_;
        std::optional<DocumentRecoveryRecord> acceptedRecovery_;

        /** @brief Owns one preview incarnation while retaining its successful publication counter across retirement. */
        struct PreviewState final {
            std::unique_ptr<Runtime::RuntimeScene> scene;
            std::unique_ptr<EditorPlaySceneHost> host;
            CancellationSource cancellation;
            std::uint64_t frame{};
            std::uint64_t revision{};
            bool published{};
        } preview_;

        std::uint64_t revision_{1}, recoveryRevision_{};
        bool conflict_{}, closed_{}, busy_{};
    };
}  // namespace Horo::Editor
