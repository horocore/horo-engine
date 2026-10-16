#include "navigation/NavigationRuntimeQualificationFixture.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <iostream>

namespace Horo::Navigation {
    namespace {
        using namespace Qualification;
        using TestSupport::RequireError;

        NavigationPathCaller Caller(const std::uint64_t id) {
            return {Id<NavigationDynamicOwnerId>(id), Id<NavigationDynamicOwnerGeneration>(1)};
        }

        NavigationOutcomeProvenance Source(const NavigationGeneration topology = Topology(), const NavigationWorldId worldId = World()) {
            return {Id<NavigationSnapshotToken>(1), worldId, topology, 1, 1, 1, 1, 0};
        }

        NavigationPathSubmission Submission(const NavigationPathCaller caller) {
            auto request = Request();
            request.start = {1, RootHeight, 5};
            request.destination = {9, RootHeight, 5};
            return {.caller = caller,
                    .request = request,
                    .source = Source(),
                    .capabilityRevision = 1,
                    .targetTick = 1,
                    .deadlineTick = 128};
        }

        struct ServiceJob final {
            JobId id{};
            std::size_t wave{};
            std::int64_t elapsedNanoseconds{};
        };

        /** @brief Correlate real Foundation records to a dispatch wave without weakening stable-prefix publication. */
        void AwaitServiceFence(JobSystem &jobs, std::array<ServiceJob, 32> &service, std::size_t &count, const std::size_t wave) {
            const auto snapshot = jobs.SnapshotIfChanged(0);
            REQUIRE(snapshot);
            REQUIRE(snapshot->droppedTerminalCount == 0);
            const auto first = count;
            for (const auto &record : snapshot->jobs) {
                const auto prior =
                    std::find_if(service.begin(), service.begin() + static_cast<std::ptrdiff_t>(count), [&](const ServiceJob &job) {
                    return job.id == record.id;
                });
                if (prior != service.begin() + static_cast<std::ptrdiff_t>(count))
                    continue;
                REQUIRE(count < service.size());
                service[count++] = {record.id, wave};
            }
            REQUIRE(count > first);
            REQUIRE(count - first <= QueryLimits().maximumJobs);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            bool terminal{};
            do {
                terminal = true;
                for (std::size_t index = first; index < count; ++index) {
                    const auto record = jobs.Find(service[index].id);
                    REQUIRE(record);
                    terminal = terminal && record->terminalResult.has_value();
                    if (record->terminalResult) {
                        REQUIRE(record->state == JobState::Succeeded);
                        REQUIRE(record->timing.startedAt);
                        REQUIRE(record->timing.finishedAt);
                        REQUIRE(*record->timing.finishedAt >= *record->timing.startedAt);
                        service[index].elapsedNanoseconds =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(*record->timing.finishedAt - record->timing.submittedAt)
                                .count();
                    }
                }
                if (terminal)
                    break;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            REQUIRE(terminal);
        }

        /** @brief Caller-owned observations survive every submitted job and its terminal fence. */
        struct PressureEvidence final {
            std::array<NavRequestHandle, 48> handles{};
            Tests::AllocationProbe::Measurement admissionAllocation{};
            std::chrono::steady_clock::time_point start;
            std::array<std::uint32_t, 3> served{};
            std::uint32_t completed{};
            std::array<std::int64_t, 48> completionLatency{};
            std::array<bool, 48> consumed{};
            std::array<ServiceJob, 32> serviceJobs{};
            std::size_t jobCount{};
            std::array<std::array<std::uint32_t, 3>, 8> serviceCounts{};
        };

        /** @brief Admit the unchanged adversarial caller groups while measuring only API admission. */
        void AdmitGrouped(NavigationCoordinator &coordinator, NavigationWorldLifecycle &world,
                          const std::array<NavigationPathCaller, 3> &callers, PressureEvidence &evidence) {
            evidence.start = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < evidence.handles.size(); ++index) {
                const auto input = Submission(callers[index / 16]);
                const auto admitted = [&] {
                    Tests::AllocationProbe::ScopedMeasurement window;
                    auto result = coordinator.Submit(world, input, 0);
                    const auto measured = window.Snapshot();
                    evidence.admissionAllocation.requests += measured.requests;
                    evidence.admissionAllocation.requestedBytes += measured.requestedBytes;
                    evidence.admissionAllocation.largestRequest =
                        std::max(evidence.admissionAllocation.largestRequest, measured.largestRequest);
                    return result;
                }();
                REQUIRE(admitted.HasValue());
                evidence.handles[index] = admitted.Value();
            }
            RequireError(coordinator.Submit(world, Submission(callers[0]), 0), NavigationErrors::AdmissionRejected);
        }

