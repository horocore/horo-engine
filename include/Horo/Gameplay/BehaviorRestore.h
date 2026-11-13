#pragma once
/** @file BehaviorRestore.h @brief Optional exact-module inactive restore contracts; no engine Scene ownership enters the SDK. */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/Behavior.h"
#include "Horo/Gameplay/Component.h"
#include "Horo/Gameplay/GameAsset.h"
#include "Horo/Gameplay/GameplayRegistration.h"
#include "Horo/Gameplay/PersistenceSource.h"
#include "Horo/Runtime/Save/SaveIdentity.h"
#include "Horo/Runtime/Save/SaveRestoreReferenceContext.h"

#include <atomic>

namespace Horo::Runtime {
    class GameplayPersistenceAdapter;
}

namespace Horo::Gameplay {
    class LoadedGameModule;
    class BehaviorRuntimeRestoreCandidate;

    /** @brief Borrowed read-only projection of one actual inactive factory instance, valid only during its owner-lane call. */
    struct BehaviorRestoreInstance final {
        GameplayEntityRef entity;
        BehaviorInstanceId instance;
        BehaviorTypeId type;
        std::span<const BehaviorField> fields;
        Math::Transform localTransform;
        Runtime::PersistentEntityId
            persistentEntity;                 /**< Supplied by checked persistence target visits; unavailable in transient prehooks. */
        std::uint64_t persistentGeneration{}; /**< Exact persistent incarnation from that same admitted map, zero in transient prehooks. */
        IBehaviorInstance *implementation{};  /**< Actual factory object; never retain this pointer or the borrowed fields. */
    };

    enum class BehaviorRestoreServices : std::uint32_t {
        None = 0,
        Physics = 1,
        Navigation = 2,
        Character = 4,
        RuntimeUi = 8
    };

    /** @brief Restore-only initialization of transient state before durable bytes are materialized; owns no Scene/service mutation grant.
     */
    class IBehaviorRestorePreparation {
    public:
        virtual ~IBehaviorRestorePreparation() = default;
        [[nodiscard]] virtual BehaviorRestoreServices RequiredRestoreServices() const noexcept = 0;
        /** @brief Initializes inactive transient fields. @param instance Actual bounded Scene projection, borrowed only for this call.
         * @return Success or original failure; candidate factory destruction rolls back all unpublished state.
         * @details Ordinary OnCreate/OnEnable are not called on this path. Do not mutate live module/global/service state.
         */
        [[nodiscard]] virtual Result<void> PrepareInactive(const BehaviorRestoreInstance &instance) = 0;
    };

    /** @brief Owner-lane visitor; borrowed instances cannot escape the call. */
    class IBehaviorPersistenceVisitor {
    public:
        virtual ~IBehaviorPersistenceVisitor() = default;
        /** @brief Uses actual candidate instances of the registered owner type. @param instances Exact bounded borrowed objects.
         * @return Original preparation result. */
        [[nodiscard]] virtual Result<void> Visit(std::span<const BehaviorRestoreInstance> instances) = 0;
    };

    namespace Detail {
        struct BehaviorPersistenceHostAccess;

        /** @brief GameplayRuntime implementation seam for revocable owner-lane visits; never a module-selected Scene backend. */
        class IBehaviorPersistenceTargetAccess {
        public:
            virtual ~IBehaviorPersistenceTargetAccess() = default;
            [[nodiscard]] virtual bool CanUse() const noexcept = 0;
            [[nodiscard]] virtual Result<void> Visit(IBehaviorPersistenceVisitor &) const = 0;
            [[nodiscard]] virtual bool InactiveRestore() const noexcept = 0;
            [[nodiscard]] virtual bool SameOwner(const IBehaviorPersistenceTargetAccess &) const noexcept = 0;
            virtual void MarkMaterialized() const noexcept = 0;
            virtual void MarkFixed() const noexcept = 0;
        };
    }  // namespace Detail

    /** @brief Unforgeable, revocable exact-module candidate binding. Native calls require their installation/runtime code pin; the target
     * only borrows revocable admission. */
    class BehaviorPersistenceTarget final {
    public:
        BehaviorPersistenceTarget() = default;

        /** @brief Checks candidate/owner-lane admission without granting a live Scene mutation capability. */
        [[nodiscard]] bool CanUse() const noexcept {
            return access_ && access_->CanUse() && (!requiresGeneration_ || !generation_.expired()) && ModuleAdmitted() &&
                   !cancellation_.IsCancellationRequested();
        }

