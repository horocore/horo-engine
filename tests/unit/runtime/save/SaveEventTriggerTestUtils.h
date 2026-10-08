#pragma once
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveEventTriggers.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::EventTriggerTestSupport {
    inline CookedSaveProjectPolicy Policy() {
        SaveProjectPolicy project;
        for (const auto mode : {SavePolicyMode::Auto, SavePolicyMode::Checkpoint}) {
            auto &value = project.modes[static_cast<std::size_t>(mode)];
            value.enabled = true;
            value.rotation = {SaveRotationStrategy::ReplaceSingle, 1, false};
            value.cooldown = {10, true};
        }
        return CookedSaveProjectPolicy::Create(project, SaveRuntimeCapabilities::AllSupported()).Value();
    }

    struct Fixture final : ISaveSafePointExecutor {
        SaveOperationArbiter arbiter{CreateSaveOperationArbiter({8}).Value()};
        SaveTriggerHostState host{.binding = {.active = Test::Address().namespaceAccess.expected,
                                              .state = SaveNamespaceBindingState::Available,
                                              .revision = 7},
                                  .catalog = {.revision = 1},
                                  .generation = {3, 4, 5},
                                  .authorized = true};
        std::unique_ptr<SaveSafePointCoordinator> safePoints{SaveSafePointCoordinator::Create(host.generation, 8).Value()};
        std::array<SaveTriggerRegistration, 5> registrations;
        std::unique_ptr<SaveEventTriggers> triggers;
        std::optional<SaveTriggerHandoff> forwarded;
        SaveRuntimeGeneration captured;
        OperationId next{101};
        unsigned captures{};

        Fixture() {
            for (std::size_t index = 0; index < registrations.size(); ++index) {
                registrations[index] = {.id = {index + 1},
                                        .kind = static_cast<SaveTriggerKind>(index),
                                        .target = {Test::Address().namespaceAccess.expected, Test::Id<SaveGameSlotId>(1)}};
            }
            registrations[2].projectSchema = 9;
            registrations[2].minimumPayloadBytes = 1;
            triggers = SaveEventTriggers::Create(registrations, Policy(), host, arbiter, *safePoints).Value();
        }

        SaveTriggerEvent Event(const std::uint64_t trigger = 1, const std::uint64_t sequence = 1) const {
            SaveTriggerEvent event{{{trigger}, sequence}, host.generation, {}};
            if (trigger == 2)
                event.payload = SaveMilestonePayload{72};
            if (trigger == 3)
                event.payload = SaveProjectTriggerPayload{9, 1, {std::byte{42}}};
            if (trigger == 4 || trigger == 5) {
                auto source = host.generation;
                auto destination = source;
                if (trigger == 4)
                    ++destination.scene;
                else
                    --source.scene;
                event.payload = SaveTransitionPayload{71, source, destination};
            }
            return event;
        }

        Result<std::optional<SaveTriggerHandoff>> Poll() {
            return triggers->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges,
                                               {.operation = next++, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 2});
        }

        SaveTriggerHandoff Admit(const SaveTriggerEvent &event) {
            REQUIRE(triggers->Submit(event).HasValue());
            auto result = Poll();
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            forwarded = *result.Value();
            return *forwarded;
        }

        Result<void> Capture(const OperationId operation, const SaveRuntimeGeneration generation) override {
            if (const auto valid = triggers->Revalidate(*forwarded); valid.HasError())
                return valid;
            captured = generation;
            ++captures;
            REQUIRE(arbiter.Advance(operation, SaveArbiterState::Capturing).HasValue());
            return arbiter.Advance(operation, SaveArbiterState::Encoding);
        }

        Result<void> Restore(OperationId, SaveRuntimeGeneration) override {
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleInvalid));
        }

        void CaptureNow() {
            const auto drained = safePoints->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, host.generation, 8, *this);
            REQUIRE(drained.HasValue());
            REQUIRE(drained.Value().captured == 1);
        }

        void Complete() {
            const auto operation = forwarded->receipt.operation.Id();
            REQUIRE(arbiter.Advance(operation, SaveArbiterState::Committing, {1, 1}).HasValue());
            REQUIRE(arbiter.Complete(operation).HasValue());
        }
    };
}  // namespace Horo::Runtime::EventTriggerTestSupport
