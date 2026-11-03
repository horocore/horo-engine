#pragma once

/** @file EditorPlaySaveHostStorage.h
 * @brief Worker-only PIE namespace ownership over the existing durable slot lifecycle.
 */
#include "EditorPlaySaveHostArchive.h"
#include "EditorPlaySaveHostCapture.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Runtime/Save/SaveSlotLifecycle.h"
#include "Horo/Security/SecureMemory.h"

namespace Horo::Application::Internal {
    /** @brief Detached actual Scene/schema values pinned by workers; contains no mutable Scene, installation, adapter or native module
     * lease. */
    struct PlaySaveStorageSchema final {
        Runtime::RuntimeSceneDefinition baseline;
        std::vector<Runtime::PersistentEntityRecord> identities;
        std::vector<Gameplay::GameplayPersistenceDeclaration> gameplay;
        std::optional<Runtime::CanonicalStateParticipantDescriptor> lifecycle; /**< Frozen inert Gameplay schema, never a runtime lease. */
        std::optional<Prefab::PrefabLimitProfile> prefabProfile; /**< Inert bounded recipe codec policy; never provider/Scene authority. */
    };

    /** @brief Explicit host-approved sandbox scope, independent of authoring/project and production save directories. */
    struct PlaySaveStorageConfig final {
        Runtime::ProductSaveRoot root;
        Runtime::SaveNamespaceId sandbox;
        Runtime::SaveProjectId project;
        Runtime::SaveWorldId world;
        Runtime::SaveBaseSceneId baseScene;
        std::shared_ptr<Security::SecureRandomSource> entropy;
        std::string engineVersion;
        std::string projectBuildId;
    };

    /** @brief Existing filesystem/slot authority configured for a single unique unsigned development namespace.
     * @details Open, List, NewSlot, Header, Read, Commit and Delete are worker operations. They never borrow runtime state.
     * CloseAdmission is owner-safe and nonblocking; accepted operation leases retain the exact namespace and cannot rebind it.
     * Destruction retires only these owned capabilities and never recursively removes the product root or production slots.
     */
    class PlaySaveStorage final {
        struct State;

    public:
        /** @brief Opens the exact approved scope on a worker, with real semantic verification and finite slot/archive limits. */
        [[nodiscard]] static Result<std::shared_ptr<PlaySaveStorage>> Open(PlaySaveStorageConfig config,
                                                                           std::shared_ptr<const PlaySaveStorageSchema> schema);
        /** @brief Returns the bound immutable namespace projection. */
        [[nodiscard]] const Runtime::SaveNamespaceId &Namespace() const noexcept;
        /** @brief Reads the actual bounded authoritative lifecycle index on a worker. */
        [[nodiscard]] Result<Runtime::SaveSlotIndex> List() const;
        /** @brief Issues a fresh opaque manual slot under the exact current catalog CAS. */
        [[nodiscard]] Result<Runtime::SaveSlotLifecycleTarget> NewSlot();
        /** @brief Issues one genuine generation and complete host metadata without publishing a slot. */
        [[nodiscard]] Result<Runtime::SaveArchiveHeader> Header(const Runtime::SaveSlotLifecycleTarget &target,
                                                                std::uint64_t unixMilliseconds, std::uint64_t playNanoseconds);
        /** @brief Exports and production-reader-admits the exact selected generation; stale projection consent fails. */
        [[nodiscard]] Result<Runtime::ValidatedSaveArchive> Read(const Runtime::SaveManagerCommand &selection,
                                                                 const CancellationToken &cancellation);
        /** @brief Publishes already verified detached archive bytes through the ordinary atomic lifecycle catalog. */
        [[nodiscard]] Result<Runtime::SaveSlotLifecycleResult> Commit(const Runtime::SaveSlotLifecycleTarget &target,
                                                                      const Runtime::FinalizedSaveArchive &archive,
                                                                      Runtime::SaveSlotDisplayMetadata display);
        /** @brief Deletes only an explicitly selected sandbox slot through existing lifecycle consent checks. */
        [[nodiscard]] Result<Runtime::SaveSlotLifecycleResult> Delete(const Runtime::SaveManagerCommand &selection,
                                                                      const CancellationToken &cancellation);
        /** @brief Retires only this activation's selected slot payloads after runtime and worker quiescence.
         * @return Success or original cleanup/reconciliation failure; no recursive filesystem deletion is performed.
         * @details Runs on a worker before CloseAdmission. The caller already closed UI/runtime request admission.
         */
        [[nodiscard]] Result<void> Cleanup();
        /** @brief Revokes new worker binding admission without waiting or invalidating accepted immutable inputs. */
        void CloseAdmission() noexcept;
        /** @brief Retains only the state owned by the validated factory. */
        explicit PlaySaveStorage(std::shared_ptr<State> state) noexcept;

    private:
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