        /** @brief Match each exactly-once public result to its actual Foundation job and caller partition. */
        void ConsumePublished(NavigationCoordinator &coordinator, const std::array<NavigationPathCaller, 3> &callers,
                              PressureEvidence &evidence) {
            for (std::size_t index = 0; index < evidence.handles.size(); ++index) {
                auto result = coordinator.Take(evidence.handles[index]);
                if (!result)
                    continue;
                REQUIRE_FALSE(evidence.consumed[index]);
                evidence.consumed[index] = true;
                REQUIRE(result->result.HasValue());
                CHECK(result->result.Value().status == NavigationPathStatus::Reachable);
                CHECK(result->source.snapshot == Source().snapshot);
                CHECK(result->source.world == World());
                CHECK(result->source.topology == Topology());
                CHECK(result->source.obstacleRevision == 1);
                CHECK(result->source.filterRevision == 1);
                CHECK(result->source.profileRevision == 1);
                CHECK(result->source.originRevision == 1);
                CHECK(result->caller == callers[index / 16]);
                CHECK(result->job != 0);
                CHECK(result->result.Value().sourceGeneration == Topology());
                REQUIRE_FALSE(result->result.Value().waypoints.empty());
                CHECK(result->result.Value().waypoints.back().position.y == RootHeight);
                const auto service =
                    std::find_if(evidence.serviceJobs.begin(),
                                 evidence.serviceJobs.begin() + static_cast<std::ptrdiff_t>(evidence.jobCount), [&](const ServiceJob &job) {
                    return job.id == result->job;
                });
                REQUIRE(service != evidence.serviceJobs.begin() + static_cast<std::ptrdiff_t>(evidence.jobCount));
                REQUIRE(service->wave < evidence.serviceCounts.size());
                ++evidence.serviceCounts[service->wave][index / 16];
                ++evidence.served[index / 16];
                REQUIRE(evidence.completed < evidence.completionLatency.size());
                evidence.completionLatency[evidence.completed++] =
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - evidence.start).count();
            }
        }

        /** @brief Prove per-caller service independently from the unchanged stable publication prefix. */
        void RunFairService(JobSystem &jobs, NavigationCoordinator &coordinator, const std::array<NavigationPathCaller, 3> &callers,
                            PressureEvidence &evidence) {
            for (std::uint64_t tick = 1; tick <= 8; ++tick) {
                const auto dispatched = coordinator.Dispatch(tick);
                REQUIRE(dispatched == 6);
                REQUIRE(coordinator.Dispatch(tick) == 0);
                AwaitServiceFence(jobs, evidence.serviceJobs, evidence.jobCount, tick - 1);
                coordinator.CollectRetiredAtSafePoint();
                const auto published = coordinator.Commit({Activation(), Source(), callers, tick});
                // Earlier caller-1 records fence later callers despite their earlier fair service.
                REQUIRE(published == (tick < 8 ? 2 : 34));
                ConsumePublished(coordinator, callers, evidence);
            }
        }

