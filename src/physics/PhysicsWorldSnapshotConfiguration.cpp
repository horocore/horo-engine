#include "PhysicsStateRestoreControl.h"
#include "PhysicsWorldSnapshotState.h"

#include <limits>
#include <new>
#include <string>
#include <string_view>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Adds cold configuration context while preserving the original typed failure and cause. */
        Error ConfigurationStageError(Error error, std::string_view stage) {
            error.message = std::string(stage) + ": " + error.message;
            return error;
        }

        bool ValidConfiguration(const PhysicsStateCaptureConfiguration &configuration) noexcept {
            return configuration.contentLease && configuration.snapshotSlots != 0 &&
                   configuration.snapshotSlots <= MaximumPhysicsResourceRecords && configuration.maximumRetainedBytes != 0;
        }

        bool BoundRuntime(const auto &world) noexcept {
            return world.runtime->stateBuild && world.runtime->stateBuildBinding && world.runtime->stateInventory;
        }

        Result<void> ColdAdmission(auto &world, const PhysicsStateCaptureConfiguration &configuration) {
            if (world.runtime->ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            if (world.runtime->mode == PhysicsRuntimeMode::Null)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
            if (!BoundRuntime(world))
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            if (!ValidConfiguration(configuration))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            if (world.snapshots)
                world.snapshots->CollectAbandonedRestore();
            if (world.snapshots && world.snapshots->pendingRestore)
                return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PhysicsWorldSnapshotState::Prepare */
    Result<std::shared_ptr<PhysicsWorldSnapshotState>> PhysicsWorldSnapshotState::Prepare(
        PhysicsWorld::Impl &owner, const PhysicsStateCaptureConfiguration &configuration) {
        using Outcome = Result<std::shared_ptr<PhysicsWorldSnapshotState>>;
        if (auto admission = ColdAdmission(owner, configuration); admission.HasError())
            return Outcome::Failure(admission.ErrorValue());
        try {
            auto prepared = std::make_shared<PhysicsWorldSnapshotState>();
            prepared->world = &owner;
            if (auto joined = prepared->CheckJoinedOwner(); joined.HasError())
                return Outcome::Failure(joined.ErrorValue());
            if (auto configured = prepared->PrepareConfiguration(configuration); configured.HasError())
                return Outcome::Failure(configured.ErrorValue());
            if (auto registered = prepared->RegisterConfiguration(); registered.HasError())
                return Outcome::Failure(registered.ErrorValue());
            return Outcome::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsWorldSnapshotState::PrepareConfiguration */
    Result<void> PhysicsWorldSnapshotState::PrepareConfiguration(const PhysicsStateCaptureConfiguration &configuration) {
        auto native = CanonicalPhysicsStateWorkspace::Prepare(world->native, world->settings, configuration);
        if (native.HasError())
            return Result<void>::Failure(ConfigurationStageError(std::move(native).ErrorValue(), "Physics state native workspace"));
        workspace = std::move(native).Value();
        resources = std::make_shared<PhysicsStateResourceRecord>(ownerThread);
        resources->build = *world->runtime->stateBuildBinding;
        resources->content = configuration.contentLease;
        resources->native = workspace->Resources();
        restoreControl = std::make_shared<PhysicsStateRestoreCandidate::Impl>();
        restoreControl->resources = resources;
        restoreControl->owner = weak_from_this();
        restoreControl->ownerThread = ownerThread;
        if (auto compatible = PrepareCompatibility(configuration); compatible.HasError())
            return Result<void>::Failure(ConfigurationStageError(std::move(compatible).ErrorValue(), "Physics state compatibility"));
        if (auto storage = PreparePublicationStorage(configuration); storage.HasError())
            return Result<void>::Failure(ConfigurationStageError(std::move(storage).ErrorValue(), "Physics state publication storage"));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::RegisterConfiguration */
    Result<void> PhysicsWorldSnapshotState::RegisterConfiguration() {
        if (world->runtime->nativeStateCandidates == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto &inventory = *world->runtime->stateInventory;
        (void)inventory.Collect(inventory.Budget().maximumCaptureOwners);
        resources->reservedPayloadBytes = retainedPayloadBytes;
        if (auto registered = inventory.Register(resources); registered.HasError())
            return registered;
        ++world->runtime->nativeStateCandidates;
        candidateRegistered = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
