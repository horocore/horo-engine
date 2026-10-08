#include "CharacterFoundationQualificationTestHelpers.h"

namespace Horo::Character {
    namespace {
        using namespace QualificationTest;

        TEST_CASE("Character preparation allocation failures roll back every retained storage stage",
                  "[physics][character][qualification][allocation]") {
            const auto descriptor = WorldDescriptor();
            const auto settings = QualificationSettings();
            const auto before = Tests::AllocationProbe::Count();
            auto baseline = CharacterWorld::Prepare(descriptor, settings);
            const auto allocationCount = Tests::AllocationProbe::Count() - before;
            REQUIRE(baseline.HasValue());
            REQUIRE(allocationCount > 0);
            REQUIRE(allocationCount < 128);
            auto lastIdentity = baseline.Value()->Descriptor().identity;
            for (std::size_t failAt = 0; failAt < allocationCount; ++failAt) {
                INFO("prepared storage allocation " << failAt);
                std::optional<Result<std::unique_ptr<CharacterWorld>>> failed;
                {
                    Tests::AllocationProbe::ScopedFailure failure{failAt};
                    failed = CharacterWorld::Prepare(descriptor, settings);
                }
                REQUIRE(failed.has_value());
                RequireError(*failed, CharacterErrors::CapacityExceeded);
                auto recovered = CharacterWorld::Prepare(descriptor, settings);
                REQUIRE(recovered.HasValue());
                REQUIRE(recovered.Value()->Descriptor().identity > lastIdentity);
                lastIdentity = recovered.Value()->Descriptor().identity;
                REQUIRE(recovered.Value()->ActiveControllerCount() == 0);
                REQUIRE(recovered.Value()->ControllerCapacity() == ControllerCount);
                REQUIRE(recovered.Value()->Activate().HasValue());
                recovered.Value()->Shutdown();
                REQUIRE(recovered.Value()->State() == CharacterWorldState::Destroyed);
            }
            REQUIRE(baseline.Value()->State() == CharacterWorldState::Prepared);
        }

        TEST_CASE("Character prepared structural slot reuse performs no new allocation or deallocation",
                  "[physics][character][qualification][allocation][handles]") {
            auto world = QualificationPreparedWorld(1);
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            std::array<CharacterControllerHandle, 128> handles;
            bool valid = true;
            const auto before = Tests::AllocationProbe::Count();
            const auto freesBefore = Tests::AllocationProbe::FreeCount();
            for (auto &handle : handles) {
                const auto created = world->CreateController(descriptor);
                if (created.HasError()) {
                    valid = false;
                    continue;
                }
                handle = created.Value();
                valid &= world->DestroyController(handle).HasValue();
            }
            const auto after = Tests::AllocationProbe::Count();
            const auto freesAfter = Tests::AllocationProbe::FreeCount();
            REQUIRE(valid);
            REQUIRE(after == before);
            REQUIRE(freesAfter == freesBefore);
            REQUIRE(world->ActiveControllerCount() == 0);
            for (std::size_t index = 0; index < handles.size(); ++index) {
                REQUIRE(handles[index].slot.index == handles.front().slot.index);
                REQUIRE(handles[index].slot.generation == handles.front().slot.generation + index);
                RequireError(world->ControllerDescriptor(handles[index]), CharacterErrors::HandleStale);
            }
            REQUIRE(world->CreateController(descriptor).HasValue());
            REQUIRE(world->ActiveControllerCount() == 1);
        }

