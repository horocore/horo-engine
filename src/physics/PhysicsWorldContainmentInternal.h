#pragma once

/** @file PhysicsWorldContainmentInternal.h
 * @brief Bounded owner-thread containment before solver entry and publication.
 */

#include "PhysicsWorldInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Retires corrupt native bodies before event reduction and preserves the first diagnostic. */
    [[nodiscard]] Result<void> ContainNonFiniteBodies(auto &impl, const PhysicsFixedTickInput &input, std::vector<BodyHandle> &quarantined,
                                                      const bool postStep = true) {
        std::size_t cursor{};
        while (const auto corrupt = Detail::FindCanonicalNonFiniteBody(impl.native, postStep, cursor)) {
            Error cause = corrupt->retirable
                              ? MakeError(PhysicsErrors::BodyStateNonFinite, "Canonical body state contains NaN or infinity.")
                              : MakeError(PhysicsErrors::SolverFatalCondition, "Resident native body vanished before publication.");
            if (!corrupt->retirable || impl.settings.Values().nonFinitePolicy == PhysicsNonFinitePolicy::FailWorld) {
                impl.Fail(cause, input.sceneGeneration, input.simulationTick);
                impl.RecordNonFiniteDiagnostic(cause, *corrupt, input.sceneGeneration, input.simulationTick);
                return Result<void>::Failure(std::move(cause));
            }
            if (quarantined.empty())
                impl.RecordNonFiniteDiagnostic(cause, *corrupt, input.sceneGeneration, input.simulationTick);
            if (!Detail::CanonicalQueryFixtureUsesBodyHandle(impl.native, corrupt->body))
                impl.queryEvents.events.SuppressBody(corrupt->body);
            Detail::QuarantineCanonicalSceneBody(impl.native, corrupt->body, impl.containment.quarantineSink);
            if (impl.queryBatch.pending)
                (void)impl.queryBatch.pending->FailCode(PhysicsErrors::QuerySnapshotStale);
            quarantined.push_back(corrupt->body);
        }
        return Result<void>::Success();
    }

    /** @brief Suppresses retired body payloads while retaining inert keys needed for source-sequence validation. */
    void SuppressQuarantinedCommands(auto &impl, const std::span<const BodyHandle> quarantined) {
        if (quarantined.empty())
            return;
        for (std::uint32_t index = 0; index < impl.commandCount; ++index) {
            PhysicsStructuralCommand &command = impl.CommandAt(index);
            if (command.order.targetKind != PhysicsCommandTargetKind::Body || impl.IsRetiredCommand(command.order))
                continue;
            if (!std::ranges::any_of(quarantined, [&command](const BodyHandle body) {
                return command.order.targetIdentity == static_cast<std::uint64_t>(body.slot.index) + 1U;
            }))
                continue;
            command.bodyMutation.reset();
            impl.containment.retiredCommands.push_back(command.order);
        }
        impl.statistics.pendingCommands = impl.commandCount - static_cast<std::uint32_t>(impl.containment.retiredCommands.size());
    }
}  // namespace Horo::Physics::Detail
