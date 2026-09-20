#pragma once

#include "CharacterWorldInternal.h"

#include <cmath>

namespace Horo::Character::Detail {
    struct SpawnRecovery final {
        Math::Vec3 position;
        std::uint32_t iterations{};
    };

    /** @brief Keeps controller records resident while an owner-thread adapter callback is in flight. */
    template <typename Impl> struct PlacementOperationGuard final {
        explicit PlacementOperationGuard(Impl &world) noexcept : impl(world), previous(std::exchange(impl.placementActive, true)) {}

        ~PlacementOperationGuard() noexcept {
            impl.placementActive = previous;
            DrainDeferredShutdown(impl);
        }

        PlacementOperationGuard(const PlacementOperationGuard &) = delete;
        PlacementOperationGuard &operator=(const PlacementOperationGuard &) = delete;

        Impl &impl;
        bool previous{};
    };

    /** @brief Checks a Character root against the Physics local-origin safety envelope. */
    [[nodiscard]] Result<void> ValidatePlacementPosition(const Math::Vec3 position) {
        if (!Math::IsFinite(position) || std::abs(position.x) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
            std::abs(position.y) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
            std::abs(position.z) > Physics::MaximumPhysicsLocalHalfExtentMeters)
            return Result<void>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character placement is outside the Physics local-origin envelope."));
        return Result<void>::Success();
    }

    /** @brief Checks one qualified Character displacement against the world work budget. */
    [[nodiscard]] Result<void> ValidatePlacementDisplacement(const Math::Vec3 from, const Math::Vec3 to, const float maximumDisplacement) {
        if (const auto fromValid = ValidatePlacementPosition(from); fromValid.HasError())
            return fromValid;
        if (const auto toValid = ValidatePlacementPosition(to); toValid.HasError())
            return toValid;
        const auto displacement = to - from;
        const auto squared = static_cast<double>(Math::LengthSquared(displacement));
        if (const auto maximumSquared = static_cast<double>(maximumDisplacement) * maximumDisplacement;
            !std::isfinite(squared) || squared > maximumSquared)
            return Result<void>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character placement exceeds its qualified displacement bound."));
        return Result<void>::Success();
    }

    /** @brief Maps one placement operation to the exact world and Physics snapshot query contract. */
    [[nodiscard]] Result<void> ValidateQueryContext(const auto &impl, const CharacterPhysicsQueryContext &query,
                                                    const std::uint64_t expectedTick) {
        const CharacterPhysicsQueryExpectations expected{impl.descriptor.sceneGeneration,        impl.descriptor.identity,
                                                         impl.descriptor.physicsWorld,           impl.descriptor.collisionFilterGeneration,
                                                         impl.descriptor.originGeneration,       expectedTick,
                                                         impl.descriptor.physicsSnapshotRevision};
        return ValidateCharacterPhysicsQueryContext(query, expected);
    }