        /** @brief Visits the actual typed owner objects, rejecting revoked candidates. @param visitor Immediate borrowed callback.
         * @return Original callback result or typed stale/thread failure. */
        [[nodiscard]] Result<void> Visit(IBehaviorPersistenceVisitor &visitor) const {
            if (!CanUse())
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreActivationStale));
            return access_->Visit(visitor);
        }

        [[nodiscard]] bool InactiveRestore() const noexcept {
            return CanUse() && access_->InactiveRestore();
        }

        [[nodiscard]] bool SameOwner(const BehaviorPersistenceTarget &other) const noexcept {
            return CanUse() && other.CanUse() && requiresGeneration_ == other.requiresGeneration_ &&
                   generation_.lock().get() == other.generation_.lock().get() && access_->SameOwner(*other.access_);
        }

        /** @brief Reads the registered behavior owner type; requires an unmoved valid binding. */
        [[nodiscard]] const BehaviorTypeId &Type() const noexcept {
            return type_;
        }

    private:
        friend class BehaviorRuntimeRestoreCandidate;
        friend class LoadedGameModule;
        friend class Runtime::GameplayPersistenceAdapter;
        friend struct Detail::BehaviorPersistenceHostAccess;

        BehaviorPersistenceTarget(std::shared_ptr<const Detail::IBehaviorPersistenceTargetAccess> access, std::shared_ptr<void> generation,
                                  BehaviorTypeId type) noexcept
            : access_(std::move(access)), generation_(generation), requiresGeneration_(bool(generation)), type_(std::move(type)) {}

        bool ModuleAdmitted() const noexcept {
            const auto admitted = admission_.lock();
            return !moduleBound_ || (admitted && admitted->load(std::memory_order_acquire));
        }

        void MarkMaterialized() const noexcept {
            access_->MarkMaterialized();
        }

        void MarkFixed() const noexcept {
            access_->MarkFixed();
        }

        std::weak_ptr<const std::atomic_bool> admission_;
        bool moduleBound_{};
        CancellationToken cancellation_;
        std::shared_ptr<const Detail::IBehaviorPersistenceTargetAccess> access_;
        std::weak_ptr<void> generation_;
        bool requiresGeneration_{};
        BehaviorTypeId type_;
    };

    /** @brief Optional real registered source capability; legacy sources remain valid for their existing non-behavior path. */
    class IBehaviorCandidatePersistenceSource {
    public:
        virtual ~IBehaviorCandidatePersistenceSource() = default;
        /** @brief Binds an inactive source to the exact privately issued target; never substitutes live objects.
         * @param target Checked scoped handle; retain the handle, never pointers from its visitor.
         * @return Real module-owned source or typed unsupported/preparation failure. */
        [[nodiscard]] virtual Result<std::shared_ptr<IPersistenceSource>> BindRestoreCandidate(BehaviorPersistenceTarget target) = 0;
    };

    /** @brief Required capability on the actual prepared result for behavior candidate restore. */
    class IInactiveBehaviorPersistenceState {
    public:
        virtual ~IInactiveBehaviorPersistenceState() = default;
        /** @brief Applies prepared bytes to unpublished objects BEFORE reference fixup and final publication.
         * @param target The exact binding used by the registered source.
         * @return Success only after actual candidate fields are visible to FixupRuntimeReferences; no live mutation.
         * @details The accompanying IPreparedPersistenceState::Publish may transfer visibility/ownership only; it must not defer
         * candidate state materialization or rerun initializer callbacks. Legacy Publish-only states cannot satisfy this capability.
         */
        [[nodiscard]] virtual Result<void> MaterializeInactive(const BehaviorPersistenceTarget &target) = 0;
    };

    struct BehaviorEntityRestoreTarget final {
        Runtime::SaveEntityReference entity;
        std::uint64_t generation{};
    };

    struct BehaviorComponentRestoreTarget final {
        BehaviorEntityRestoreTarget entity;
        ComponentTypeId type;
    };

    struct BehaviorAssetRestoreTarget final {
        Runtime::SaveAssetId identity;
        GameAssetTypeId type;
    };

    struct BehaviorServiceRestoreTarget final {
        GameplayServiceId service;
        std::uint64_t generation{};
    };

    using BehaviorRestoreReferenceTarget =
        std::variant<BehaviorEntityRestoreTarget, BehaviorComponentRestoreTarget, BehaviorAssetRestoreTarget, BehaviorServiceRestoreTarget,
                     Runtime::SaveRestoreParticipantTarget>;

    /** @brief Complete durable source attachment; instance identity is only unique within its actual persistent entity. */
    struct BehaviorRestoreAttachment final {
        Runtime::PersistentEntityId entity;
        std::uint64_t generation{};
        BehaviorInstanceId instance;
        [[nodiscard]] auto operator<=>(const BehaviorRestoreAttachment &) const noexcept = default;
    };

    /** @brief Actual schema-owned immutable reference input; no RuntimeScene type, runtime pointer or native authority. */
    struct BehaviorRestoreReference final {
        Runtime::SaveRestoreReferenceId identity;
        Runtime::SaveParticipantId owner;
        BehaviorRestoreAttachment source;
        BehaviorRestoreReferenceTarget target;
        bool allocationPrerequisite{}, required{true};
    };

    /** @brief Optional inventory on the ACTUAL prepared native result. Storage remains immutable and owned through fixup. */
    class IBehaviorPersistenceReferenceInventory {
    public:
        virtual ~IBehaviorPersistenceReferenceInventory() = default;
        [[nodiscard]] virtual std::span<const BehaviorRestoreReference> RestoreReferences() const noexcept = 0;
    };

    /** @brief Optional actual-native proof that published materialized state belongs to the actual runtime semantic owner.
     * @details Materialize must transfer/copy every value into candidate-owned storage; no active value may borrow this receipt.
     * The host calls this outside publication after old Scene/runtime retirement. Failure retains receipt/source/code pins for retry.
     */
    class IPublishedGameplayPreparationRetirement {
    public:
        virtual ~IPublishedGameplayPreparationRetirement() = default;
        [[nodiscard]] virtual Result<void> RetirePreparation() = 0;
    };
}  // namespace Horo::Gameplay
