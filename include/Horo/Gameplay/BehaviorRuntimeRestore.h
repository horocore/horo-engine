#pragma once
/** @file BehaviorRuntimeRestore.h @brief Scene-owned Gameplay candidates and canonical actual behavior lifecycle persistence. */
#include "Horo/Gameplay/BehaviorRestore.h"
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/SaveGameplayPersistence.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"

#include <functional>

namespace Horo::Gameplay {
    /** @brief Actual engine flags; no value is reconstructed from authored defaults. */
    struct BehaviorLifecycleFlags final {
        bool created{}, enabledCallbackActive{}, started{}, enabled{};
        [[nodiscard]] constexpr auto operator<=>(const BehaviorLifecycleFlags &) const noexcept = default;
    };

    /** @brief One durable lifecycle entry; contains no runtime entity handle. */
    struct BehaviorLifecycleEntry final {
        Runtime::PersistentEntityId entity;
        Runtime::PersistentEntityGeneration generation;
        BehaviorInstanceId instance;
        BehaviorTypeId type;
        BehaviorLifecycleFlags flags;
    };

    struct BehaviorLifecycleSnapshot final {
        std::vector<BehaviorLifecycleEntry> instances;
    };

    /** @brief Gameplay schema1 encoded through the existing bounded canonical codec. */
    [[nodiscard]] Result<Runtime::CanonicalEncodedValue> EncodeBehaviorLifecycle(const BehaviorLifecycleSnapshot &,
                                                                                 Runtime::CanonicalCodecLimits limits = {});
    /** @brief Strictly decodes schema1, rejecting duplicates, noncanonical order, unknown/trailing fields and invalid booleans. */
    [[nodiscard]] Result<BehaviorLifecycleSnapshot> DecodeBehaviorLifecycle(std::span<const std::byte>,
                                                                            Runtime::CanonicalCodecLimits limits = {});

    class BehaviorRuntimeSceneOwner;
    class BehaviorLifecyclePersistenceAdapter;

    /** @brief One actual inactive runtime, owned and retired with its Scene activation candidate. */
    class BehaviorRuntimeRestoreCandidate final : public std::enable_shared_from_this<BehaviorRuntimeRestoreCandidate> {
    public:
        struct State; /**< Opaque engine-owned candidate storage. */
        ~BehaviorRuntimeRestoreCandidate();
        /** @brief Issues a revocable target only for actual instances of the requested registered owner type. */
        [[nodiscard]] Result<BehaviorPersistenceTarget> Binding(const BehaviorTypeId &type,
                                                                const Runtime::PersistentEntityIdentityMap &identities);
        /** @brief Matches saved stable identities against the actual restored Scene, without old live inventory restrictions. */
        [[nodiscard]] Result<void> StageLifecycle(const BehaviorLifecycleSnapshot &, const Runtime::PersistentEntityIdentityMap &);
        /** @brief Pure final validation; no callbacks, providers, allocation or mutation. */
        [[nodiscard]] Result<void> ValidatePublication() const;
        /** @brief Borrows the actual constructed runtime; the caller must obey its inactive/owner-lane admission. */
        [[nodiscard]] BehaviorRuntime *Runner() const noexcept;

    private:
        friend class BehaviorRuntimeSceneOwner;
        friend class BehaviorLifecyclePersistenceAdapter;
        friend class BehaviorRuntimeRestoreAccess;
        explicit BehaviorRuntimeRestoreCandidate(std::shared_ptr<State>) noexcept;
        void PublishLifecycle() noexcept;
        void Shutdown() noexcept;
        std::shared_ptr<State> state_;
    };