    /** @brief Revalidates owner state and exact controller liveness after an adapter callback. */
    [[nodiscard]] Result<void> ValidatePlacementContinuation(auto &impl, const CharacterControllerHandle &handle) {
        if (impl.state.load() != CharacterWorldState::Active || !impl.placementActive || impl.shutdownRequested)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto registryLock = impl.synchronization.LockRegistry();
        if (const auto record = impl.controllers.Resolve(handle); record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        return Result<void>::Success();
    }

    /** @brief Performs bounded overlap recovery and re-probes after the final allowed displacement. */
    [[nodiscard]] Result<SpawnRecovery> RecoverSpawnPosition(auto &impl, const CharacterControllerHandle &handle,
                                                             const CharacterControllerDescriptor &descriptor,
                                                             const CharacterPhysicsQueryContext &query, const std::uint64_t expectedTick,
                                                             const Math::Vec3 initialPosition) {
        if (const auto valid = ValidateQueryContext(impl, query, expectedTick); valid.HasError())
            return Result<SpawnRecovery>::Failure(valid.ErrorValue());

        if (const auto valid = ValidatePlacementPosition(initialPosition); valid.HasError())
            return Result<SpawnRecovery>::Failure(valid.ErrorValue());
        Math::Vec3 position = initialPosition;
        const auto maximumIterations = impl.settings.Values().work.maximumRecoveryIterations;
        for (std::uint32_t iteration{};; ++iteration) {
            const CharacterOverlapProbeRequest request{
                handle,
                impl.descriptor.sceneGeneration,
                impl.descriptor.identity,
                impl.descriptor.physicsWorld,
                descriptor.capsule,
                position,
                descriptor.up,
                descriptor.collisionProfile,
                descriptor.queryChannel,
                iteration,
            };
            const auto probe = query.overlap(query.context, request);
            if (const auto continuation = ValidatePlacementContinuation(impl, handle); continuation.HasError())
                return Result<SpawnRecovery>::Failure(continuation.ErrorValue());
            if (probe.HasError())
                return Result<SpawnRecovery>::Failure(probe.ErrorValue());
            if (const auto valid = ValidateCharacterOverlapProbeResult(probe.Value()); valid.HasError())
                return Result<SpawnRecovery>::Failure(valid.ErrorValue());
            if (probe.Value().overlapCount == 0)
                return Result<SpawnRecovery>::Success({position, iteration});
            if (iteration >= maximumIterations)
                return Result<SpawnRecovery>::Failure(MakeError(CharacterErrors::OverlapRecoveryFailed));
            position += probe.Value().recoveryDisplacement;
            if (const auto valid =
                    ValidatePlacementDisplacement(initialPosition, position, impl.settings.Values().work.maximumDisplacementMetersPerTick);
                valid.HasError())
                return Result<SpawnRecovery>::Failure(valid.ErrorValue());
        }
    }

    /** @brief Requires a teleport target to be clear without applying spawn recovery. */
    [[nodiscard]] Result<void> ValidateTeleportClearance(auto &impl, const CharacterControllerHandle &handle,
                                                         const CharacterControllerDescriptor &descriptor,
                                                         const CharacterPhysicsQueryContext &query, const std::uint64_t expectedTick,
                                                         const Math::Vec3 position) {
        if (const auto valid = ValidateQueryContext(impl, query, expectedTick); valid.HasError())
            return valid;
        if (const auto valid = ValidatePlacementPosition(position); valid.HasError())
            return valid;
        const CharacterOverlapProbeRequest request{
            handle,
            impl.descriptor.sceneGeneration,
            impl.descriptor.identity,
            impl.descriptor.physicsWorld,
            descriptor.capsule,
            position,
            descriptor.up,
            descriptor.collisionProfile,
            descriptor.queryChannel,
            0,
        };
        const auto probe = query.overlap(query.context, request);
        if (const auto continuation = ValidatePlacementContinuation(impl, handle); continuation.HasError())
            return continuation;
        if (probe.HasError())
            return Result<void>::Failure(probe.ErrorValue());
        if (const auto valid = ValidateCharacterOverlapProbeResult(probe.Value()); valid.HasError())
            return valid;
        if (probe.Value().overlapCount != 0)
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid, "Teleport target overlaps Physics geometry."));
        return Result<void>::Success();
    }

    /** @brief Reserves one exact next tick before a teleport invokes the overlap adapter. */
    [[nodiscard]] Result<CharacterControllerDescriptor> ReserveTeleport(auto &impl, const CharacterTeleportRequest &request) {
        const auto queueLock = impl.synchronization.LockCommands();
        const auto registryLock = impl.synchronization.LockRegistry();
        const auto record = impl.controllers.ResolveMutable(request.controller);
        if (record.HasError())
            return Result<CharacterControllerDescriptor>::Failure(record.ErrorValue());
        if (!record.Value()->spawned)
            return Result<CharacterControllerDescriptor>::Failure(
                MakeError(CharacterErrors::InvalidState, "A Character controller must be spawned before teleport."));
        if (impl.closedTick.load() == std::numeric_limits<std::uint64_t>::max() || request.tick != impl.closedTick.load() + 1 ||
            record.Value()->reservedTeleportTick.has_value() || record.Value()->lastTeleportTick >= request.tick)
            return Result<CharacterControllerDescriptor>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        if (std::ranges::any_of(impl.fastPath.Commands(), [&request](const CharacterMovementRequest &command) {
            return command.controller == request.controller && command.tick == request.tick;
        }))
            return Result<CharacterControllerDescriptor>::Failure(
                MakeError(CharacterErrors::CommandOrderInvalid, "Move and teleport cannot target one Character tick."));
        record.Value()->reservedTeleportTick = request.tick;
        return Result<CharacterControllerDescriptor>::Success(record.Value()->descriptor);
    }

    /** @brief Releases a failed teleport reservation without changing the prior publication. */
    void ReleaseTeleportReservation(auto &impl, const CharacterControllerHandle &handle, const std::uint64_t tick) noexcept {
        const auto registryLock = impl.synchronization.LockRegistry();
        const auto record = impl.controllers.ResolveMutable(handle);
        if (record.HasValue() && record.Value()->reservedTeleportTick == tick)
            record.Value()->reservedTeleportTick.reset();
    }

    /** @brief Copies the current published root while retaining registry lifetime synchronization. */
    [[nodiscard]] Result<Math::Vec3> CurrentControllerPosition(const auto &impl, const CharacterControllerHandle &handle) {
        const auto registryLock = impl.synchronization.LockRegistry();
        const auto record = impl.controllers.Resolve(handle);
        if (record.HasError())
            return Result<Math::Vec3>::Failure(record.ErrorValue());
        return Result<Math::Vec3>::Success(record.Value()->publication.position);
    }

    /** @brief Commits one validated placement after re-resolving the exact live controller record. */
    [[nodiscard]] Result<CharacterPlacementResult> PublishPlacement(auto &impl, const CharacterControllerHandle &handle,
                                                                    const CharacterPlacementOperation operation, const std::uint64_t tick,
                                                                    const Math::Vec3 position, const Math::Quaternion heading,
                                                                    const std::uint32_t recoveryIterations) {
        const auto registryLock = impl.synchronization.LockRegistry();
        if (impl.state.load() != CharacterWorldState::Active || !impl.placementActive)
            return Result<CharacterPlacementResult>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto resolved = impl.controllers.ResolveMutable(handle);
        if (resolved.HasError())
            return Result<CharacterPlacementResult>::Failure(resolved.ErrorValue());
        auto *record = resolved.Value();
        const auto publicationLock = impl.synchronization.LockPublication();
        if (record->publication.publicationRevision == std::numeric_limits<std::uint64_t>::max())
            return Result<CharacterPlacementResult>::Failure(MakeError(CharacterErrors::PublicationRevisionExhausted));
        const auto nextRevision = record->publication.publicationRevision + 1;
        if (operation == CharacterPlacementOperation::Teleport && record->reservedTeleportTick != tick)
            return Result<CharacterPlacementResult>::Failure(MakeError(CharacterErrors::InvalidState));
        const CharacterTransformPublication publication{handle,       tick,
                                                        nextRevision, position,
                                                        heading,      record->descriptor.up,
                                                        false,        false,
                                                        true,         CharacterTransformAuthority::CharacterController};
        if (const auto valid =
                ValidateCharacterTransformPublication(publication, impl.descriptor.sceneGeneration, impl.descriptor.identity);
            valid.HasError())
            return Result<CharacterPlacementResult>::Failure(valid.ErrorValue());
        record->publication = publication;
        record->spawned = true;
        if (operation == CharacterPlacementOperation::Teleport) {
            record->lastMovement.reset();
            record->locomotion.reset();
        }
        if (operation == CharacterPlacementOperation::Teleport) {
            record->lastTeleportTick = tick;
            record->reservedTeleportTick.reset();
        }
        return Result<CharacterPlacementResult>::Success({handle, operation, publication, recoveryIterations, recoveryIterations != 0});
    }

}  // namespace Horo::Character::Detail