        TEST_CASE("Character reused controller generations remain allocation free through repeated placement and ticks",
                  "[physics][character][qualification][allocation][lifecycle]") {
            auto world = QualificationPreparedWorld(1);
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            std::array<CharacterControllerHandle, TickCount> retired;
            for (auto &handle : retired) {
                const auto created = world->CreateController(descriptor);
                REQUIRE(created.HasValue());
                handle = created.Value();
                REQUIRE(world->DestroyController(handle).HasValue());
            }
            const auto current = world->CreateController(descriptor).Value();
            for (const auto stale : retired)
                RequireError(world->ControllerDescriptor(stale), CharacterErrors::HandleStale);
            REQUIRE(current.slot.index == retired.front().slot.index);
            REQUIRE(current.slot.generation == retired.back().slot.generation + 1);
            ClearPlacement placement;
            const auto before = Tests::AllocationProbe::Count();
            const auto freesBefore = Tests::AllocationProbe::FreeCount();
            const auto activated = world->Activate();
            const auto spawned = world->SpawnController(current, placement.Context(world->Descriptor()));
            bool valid = spawned.HasValue() && activated.HasValue();
            valid &= RunTeleportTicks(*world, current, placement);
            const auto after = Tests::AllocationProbe::Count();
            const auto freesAfter = Tests::AllocationProbe::FreeCount();
            REQUIRE(valid);
            REQUIRE(after == before);
            REQUIRE(freesAfter == freesBefore);
            REQUIRE(placement.calls == TickCount + 1);
            REQUIRE(world->ControllerTransform(current).Value().position == Math::Vec3{8, 0, 0});
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{TickCount, TickCount, 0}));
            world->Shutdown();
            world->Shutdown();
            REQUIRE(world->ActiveControllerCount() == 0);
            REQUIRE(world->TickStatistics().pendingCommands == 0);
            RequireError(world->QueueMovementCommand(Movement(current, TickCount + 1, 1)), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character concurrent repeated command frames have deterministic allocation free final publications",
                  "[physics][character][qualification][allocation][thread]") {
            for (const bool reversed : {false, true}) {
                QualificationWorld active;
                ConcurrentProducers::Requests requests;
                for (std::size_t frame = 0; frame < TickCount; ++frame)
                    for (std::size_t controller = 0; controller < ControllerCount; ++controller) {
                        requests[frame][controller] = Movement(active.handles[controller], frame + 1, frame + 1);
                        requests[frame][controller].desiredVelocityMetersPerSecond = Math::Vec3{static_cast<float>(controller + 1), 0, 0};
                    }
                ConcurrentProducers producers{*active.world, requests, reversed};
                if (!producers.Start())
                    SKIP("Standard thread creation is unavailable; concurrent qualification was not executed.");
                const auto before = Tests::AllocationProbe::Count();
                const auto freesBefore = Tests::AllocationProbe::FreeCount();
                const auto summary = AdvanceConcurrentFrames(active, producers, requests, reversed);
                const auto after = Tests::AllocationProbe::Count();
                const auto freesAfter = Tests::AllocationProbe::FreeCount();
                REQUIRE(summary.valid);
                REQUIRE(after == before);
                REQUIRE(freesAfter == freesBefore);
                REQUIRE((active.world->PublishedTick() == CharacterPublishedTick{TickCount, TickCount, ControllerCount}));
                for (std::size_t index = 0; index < ControllerCount; ++index) {
                    const auto snapshot = active.world->ControllerLocomotionSnapshot(active.handles[index]);
                    REQUIRE(snapshot.HasValue());
                    REQUIRE(snapshot.Value().transform.position == Math::Vec3{static_cast<float>(index + 1) * 8.0F, 0, 0});
                    REQUIRE(snapshot.Value().transform.sourceTick == TickCount);
                }
                const auto statistics = active.world->TickStatistics();
                REQUIRE(statistics.pendingCommands == 0);
                REQUIRE(statistics.admittedCommands == TickCount * ControllerCount);
                REQUIRE(statistics.rejectedCommands == summary.repaired);
                REQUIRE(statistics.maximumCommandDepth == ControllerCount);
            }
        }

