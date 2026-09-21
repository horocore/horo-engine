#pragma once

#include "CharacterWorldInternal.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Horo::Character::Detail {
    /** @brief Validates immutable request evidence before attempting queue ownership. */
    [[nodiscard]] Result<void> ValidateAdmissionRequest(const auto &impl, const CharacterMovementRequest &request) {
        if (!impl.acceptingCommands.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (const auto valid = ValidateCharacterMovementRequest(request, impl.descriptor.sceneGeneration, impl.descriptor.identity);
            valid.HasError())
            return valid;
        if (request.tick <= impl.closedTick.load())
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @brief Tests whether one queued sequence makes a new request duplicate or globally stale. */
    [[nodiscard]] bool ConflictsWithQueuedCommand(const CharacterMovementRequest &queued,
                                                  const CharacterMovementRequest &request) noexcept {
        if (queued.controller != request.controller)
            return false;
        return (queued.tick == request.tick && queued.sequence == request.sequence) ||
               (queued.tick < request.tick && queued.sequence >= request.sequence) ||
               (queued.tick > request.tick && queued.sequence <= request.sequence);
    }

    /** @brief Revalidates lifecycle/order and exact duplication while queue ownership is held. */
    [[nodiscard]] Result<void> ValidateLockedAdmission(const auto &impl, const CharacterMovementRequest &request) {
        if (!impl.acceptingCommands.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        const std::uint32_t slot = request.controller.slot.index;
        if (slot >= impl.controllerGenerations.size() || impl.controllerGenerations[slot] != request.controller.slot.generation)
            return Result<void>::Failure(MakeError(CharacterErrors::HandleStale));
        if (request.tick <= impl.closedTick.load() || request.sequence <= impl.closedSequences[slot])
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        const auto record = impl.controllers.Resolve(request.controller);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        if (record.Value()->reservedTeleportTick == request.tick || record.Value()->lastTeleportTick == request.tick)
            return Result<void>::Failure(
                MakeError(CharacterErrors::CommandOrderInvalid, "Move and teleport cannot target one Character tick."));
        if (std::ranges::any_of(impl.fastPath.Commands(), [&request](const CharacterMovementRequest &queued) {
            return ConflictsWithQueuedCommand(queued, request);
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        return Result<void>::Success();
    }

    /** @brief Orders owned commands independently of producer arrival timing. */
    [[nodiscard]] bool CommandLess(const CharacterMovementRequest &left, const CharacterMovementRequest &right) noexcept {
        if (left.tick != right.tick)
            return left.tick < right.tick;
        if (left.controller != right.controller)
            return left.controller < right.controller;
        return left.sequence < right.sequence;
    }

    /** @brief Restores the non-reentrant tick guard and completes deferred owner shutdown. */
    template <typename Impl> struct TickGuard final {
        explicit TickGuard(Impl &impl) noexcept : impl_(impl), previous_(impl_.ticking.exchange(true)) {}

        ~TickGuard() noexcept {
            impl_.ticking.store(previous_);
            DrainDeferredShutdown(impl_);
        }

        TickGuard(const TickGuard &) = delete;
        TickGuard &operator=(const TickGuard &) = delete;

        Impl &impl_;
        bool previous_{};
    };

    /** @brief Validates a frozen frame completely before queue or controller publication changes. */
    [[nodiscard]] Result<void> ValidateFrozenCommands(auto &impl) {
        for (std::size_t index = 0; index < impl.fastPath.CommandScratch().size(); ++index) {
            const CharacterMovementRequest &command = impl.fastPath.CommandScratch()[index];
            if (const auto valid = ValidateCharacterMovementRequest(command, impl.descriptor.sceneGeneration, impl.descriptor.identity);
                valid.HasError())
                return valid;
            const auto record = impl.controllers.ResolveMutable(command.controller);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            if (command.sequence <= record.Value()->lastSequence)
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
            if (index != 0 && impl.fastPath.CommandScratch()[index - 1].controller == command.controller &&
                impl.fastPath.CommandScratch()[index - 1].sequence == command.sequence)
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        }
        return Result<void>::Success();
    }

    /** @brief Canonicalizes and removes one validated eligible frame while holding queue ownership. */
    [[nodiscard]] Result<void> FreezeCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        const auto queueLock = impl.synchronization.LockCommands();
        if (std::ranges::any_of(impl.fastPath.Commands(), [&input](const CharacterMovementRequest &command) {
            return command.tick < input.tick;
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        if (const auto eligible = static_cast<std::size_t>(std::ranges::count_if(impl.fastPath.Commands(),
                                                                                 [&input](const CharacterMovementRequest &command) {
            return command.tick == input.tick;
        }));
            eligible > impl.settings.Values().work.maximumCommandsPerTick)
            return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));

        impl.fastPath.CommandScratch().clear();
        for (const CharacterMovementRequest &command : impl.fastPath.Commands()) {
            if (command.tick == input.tick)
                impl.fastPath.CommandScratch().push_back(command);
        }
        std::ranges::sort(impl.fastPath.CommandScratch(), CommandLess);
        if (const auto valid = ValidateFrozenCommands(impl); valid.HasError())
            return valid;
        for (const CharacterMovementRequest &command : impl.fastPath.CommandScratch())
            impl.closedSequences[command.controller.slot.index] = command.sequence;
        std::erase_if(impl.fastPath.Commands(), [&input](const CharacterMovementRequest &command) {
            return command.tick == input.tick;
        });
        impl.pendingCommands.store(static_cast<std::uint32_t>(impl.fastPath.Commands().size()));
        impl.closedTick.store(input.tick);
        return Result<void>::Success();
    }

    /** @brief Checks one post-tick position against the fixed Character movement envelope. */
    [[nodiscard]] Result<void> ValidateMovementPosition(const auto &impl, const Math::Vec3 previous, const Math::Vec3 candidate) {
        if (!Math::IsFinite(candidate) || std::abs(candidate.x) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
            std::abs(candidate.y) > Physics::MaximumPhysicsLocalHalfExtentMeters ||
            std::abs(candidate.z) > Physics::MaximumPhysicsLocalHalfExtentMeters)
            return Result<void>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character movement result is outside the Physics local-origin envelope."));
        const auto squared = static_cast<double>(Math::LengthSquared(candidate - previous));
        if (const auto maximum = static_cast<double>(impl.settings.Values().work.maximumDisplacementMetersPerTick);
            !std::isfinite(squared) || squared > maximum * maximum)
            return Result<void>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character movement result exceeds its fixed-tick displacement bound."));
        return Result<void>::Success();
    }

    /** @brief Provides the stable response rank used by the Character sweep reducer. */
    [[nodiscard]] std::uint8_t SweepResponseRank(const Physics::PhysicsQueryResponse response) noexcept {
        return response == Physics::PhysicsQueryResponse::Block ? 0U : 1U;
    }

    /** @brief Orders sweep evidence independently of native collector traversal order. */
    [[nodiscard]] bool SweepHitLess(const CharacterSweepHit &left, const CharacterSweepHit &right) noexcept {
        const auto leftBody = left.body.value_or(Physics::BodyHandle{});
        const auto rightBody = right.body.value_or(Physics::BodyHandle{});
        const auto leftKey = std::tuple{left.distanceMeters,
                                        SweepResponseRank(left.response),
                                        left.body.has_value(),
                                        leftBody.world,
                                        leftBody.slot.index,
                                        leftBody.slot.generation,
                                        left.shape.world,
                                        left.shape.slot.index,
                                        left.shape.slot.generation,
                                        left.point.x,
                                        left.point.y,
                                        left.point.z,
                                        left.normal.x,
                                        left.normal.y,
                                        left.normal.z};
        const auto rightKey = std::tuple{right.distanceMeters,
                                         SweepResponseRank(right.response),
                                         right.body.has_value(),
                                         rightBody.world,
                                         rightBody.slot.index,
                                         rightBody.slot.generation,
                                         right.shape.world,
                                         right.shape.slot.index,
                                         right.shape.slot.generation,
                                         right.point.x,
                                         right.point.y,
                                         right.point.z,
                                         right.normal.x,
                                         right.normal.y,
                                         right.normal.z};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        if (left.material.has_value() != right.material.has_value())
            return !left.material.has_value();
        if (!left.material.has_value())
            return false;
        const auto leftSlot = left.material->slot.Value();
        const auto rightSlot = right.material->slot.Value();
        return std::tie(left.material->asset.Bytes(), left.material->assetGeneration, leftSlot) <
               std::tie(right.material->asset.Bytes(), right.material->assetGeneration, rightSlot);
    }

    /** @brief Orders retained contacts by stable identities and copied geometry. */
    [[nodiscard]] bool ContactLess(const CharacterSurfaceContact &left, const CharacterSurfaceContact &right) noexcept {
        const auto leftBody = left.body.value_or(Physics::BodyHandle{});
        const auto rightBody = right.body.value_or(Physics::BodyHandle{});
        const auto leftKey = std::tuple{left.body.has_value(),
                                        leftBody.world,
                                        leftBody.slot.index,
                                        leftBody.slot.generation,
                                        left.shape.world,
                                        left.shape.slot.index,
                                        left.shape.slot.generation,
                                        left.normal.x,
                                        left.normal.y,
                                        left.normal.z,
                                        left.point.x,
                                        left.point.y,
                                        left.point.z};
        const auto rightKey = std::tuple{right.body.has_value(),
                                         rightBody.world,
                                         rightBody.slot.index,
                                         rightBody.slot.generation,
                                         right.shape.world,
                                         right.shape.slot.index,
                                         right.shape.slot.generation,
                                         right.normal.x,
                                         right.normal.y,
                                         right.normal.z,
                                         right.point.x,
                                         right.point.y,
                                         right.point.z};
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        const auto leftSlot = left.material.slot.Value();
        const auto rightSlot = right.material.slot.Value();
        return std::tie(left.material.asset.Bytes(), left.material.assetGeneration, leftSlot) <
               std::tie(right.material.asset.Bytes(), right.material.assetGeneration, rightSlot);
    }

    /** @brief Classifies one blocking normal without changing the controller's up basis. */
    [[nodiscard]] CharacterCollisionFlags CollisionFlagForNormal(const Math::Vec3 normal, const CharacterControllerDescriptor &descriptor) {
        const float upDot = Math::Dot(normal, descriptor.up);
        const float slopeRadians = descriptor.maximumSlopeDegrees * Math::Pi / 180.0F;
        const float walkableCosine = std::cos(slopeRadians);
        if (upDot > 0.0F && upDot >= walkableCosine)
            return CharacterCollisionFlags::Ground;
        if (upDot < 0.0F && -upDot >= walkableCosine)
            return CharacterCollisionFlags::Ceiling;
        return CharacterCollisionFlags::Sides;
    }

    /** @brief Retains one stable contact and reports overflow without allocating or reordering state. */
    void RetainSweepContact(CharacterMovementResult &result, const CharacterSweepHit &hit,
                            const CharacterControllerDescriptor &descriptor) {
        const auto duplicate = std::ranges::find_if(result.contacts.begin(), result.contacts.begin() + result.contactCount,
                                                    [&hit](const CharacterSurfaceContact &contact) {
            return contact.body == hit.body && contact.shape == hit.shape && contact.normal == hit.normal;
        });
        if (duplicate != result.contacts.begin() + result.contactCount)
            return;
        if (result.contactCount >= descriptor.maximumContacts) {
            result.truncated = true;
            return;
        }
        auto &contact = result.contacts[result.contactCount++];
        contact.body = hit.body;
        contact.shape = hit.shape;
        contact.point = hit.point;
        contact.normal = hit.normal;
        contact.material = hit.material.value_or(descriptor.defaultMaterial);
        contact.penetrationDepthMeters = std::max(0.0F, descriptor.skinWidthMeters - hit.distanceMeters);
    }

    /**
     * @brief Resolves one desired displacement through bounded Horo capsule sweeps and iterative slide.
     *
     * The callback only supplies copied evidence. Character owns ordering, skin-width advancement,
     * contact retention and projection against the canonical normal prefix, so native traversal order
     * cannot change the resulting motion.
     */
    [[nodiscard]] Result<CharacterMovementResult> BuildCapsuleSweepMovementResult(const auto &impl, const CharacterMovementRequest &command,
                                                                                  const CharacterTransformPublication &previous,
                                                                                  const CharacterFixedTickInput &input,
                                                                                  const CharacterControllerDescriptor &descriptor) {
        const CharacterPhysicsQueryExpectations expected{impl.descriptor.sceneGeneration,        impl.descriptor.identity,
                                                         impl.descriptor.physicsWorld,           impl.descriptor.collisionFilterGeneration,
                                                         impl.descriptor.originGeneration,       input.tick,
                                                         impl.descriptor.physicsSnapshotRevision};
        if (const auto valid = ValidateCharacterPhysicsQueryContext(input.query, expected); valid.HasError())
            return Result<CharacterMovementResult>::Failure(valid.ErrorValue());

        CharacterMovementResult result;
        result.controller = command.controller;
        result.tick = input.tick;
        result.sequence = command.sequence;
        result.finalPosition = previous.position;
        result.finalHeading = command.desiredHeading.value_or(previous.heading);
        result.up = descriptor.up;
        result.groundNormal = descriptor.up;
        result.groundingRevalidationRequired = true;

        if (!command.desiredVelocityMetersPerSecond.has_value())
            return Result<CharacterMovementResult>::Success(std::move(result));

        const double seconds = static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0;
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > static_cast<double>(std::numeric_limits<float>::max()))
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character fixed-tick delta cannot produce a finite movement result."));
        const float elapsedSeconds = static_cast<float>(seconds);
        Math::Vec3 remaining = *command.desiredVelocityMetersPerSecond * elapsedSeconds;
        if (!Math::IsFinite(remaining))
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Character desired displacement is not finite."));

        Math::Vec3 position = previous.position;
        constexpr float DistanceEpsilon = 1.0e-5F;
        constexpr float NormalEpsilon = 1.0e-5F;
        const std::uint32_t maximumIterations = impl.settings.Values().work.maximumMovementIterations;
        for (std::uint32_t iteration{}; iteration < maximumIterations; ++iteration) {
            const float distance = Math::Length(remaining);
            if (!std::isfinite(distance) || distance <= descriptor.minimumMoveDistanceMeters)
                break;
            const Math::Vec3 direction = remaining / distance;
            const CharacterSweepProbeRequest request{command.controller,
                                                     impl.descriptor.sceneGeneration,
                                                     impl.descriptor.identity,
                                                     impl.descriptor.physicsWorld,
                                                     descriptor.capsule,
                                                     position,
                                                     descriptor.up,
                                                     direction,
                                                     distance,
                                                     descriptor.collisionProfile,
                                                     descriptor.queryChannel,
                                                     iteration};
            const auto probe = input.query.sweep(input.query.context, request);
            if (impl.state.load() != CharacterWorldState::Active)
                return Result<CharacterMovementResult>::Failure(MakeError(CharacterErrors::InvalidState));
            if (probe.HasError())
                return Result<CharacterMovementResult>::Failure(probe.ErrorValue());
            CharacterSweepProbeResult evidence = std::move(probe).Value();
            if (const auto valid = ValidateCharacterSweepProbeResult(evidence, request); valid.HasError())
                return Result<CharacterMovementResult>::Failure(valid.ErrorValue());
            std::ranges::sort(evidence.hits.begin(), evidence.hits.begin() + evidence.hitCount, SweepHitLess);

            float nearest = distance;
            bool blocked{};
            for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
                const auto &hit = evidence.hits[index];
                if (hit.response != Physics::PhysicsQueryResponse::Block || Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                    continue;
                if (!blocked || hit.distanceMeters < nearest) {
                    nearest = hit.distanceMeters;
                    blocked = true;
                }
            }
            if (!blocked) {
                position += remaining;
                remaining = {};
                break;
            }

            const float travel = std::max(0.0F, nearest - descriptor.skinWidthMeters);
            position += direction * travel;
            remaining -= direction * travel;

            std::array<Math::Vec3, MaximumCharacterSweepHits> activeNormals{};
            std::uint32_t activeNormalCount{};
            for (std::uint32_t index{}; index < evidence.hitCount; ++index) {
                const auto &hit = evidence.hits[index];
                if (hit.response != Physics::PhysicsQueryResponse::Block || hit.distanceMeters > nearest + DistanceEpsilon ||
                    Math::Dot(hit.normal, direction) >= -NormalEpsilon)
                    continue;
                result.collisions = result.collisions | CollisionFlagForNormal(hit.normal, descriptor);
                RetainSweepContact(result, hit, descriptor);
                if (activeNormalCount < activeNormals.size())
                    activeNormals[activeNormalCount++] = hit.normal;
            }

            const float beforeProjection = Math::LengthSquared(remaining);
            for (std::uint32_t index{}; index < activeNormalCount; ++index) {
                const float intoSurface = Math::Dot(remaining, activeNormals[index]);
                if (intoSurface < 0.0F)
                    remaining -= activeNormals[index] * intoSurface;
            }
            const float afterProjection = Math::LengthSquared(remaining);
            if (std::isfinite(beforeProjection) && std::isfinite(afterProjection) && afterProjection > beforeProjection &&
                afterProjection > 0.0F)
                remaining *= std::sqrt(beforeProjection / afterProjection);
        }

        result.finalPosition = position;
        result.achievedVelocityMetersPerSecond = (position - previous.position) / elapsedSeconds;
        std::ranges::sort(result.contacts.begin(), result.contacts.begin() + result.contactCount, ContactLess);
        return Result<CharacterMovementResult>::Success(std::move(result));
    }

    /** @brief Produces a deterministic backend-free result when no Physics resolver is attached. */
    [[nodiscard]] Result<CharacterMovementResult> BuildBaselineMovementResult(const auto &impl, const CharacterMovementRequest &command,
                                                                              const CharacterTransformPublication &previous,
                                                                              const CharacterFixedTickInput &input) {
        CharacterMovementResult result;
        result.controller = command.controller;
        result.tick = input.tick;
        result.sequence = command.sequence;
        result.finalPosition = previous.position;
        result.finalHeading = previous.heading;
        result.achievedVelocityMetersPerSecond = {};
        result.up = previous.up;
        result.groundNormal = previous.up;
        result.groundingRevalidationRequired = true;
        if (command.desiredVelocityMetersPerSecond.has_value()) {
            const double seconds = static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0;
            if (!std::isfinite(seconds) || seconds > static_cast<double>(std::numeric_limits<float>::max()))
                return Result<CharacterMovementResult>::Failure(
                    MakeError(CharacterErrors::PlacementInvalid, "Character fixed-tick delta cannot produce a finite movement result."));
            const auto elapsedSeconds = static_cast<float>(seconds);
            result.achievedVelocityMetersPerSecond = *command.desiredVelocityMetersPerSecond;
            result.finalPosition += result.achievedVelocityMetersPerSecond * elapsedSeconds;
        }
        if (command.desiredHeading.has_value())
            result.finalHeading = *command.desiredHeading;
        if (const auto valid = ValidateMovementPosition(impl, previous.position, result.finalPosition); valid.HasError())
            return Result<CharacterMovementResult>::Failure(valid.ErrorValue());
        return Result<CharacterMovementResult>::Success(std::move(result));
    }

    /** @brief Resolves and validates one backend-neutral result without mutating the controller. */
    [[nodiscard]] Result<CharacterMovementResult> ResolveMovementResult(auto &impl, const CharacterMovementRequest &command,
                                                                        const CharacterTransformPublication &previous,
                                                                        const CharacterFixedTickInput &input,
                                                                        const CharacterControllerDescriptor &descriptor) {
        Result<CharacterMovementResult> resolved =
            input.observer.movementResult ? input.observer.movementResult(input.observer.context, command, previous)
            : input.query.sweep           ? BuildCapsuleSweepMovementResult(impl, command, previous, input, descriptor)
                                          : BuildBaselineMovementResult(impl, command, previous, input);
        if (resolved.HasError())
            return Result<CharacterMovementResult>::Failure(resolved.ErrorValue());
        CharacterMovementResult result = std::move(resolved).Value();
        if (result.controller != command.controller || result.tick != input.tick || result.sequence != command.sequence)
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::RequestInvalid, "Movement result identity does not match its admitted command."));
        if (result.up != descriptor.up)
            return Result<CharacterMovementResult>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Movement result changed the controller up axis."));
        if (const auto valid = ValidateCharacterMovementResult(result, descriptor); valid.HasError())
            return Result<CharacterMovementResult>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateMovementPosition(impl, previous.position, result.finalPosition); valid.HasError())
            return Result<CharacterMovementResult>::Failure(valid.ErrorValue());
        return Result<CharacterMovementResult>::Success(std::move(result));
    }

    /** @brief Identifies the final replacement for one controller in sorted command scratch. */
    [[nodiscard]] bool IsFinalCommand(const auto &commands, const std::size_t index) noexcept {
        return index + 1 == commands.size() || commands[index + 1].controller != commands[index].controller;
    }

    /** @brief Builds and validates one immutable post-tick publication candidate. */
    [[nodiscard]] Result<CharacterLocomotionSnapshot> BuildLocomotionSnapshot(const CharacterControllerRecord &record,
                                                                              const CharacterMovementRequest &command,
                                                                              const CharacterMovementResult &movement,
                                                                              const CharacterFixedTickInput &input) {
        if (record.publication.publicationRevision == std::numeric_limits<std::uint64_t>::max() ||
            record.stateRevision == std::numeric_limits<std::uint64_t>::max())
            return Result<CharacterLocomotionSnapshot>::Failure(MakeError(CharacterErrors::PublicationRevisionExhausted));

        const std::uint64_t nextPublicationRevision = record.publication.publicationRevision + 1;
        const std::uint64_t nextStateRevision = record.stateRevision + 1;
        const CharacterTransformPublication publication{command.controller,
                                                        input.tick,
                                                        nextPublicationRevision,
                                                        movement.finalPosition,
                                                        movement.finalHeading,
                                                        movement.up,
                                                        movement.grounded,
                                                        movement.platformAttached,
                                                        movement.groundingRevalidationRequired,
                                                        CharacterTransformAuthority::CharacterController};
        const CharacterLocomotionSnapshot snapshot{command.controller, input.tick, nextStateRevision, movement, publication};
        if (const auto valid = ValidateCharacterLocomotionSnapshot(snapshot, record.descriptor); valid.HasError())
            return Result<CharacterLocomotionSnapshot>::Failure(valid.ErrorValue());
        return Result<CharacterLocomotionSnapshot>::Success(snapshot);
    }

    /** @brief Resolves final commands into validated movement candidates without mutating records. */
    [[nodiscard]] Result<std::uint32_t> ResolveCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        std::uint32_t applied{};
        auto &movementResults = impl.fastPath.MovementResults();
        for (std::size_t index = 0; index < impl.fastPath.CommandScratch().size(); ++index) {
            const CharacterMovementRequest &command = impl.fastPath.CommandScratch()[index];
            if (!IsFinalCommand(impl.fastPath.CommandScratch(), index))
                continue;
            CharacterTransformPublication previous;
            CharacterControllerDescriptor descriptor;
            bool spawned{};
            {
                const auto registryLock = impl.synchronization.LockRegistry();
                const auto record = impl.controllers.Resolve(command.controller);
                if (record.HasError())
                    return Result<std::uint32_t>::Failure(record.ErrorValue());
                descriptor = record.Value()->descriptor;
                previous = record.Value()->publication;
                spawned = record.Value()->spawned;
            }
            if (input.observer.movement)
                input.observer.movement(input.observer.context, command);
            if (impl.state.load() != CharacterWorldState::Active)
                return Result<std::uint32_t>::Failure(MakeError(CharacterErrors::InvalidState));
            if (spawned) {
                const auto resolved = ResolveMovementResult(impl, command, previous, input, descriptor);
                if (resolved.HasError())
                    return Result<std::uint32_t>::Failure(resolved.ErrorValue());
                CharacterMovementResult movement = std::move(resolved).Value();
                for (std::uint32_t contactIndex{}; contactIndex < movement.contactCount; ++contactIndex)
                    static_cast<void>(impl.fastPath.TryAppendContact(movement.contacts[contactIndex]));
                movementResults.push_back(std::move(movement));
            }
            ++applied;
        }

        if (impl.state.load() != CharacterWorldState::Active)
            return Result<std::uint32_t>::Failure(MakeError(CharacterErrors::InvalidState));
        return Result<std::uint32_t>::Success(applied);
    }

    /** @brief Validates all staged publications while registry and publication state are frozen. */
    [[nodiscard]] Result<void> PreflightCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        std::size_t movementResultIndex{};
        for (std::size_t index = 0; index < impl.fastPath.CommandScratch().size(); ++index) {
            if (!IsFinalCommand(impl.fastPath.CommandScratch(), index))
                continue;
            const CharacterMovementRequest &command = impl.fastPath.CommandScratch()[index];
            const auto record = impl.controllers.Resolve(command.controller);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            if (!record.Value()->spawned)
                continue;
            if (movementResultIndex >= impl.fastPath.MovementResults().size())
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            if (const auto snapshot =
                    BuildLocomotionSnapshot(*record.Value(), command, impl.fastPath.MovementResults()[movementResultIndex], input);
                snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            ++movementResultIndex;
        }
        return Result<void>::Success();
    }

    /** @brief Commits one final command and its already-preflighted immutable publication. */
    [[nodiscard]] Result<void> CommitCommand(auto &impl, const CharacterMovementRequest &command, const CharacterFixedTickInput &input,
                                             std::size_t &movementResultIndex) {
        const auto record = impl.controllers.ResolveMutable(command.controller);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        record.Value()->lastMovement = command;
        record.Value()->lastSequence = command.sequence;
        if (!record.Value()->spawned)
            return Result<void>::Success();
        if (movementResultIndex >= impl.fastPath.MovementResults().size())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto snapshot =
            BuildLocomotionSnapshot(*record.Value(), command, impl.fastPath.MovementResults()[movementResultIndex++], input);
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        CharacterLocomotionSnapshot committed = std::move(snapshot).Value();
        record.Value()->publication = committed.transform;
        record.Value()->stateRevision = committed.stateRevision;
        record.Value()->locomotion = std::move(committed);
        return Result<void>::Success();
    }

    /** @brief Preflights and commits all resolved movement candidates under one publication lock. */
    [[nodiscard]] Result<void> CommitCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        const auto registryLock = impl.synchronization.LockRegistry();
        const auto publicationLock = impl.synchronization.LockPublication();
        if (const auto valid = PreflightCommandFrame(impl, input); valid.HasError())
            return valid;
        std::size_t movementResultIndex{};
        for (std::size_t index = 0; index < impl.fastPath.CommandScratch().size(); ++index) {
            if (!IsFinalCommand(impl.fastPath.CommandScratch(), index))
                continue;
            if (const auto committed = CommitCommand(impl, impl.fastPath.CommandScratch()[index], input, movementResultIndex);
                committed.HasError())
                return committed;
        }
        return Result<void>::Success();
    }

    /** @brief Resolves and commits one fixed-tick frame without exposing mutable pipeline storage. */
    [[nodiscard]] Result<std::uint32_t> ApplyCommandFrame(auto &impl, const CharacterFixedTickInput &input) {
        const auto resolved = ResolveCommandFrame(impl, input);
        if (resolved.HasError())
            return Result<std::uint32_t>::Failure(resolved.ErrorValue());
        if (const auto committed = CommitCommandFrame(impl, input); committed.HasError())
            return Result<std::uint32_t>::Failure(committed.ErrorValue());
        return resolved;
    }

    /** @brief Atomically replaces the coherent Character publication marker. */
    void PublishTick(auto &impl, const CharacterFixedTickInput &input, const std::uint32_t applied) noexcept {
        const auto publicationLock = impl.synchronization.LockPublication();
        impl.published.completedTick = input.tick;
        ++impl.published.publicationRevision;
        impl.published.appliedCommands = applied;
    }

    /** @brief Emits one optional phase observation without exposing pipeline storage. */
    void ObservePhase(const CharacterFixedTickInput &input, const CharacterTickPhase phase) noexcept {
        if (input.observer.phase)
            input.observer.phase(input.observer.context, phase, input.tick);
    }
}  // namespace Horo::Character::Detail