        /** @brief Join jobs, prove complete bounded drain, and report advisory service/publication measurements. */
        void RequireCompleteAndReport(JobSystem &jobs, NavigationCoordinator &coordinator, PressureEvidence &evidence) {
            for (const auto &wave : evidence.serviceCounts)
                for (const auto count : wave)
                    CHECK(count == 2);
            REQUIRE(evidence.completed == evidence.handles.size());
            for (const auto handle : evidence.handles)
                REQUIRE_FALSE(coordinator.Take(handle));
            for (const auto count : evidence.served)
                CHECK(count == 16);
            jobs.Shutdown(ShutdownPolicy::Drain);
            coordinator.CollectRetiredAtSafePoint();
            REQUIRE(coordinator.IsDrained());
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - evidence.start);
            std::sort(evidence.completionLatency.begin(), evidence.completionLatency.end());
            std::array<std::int64_t, 32> serviceLatency{};
            REQUIRE(evidence.jobCount > 0);
            for (std::size_t index = 0; index < evidence.jobCount; ++index)
                serviceLatency[index] = evidence.serviceJobs[index].elapsedNanoseconds;
            std::sort(serviceLatency.begin(), serviceLatency.begin() + static_cast<std::ptrdiff_t>(evidence.jobCount));
            std::cout << "NAV-004.12 profile=RuntimeQualificationSmall admission=grouped accepted=48 callers=3 per_caller_per_tick=2"
                      << " serviced_ticks=8 end_to_end_owner_us=" << elapsed.count()
                      << " completion_p50_us=" << evidence.completionLatency[24] << " completion_p95_us=" << evidence.completionLatency[44]
                      << " native_partition_samples=" << evidence.jobCount
                      << " native_partition_service_p50_ns=" << serviceLatency[evidence.jobCount / 2]
                      << " native_partition_service_p95_ns=" << serviceLatency[(evidence.jobCount - 1) * 95 / 100]
                      << " build=" << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                      << " compiler=" << HORO_QUALIFICATION_COMPILER
                      << " admission_cpp_allocations=" << evidence.admissionAllocation.requests
                      << " admission_cpp_bytes=" << evidence.admissionAllocation.requestedBytes
                      << " largest_admission_cpp_request=" << evidence.admissionAllocation.largestRequest
                      << " native_and_worker_allocations=unobserved\n";
        }

        /** @brief Reject post-shutdown admission and prove the queued handle releases its world without publication. */
        void RequireQueuedShutdown(NavigationWorldLifecycle &world, const NavigationWorldActivationDescriptor &activation,
                                   const std::array<NavigationPathCaller, 1> &callers) {
            JobSystem jobs{{.workerCount = 0}};
            auto coordinator = NavigationCoordinator::Create(jobs, QueryLimits()).Value();
            const auto queued = coordinator.Submit(world,
                                                   {.caller = callers[0],
                                                    .request = Request(activation.world, activation.topology),
                                                    .source = Source(activation.topology, activation.world),
                                                    .capabilityRevision = 1,
                                                    .targetTick = 2,
                                                    .deadlineTick = 128},
                                                   1);
            REQUIRE(queued.HasValue());
            coordinator.BeginShutdown();
            world.BeginShutdown();
            RequireError(coordinator.Submit(world, Submission(callers[0]), 2), NavigationErrors::AdmissionRejected);
            REQUIRE(coordinator.Abandon(queued.Value()));
            coordinator.CollectRetiredAtSafePoint();
            CHECK(coordinator.IsDrained());
            CHECK_FALSE(coordinator.Take(queued.Value()));
            CHECK(world.CollectRetired() == NavigationWorldLifecycleState::Closed);
            CHECK(world.RetiredCount() == 0);
        }

    }  // namespace

    TEST_CASE("Native query pressure preserves bounded rotating caller service and reports phase measurements",
              "[integration][navigation][qualification][native][fairness]") {
        auto world = NavigationWorldLifecycle::Create(4).Value();
        REQUIRE(world.Stage(Activation(), Provider()).HasValue());
        REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
        JobSystem jobs{{.workerCount = 1}};
        auto coordinator = NavigationCoordinator::Create(jobs, QueryLimits()).Value();
        const std::array callers{Caller(1), Caller(2), Caller(3)};
        PressureEvidence evidence;
        AdmitGrouped(coordinator, world, callers, evidence);
        RunFairService(jobs, coordinator, callers, evidence);
        RequireCompleteAndReport(jobs, coordinator, evidence);
        coordinator.BeginShutdown();
        world.BeginShutdown();
        CHECK(world.CollectRetired() == NavigationWorldLifecycleState::Closed);
        CHECK(world.RetiredCount() == 0);
    }

    TEST_CASE("Cancellation before and after real native dispatch is terminal once and releases generations",
              "[integration][navigation][qualification][native][cancellation]") {
        for (const bool dispatched : {false, true}) {
            auto world = NavigationWorldLifecycle::Create(4).Value();
            REQUIRE(world.Stage(Activation(), Provider()).HasValue());
            REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
            JobSystem jobs{{.workerCount = 0}};
            auto coordinator = NavigationCoordinator::Create(jobs, QueryLimits()).Value();
            const std::array callers{Caller(1)};
            const auto handle = coordinator.Submit(world, Submission(callers[0]), 0).Value();
            if (dispatched)
                REQUIRE(coordinator.Dispatch(1) == 1);
            REQUIRE(coordinator.Cancel(handle));
            jobs.Shutdown(ShutdownPolicy::Drain);
            REQUIRE(coordinator.Commit({Activation(), Source(), callers, 1}) == 1);
            auto result = coordinator.Take(handle);
            REQUIRE(result);
            RequireError(result->result, NavigationErrors::QueryCancelled);
            REQUIRE_FALSE(coordinator.Take(handle));
            REQUIRE_FALSE(coordinator.IsRetained(handle));
            REQUIRE(coordinator.IsDrained());
            const auto next = coordinator.Submit(world, Submission(callers[0]), 1);
            REQUIRE(next.HasValue());
            CHECK(next.Value().slot.index == handle.slot.index);
            CHECK(next.Value().slot.generation > handle.slot.generation);
            CHECK_FALSE(coordinator.Cancel(handle));
            REQUIRE(coordinator.Abandon(next.Value()));
            coordinator.CollectRetiredAtSafePoint();
            CHECK(coordinator.IsDrained());
        }
    }

    TEST_CASE("Topology replacement rejects completed native corridors and shutdown preserves only explicit external leases",
              "[integration][navigation][qualification][native][lifecycle]") {
        auto world = NavigationWorldLifecycle::Create(4).Value();
        auto activation = Activation();
        REQUIRE(world.Stage(activation, Provider()).HasValue());
        REQUIRE(world.CommitAtSafePoint(activation.scene, activation.sceneGeneration).HasValue());
        const std::array callers{Caller(1)};
        // Eight replacements cross the four-retired-world preparation bound;
        // each cycle must reclaim the previous generation before the next stage.
        for (std::uint64_t replacement = 0; replacement < 8; ++replacement) {
            auto external = world.Acquire(activation.world).Value();
            JobSystem jobs{{.workerCount = 0}};
            auto coordinator = NavigationCoordinator::Create(jobs, QueryLimits()).Value();
            auto pending = Submission(callers[0]);
            pending.request.world = activation.world;
            pending.request.topology = activation.topology;
            pending.source = Source(activation.topology, activation.world);
            const auto handle = coordinator.Submit(world, pending, 0).Value();
            REQUIRE(coordinator.Dispatch(1) == 1);
            jobs.Shutdown(ShutdownPolicy::Drain);
            activation.world = World(activation.world.Value() + 1);
            activation.topology = Topology(activation.topology.Value() + 1);
            REQUIRE(world.Stage(activation, Provider(activation.topology, replacement % 2 == 0, activation.world)).HasValue());
            REQUIRE(world.CommitAtSafePoint(activation.scene, activation.sceneGeneration).HasValue());
            CHECK(external.IsRevoked());
            REQUIRE(coordinator.Commit({activation, Source(activation.topology, activation.world), callers, 1}) == 1);
            const auto stale = coordinator.Take(handle);
            REQUIRE(stale);
            RequireError(stale->result, NavigationErrors::InvalidWorld);
            REQUIRE_FALSE(coordinator.Take(handle));
            REQUIRE(coordinator.IsDrained());
            CHECK(world.RetiredCount() == 1);
            external = {};
            CHECK(world.CollectRetired() == NavigationWorldLifecycleState::Active);
            CHECK(world.RetiredCount() == 0);
        }
        RequireQueuedShutdown(world, activation, callers);
    }
}  // namespace Horo::Navigation