        TEST_CASE("Character concurrent readers observe the previous publication and future intents wait for their tick",
                  "[physics][character][qualification][thread][publication]") {
            auto active = QualificationSpawnedWorld();
            auto first = Movement(active.controller, 1, 1);
            first.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            auto future = Movement(active.controller, 2, 2);
            future.desiredVelocityMetersPerSecond = Math::Vec3{2, 0, 0};
            REQUIRE(active.world->QueueMovementCommand(first).HasValue());
            const auto previous = active.world->ControllerTransform(active.controller).Value();
            PublicationBoundary boundary;
            PublicationReadback readback;
            std::jthread reader;
            try {
                reader = StartBoundaryReader(*active.world, active.controller, boundary, future, readback);
            } catch (const std::system_error &error) {
                if (error.code() == std::errc::resource_unavailable_try_again || error.code() == std::errc::operation_not_supported)
                    SKIP("Standard thread creation is unavailable; concurrent publication qualification was not executed.");
                throw;
            }
            const auto tick = active.world->AdvanceFixedTick({.tick = 1,
                                                              .sceneGeneration = active.world->Descriptor().sceneGeneration,
                                                              .fixedDelta = Duration::FromNanoseconds(250'000'000),
                                                              .observer = {.context = &boundary, .phase = PublicationBoundary::Phase}});
            reader.join();
            REQUIRE(tick.HasValue());
            REQUIRE((readback.observed == CharacterPublishedTick{}));
            REQUIRE(readback.transform.has_value());
            REQUIRE(readback.transform->HasValue());
            REQUIRE(readback.transform->Value().position == previous.position);
            REQUIRE(readback.transform->Value().publicationRevision == previous.publicationRevision);
            REQUIRE(readback.admission.has_value());
            REQUIRE(readback.admission->HasValue());
            REQUIRE(readback.admission->Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(active.world->ControllerTransform(active.controller).Value().position == Math::Vec3{0.25F, 0, 0});
            REQUIRE(active.world->TickStatistics().pendingCommands == 1);
            REQUIRE(active.world
                        ->AdvanceFixedTick({.tick = 2,
                                            .sceneGeneration = active.world->Descriptor().sceneGeneration,
                                            .fixedDelta = Duration::FromNanoseconds(250'000'000)})
                        .HasValue());
            REQUIRE(active.world->ControllerTransform(active.controller).Value().position == Math::Vec3{0.75F, 0, 0});
            REQUIRE((active.world->PublishedTick() == CharacterPublishedTick{2, 2, 1}));
        }

        TEST_CASE("Character admission remains nonblocking while an actual copied-state reader holds the registry",
                  "[physics][character][qualification][thread][admission]") {
            auto active = QualificationSpawnedWorld();
            auto stale = active.controller;
            ++stale.slot.generation;
            const auto before = Tests::AllocationProbe::Count();
            const auto staleResult = active.world->ControllerDescriptor(stale);
            const auto after = Tests::AllocationProbe::Count();
            RequireError(staleResult, CharacterErrors::HandleStale);
            if (after == before)
                SKIP("Error projection does not allocate on this standard library; registry-held fault observation is unsupported.");
            RegistryContention probe;
            const auto request = Movement(active.controller, 1, 1);
            RegistryProducer producer{*active.world, request, probe};
            if (!producer.Start())
                SKIP("Standard thread creation is unavailable; registry-contention qualification was not executed.");
            const bool injected = ObserveRegistryHold(*active.world, stale, probe);
            producer.Finish();
            REQUIRE(injected);
            REQUIRE(probe.invoked);
            REQUIRE(probe.returnedWhileHeld);
            REQUIRE(probe.after == probe.before);
            REQUIRE(probe.admission.has_value());
            REQUIRE(probe.admission->HasValue());
            REQUIRE(probe.admission->Value().status == CharacterCommandAdmissionStatus::RejectedBusy);
            REQUIRE(active.world->TickStatistics().pendingCommands == 0);
            REQUIRE(active.world->TickStatistics().rejectedCommands == 1);
            REQUIRE((active.world->PublishedTick() == CharacterPublishedTick{}));
            const auto repaired = active.world->QueueMovementCommand(request);
            REQUIRE(repaired.HasValue());
            REQUIRE(repaired.Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE((active.world->PublishedTick() == CharacterPublishedTick{1, 1, 1}));
            REQUIRE(active.world->TickStatistics().admittedCommands == 1);
        }
    }  // namespace
}  // namespace Horo::Character
