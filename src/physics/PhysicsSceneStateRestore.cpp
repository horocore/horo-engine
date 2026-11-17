#include "PhysicsSceneStateStorage.h"

#include <algorithm>
#include <format>
#include <new>
#include <stdexcept>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Describe the complete durable witness only on a rejected cold restore. */
        std::string DescribeOwner(const PhysicsStatePersistentOwner &owner) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string identity;
            identity.reserve(32);
            for (const auto byte : owner.entity) {
                identity.push_back(digits[byte >> 4]);
                identity.push_back(digits[byte & 15]);
            }
            return std::format("uuid={} generation={} role={} slot={} object={}", identity, owner.generation,
                               static_cast<unsigned>(owner.role), owner.slot, owner.object);
        }

        /** @brief Retain the first unequal witness and both inventory sizes without weakening admission. */
        std::string DescribeOwnerMismatch(std::span<const PhysicsStatePersistentOwner> saved,
                                          std::span<const PhysicsStatePersistentOwner> receiving) {
            const auto mismatch = std::ranges::mismatch(saved, receiving);
            const auto savedText = mismatch.in1 == saved.end() ? "<end>" : DescribeOwner(*mismatch.in1);
            const auto receivingText = mismatch.in2 == receiving.end() ? "<end>" : DescribeOwner(*mismatch.in2);
            return std::format("Physics persistent owner-key inventory differs: saved={} receiving={} row={}; saved [{}]; receiving [{}].",
                               saved.size(), receiving.size(), mismatch.in1 - saved.begin(), savedText, receivingText);
        }

        /** @brief A completed private World must never publish with stale Scene tables if a later preparation check fails.
         * This closes the SAME existing owner admission; ordinary Scene rollback still owns all native cleanup.
         */
        class RebindAdmission final {
        public:
            explicit RebindAdmission(PhysicsStructuralState &state) noexcept : state_(state) {}

            RebindAdmission(const RebindAdmission &) = delete;
            RebindAdmission &operator=(const RebindAdmission &) = delete;

            ~RebindAdmission() {
                if (!complete_)
                    state_.closed = true;
            }

            void Complete() noexcept {
                complete_ = true;
            }

        private:
            PhysicsStructuralState &state_;
            bool complete_{};
        };
    }  // namespace

    /** @copydoc PreparedPhysicsSceneRestore::PrepareTables */
    Result<std::size_t> PreparedPhysicsSceneRestore::PrepareTables(const PhysicsStructuralState &state, SceneStateStoragePolicy policy) {
        using Outcome = Result<std::size_t>;
        if (state.revision == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        stagingTables_ = std::make_unique<PhysicsSceneStateRebind>();
        stagingTables_->bodies = state.bodies;
        stagingTables_->shapes = state.shapes;
        stagingTables_->constraints = state.constraints;
        stagingTables_->authoredBodies = state.authoredBodies;
        stagingTables_->authoredShapes = state.authoredShapes;
        stagingTables_->authoredConstraints = state.authoredConstraints;
        // The existing structural tables are authoritative after runtime additions/retirement;
        // do not resurrect stale initial-only legacy entries.
        stagingTables_->legacyBodies = state.authoredBodies;
        stagingTables_->legacyShapes = state.authoredShapes;
        stagingTables_->legacyConstraints = state.authoredConstraints;
        return SceneStateRemainingBytes(policy.maximumRetainedBytes,
                                        std::array{sizeof(*this), input_->RetainedPayloadBytes(), projection_->RetainedPayloadBytes(),
                                                   stagingTables_->RetainedPayloadBytes()});
    }

    /** @copydoc PreparedPhysicsSceneRestore::Initialize */
    Result<void> PreparedPhysicsSceneRestore::Initialize(PhysicsStructuralState &state,
                                                         const Runtime::PersistentEntityIdentityMap &identities,
                                                         SceneStateStoragePolicy policy, const Sha256Digest &content,
                                                         std::shared_ptr<const void> contentLease) {
        if (!HasSceneStateContent(content, contentLease))
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        auto projection = PhysicsSceneStateProjection::Build(*state.world, state, identities, policy.projection);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        projection_ = std::make_unique<PhysicsSceneStateProjection>(std::move(projection).Value());
        if (!std::ranges::equal(input_->Construction().owners, projection_->OwnerRecords()))
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported,
                                                   DescribeOwnerMismatch(input_->Construction().owners, projection_->OwnerRecords())));
        const auto fingerprint = projection_->ContentFingerprint(content);
        if (input_->Envelope().compatibility.content != fingerprint)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        auto remaining = PrepareTables(state, policy);
        if (remaining.HasError())
            return Result<void>::Failure(remaining.ErrorValue());
        PhysicsStateCaptureConfiguration configuration{.snapshotSlots = policy.snapshotSlots,
                                                       .maximumRetainedBytes = remaining.Value(),
                                                       .content = fingerprint,
                                                       .contentLease = std::move(contentLease),
                                                       .owners = projection_->OwnerRecords()};
        return Reconstruct(state, std::move(configuration));
    }

    /** @copydoc PreparedPhysicsSceneRestore::Reconstruct */
    Result<void> PreparedPhysicsSceneRestore::Reconstruct(PhysicsStructuralState &state, PhysicsStateCaptureConfiguration configuration) {
        auto bindings = PhysicsWorldStateRestartAccess::Reconstruct(*state.world, *input_, projection_->Routes(), std::move(configuration),
                                                                    cancellation_);
        if (bindings.HasError())
            return Result<void>::Failure(bindings.ErrorValue());
        RebindAdmission admission{state};
        if (auto rebound = stagingTables_->Rebind(*projection_, bindings.Value()); rebound.HasError())
            return rebound;
        if (auto character = target_.RefreshEmptyCharacter(); character.HasError())
            return character;
        auto owner = PhysicsWorldStateRestartAccess::ConfigurationOwner(*state.world);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        auto bytes = PhysicsWorldStateRestartAccess::ConfigurationPayloadBytes(*state.world);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        target_.InstallBindings(state, *stagingTables_);
        stagingTables_.reset();
        configuration_ = std::move(owner).Value();
        configurationBytes_ = bytes.Value();
        publicationRevision_ = state.world->PublishedTick().publicationRevision;
        admission.Complete();
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateTarget::PrepareRestore */
    Result<std::shared_ptr<PreparedPhysicsSceneRestore>> PhysicsSceneStateTarget::PrepareRestore(
        std::unique_ptr<StateDetail::RestartInput> input, const Runtime::PersistentEntityIdentityMap &identities,
        SceneStateStoragePolicy policy, const Sha256Digest &content, std::shared_ptr<const void> contentLease,
        const CancellationToken &cancellation) const {
        using Outcome = Result<std::shared_ptr<PreparedPhysicsSceneRestore>>;
        auto state = CoveredOwner(false);
        if (state.HasError())
            return Outcome::Failure(state.ErrorValue());
        if (auto pending = PreflightPendingContinuation(); pending.HasError())
            return Outcome::Failure(pending.ErrorValue());
        if (!input)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(PhysicsErrors::StateCancelled));
        try {
            auto result = std::shared_ptr<PreparedPhysicsSceneRestore>{new PreparedPhysicsSceneRestore{*this}};
            result->input_ = std::move(input);
            result->cancellation_ = cancellation;
            if (auto prepared = result->Initialize(*state.Value(), identities, policy, content, std::move(contentLease));
                prepared.HasError())
                return Outcome::Failure(prepared.ErrorValue());
            return Outcome::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PreparedPhysicsSceneRestore::ValidateAndArm */
    Result<void> PreparedPhysicsSceneRestore::ValidateAndArm() {
        return CheckPrepared();
    }

    /** @copydoc PreparedPhysicsSceneRestore::CheckPrepared */
    Result<void> PreparedPhysicsSceneRestore::CheckPrepared() const {
        auto state = target_.CoveredOwner(false);
        if (state.HasError())
            return Result<void>::Failure(state.ErrorValue());
        if (cancellation_.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PhysicsErrors::StateCancelled));
        return PhysicsWorldStateRestartAccess::ValidateConfigured(*state.Value()->world, configuration_, publicationRevision_);
    }

    /** @copydoc PreparedPhysicsSceneRestore::RetainedPayloadBytes */
    std::size_t PreparedPhysicsSceneRestore::RetainedPayloadBytes() const noexcept {
        return SceneStatePayloadSum(std::array{sizeof(*this), input_ ? input_->RetainedPayloadBytes() : 0,
                                               projection_ ? projection_->RetainedPayloadBytes() : 0,
                                               stagingTables_ ? stagingTables_->RetainedPayloadBytes() : 0, configurationBytes_});
    }
}  // namespace Horo::Physics::Detail