    /** @brief Application-owned binding; the Scene activation candidate retains actual runtime ownership. */
    class BehaviorRuntimeSceneOwner final : public std::enable_shared_from_this<BehaviorRuntimeSceneOwner> {
    public:
        struct State; /**< Opaque owner-lane binding storage. */
        /** @brief Captures actual Scene service/registry dependencies; both must outlive this owner and its candidates. */
        [[nodiscard]] static Result<std::shared_ptr<BehaviorRuntimeSceneOwner>> Create(Runtime::RuntimeSceneService &,
                                                                                       const BehaviorRegistry &,
                                                                                       BehaviorRuntimeLimits limits = {});
        /** @brief Registers ordinary and durable candidates through the existing Scene owner, never a second transaction. */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeActivationParticipant();
        /** @brief Called after the actual SceneRestoreBundle::PrepareScene; constructs that exact inactive Scene; its DerivedResources
         * participant later runs prehooks after initial placement.
         * @param scene Actual unpublished Scene, not a copied/authoring view. @param services Real prepared scopes or explicitly absent
         * scopes.
         * @return Original preparation failure with complete unpublished rollback.
         */
        [[nodiscard]] Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>> PrepareRestoreCandidate(
            Runtime::RuntimeScene &scene, BehaviorRuntimeServiceBindings services = {});
        /** @brief Revokes only the exact unpublished candidate during the existing Scene restore rollback. */
        void CancelPrepared(Runtime::SceneRuntimeId) noexcept;
        /** @brief Finds only the exact pending candidate; never falls back to the active runtime. */
        [[nodiscard]] std::shared_ptr<BehaviorRuntimeRestoreCandidate> Prepared(Runtime::SceneRuntimeId) const noexcept;
        /** @brief Borrows the published runtime on its owner lane; null before publication or after retirement. */
        [[nodiscard]] BehaviorRuntime *Active() const noexcept;
        /** @brief Issues a checked target for binding the original installed capture source to the actual published runtime. */
        [[nodiscard]] Result<BehaviorPersistenceTarget> BindingForActive(const BehaviorTypeId &type,
                                                                         const Runtime::PersistentEntityIdentityMap &identities);
        /** @brief Performs EXISTING ordinary Play initialization after initial Scene publication, when native services are published.
         * @details Forbidden for durable candidates; all their prehooks, materialization, fixups and flags are ready before the gate.
         */
        [[nodiscard]] Result<void> ActivatePublished(BehaviorRuntimeServiceBindings services = {},
                                                     std::shared_ptr<const GameplayPrefabContext> prefabs = {});
        /** @brief Closes the existing published runner before the existing module reload transaction unloads its generation.
         * @pre CaptureReloadSnapshot succeeded, SaveSession/source leases are settled, and no restore candidate is pending. */
        [[nodiscard]] Result<void> QuiesceForReload();
        /** @brief Replaces only the quiesced runner in the same Scene-owned slot after the existing module reload transaction.
         * @return Original construction/ordinary activation/reload-state error; failure leaves the slot quiesced. */
        [[nodiscard]] Result<void> ResumeAfterReload(const BehaviorRegistry &, const BehaviorRuntimeReloadSnapshot &,
                                                     BehaviorRuntimeServiceBindings services = {},
                                                     std::shared_ptr<const GameplayPrefabContext> prefabs = {});
        /** @brief Checks only known requirements on actual active factory objects for the explicit no-candidate-services profile.
         * @return Success when each actual behavior exposes the inactive prehook and declares no required candidate services;
         * typed failure for stale/foreign admission, missing hooks, unknown flags or any non-None requirement.
         * @details Owner-lane read-only admission before durable capture/writing. Never invokes PrepareInactive, restore or live
         * mutation. Success does NOT guarantee interfaces on future prepared persistence results or a future changed inventory;
         * actual source binding and pre-publication materialization/retirement validation remain mandatory.
         */
        [[nodiscard]] Result<void> ValidateKnownNoServiceRestoreRequirements() const;
        /** @brief Captures actual published lifecycle values through the existing immutable Scene identity map. */
        [[nodiscard]] Result<BehaviorLifecycleSnapshot> CaptureLifecycle(const Runtime::PersistentEntityIdentityMap &) const;

    private:
        friend class BehaviorRuntimeRestoreAccess;
        explicit BehaviorRuntimeSceneOwner(std::shared_ptr<State>) noexcept;
        std::shared_ptr<State> state_;
    };

    /** @brief Existing canonical participant/record descriptors owned by Gameplay (schema1). */
    [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor BehaviorLifecycleParticipant();
    [[nodiscard]] Runtime::SaveRecordId BehaviorLifecycleRecord();

    /** @brief Existing-container canonical adapter; map provider supplies the host's actual immutable identity mapping. */
    class BehaviorLifecyclePersistenceAdapter final : public Runtime::ICanonicalStateAdapter {
    public:
        using IdentityProvider = std::function<Result<Runtime::PersistentEntityIdentityMap>()>;
        [[nodiscard]] static Result<std::shared_ptr<BehaviorLifecyclePersistenceAdapter>> Create(std::shared_ptr<BehaviorRuntimeSceneOwner>,
                                                                                                 IdentityProvider);
        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &,
                                                                           Runtime::ICanonicalCaptureSink &) const override;
        /** @brief Creates the existing transaction receipt; maps are consumed during validation and never copied into a second authority.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::IStagedRestoreParticipant>> StageRestore(
            Runtime::ParticipantSchemaVersion, Runtime::SaveRecordId, std::span<const std::byte>,
            std::shared_ptr<BehaviorRuntimeRestoreCandidate>, const Runtime::PersistentEntityIdentityMap &) const;

    private:
        BehaviorLifecyclePersistenceAdapter(std::shared_ptr<BehaviorRuntimeSceneOwner>, IdentityProvider);
        std::shared_ptr<BehaviorRuntimeSceneOwner> owner_;
        IdentityProvider identities_;
    };
}  // namespace Horo::Gameplay
