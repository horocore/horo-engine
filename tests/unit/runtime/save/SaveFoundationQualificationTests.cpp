#include "Horo/Runtime/Save/SaveSafePointCoordinator.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace CaptureTestSupport;

        class SnapshotExecutor final : public ISaveSafePointExecutor {
        public:
            explicit SnapshotExecutor(SaveParticipantRegistrySnapshot participants) : participants_(std::move(participants)) {}

            Result<void> Capture(const OperationId operation, const SaveRuntimeGeneration generation) override {
                auto provenance = Provenance(participants_);
                provenance.capturedState = Test::Id<CapturedStateId>(static_cast<std::uint8_t>(operation));
                provenance.sceneIncarnation = generation.scene;
                auto created = RuntimeSaveCaptureBuilder::Create(provenance, participants_);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto builder = std::move(created).Value();
                if (auto captured = builder.CaptureParticipants(); captured.HasError())
                    return captured;
                auto sealed = builder.Seal();
                if (sealed.HasError())
                    return Result<void>::Failure(sealed.ErrorValue());
                snapshot = std::move(sealed).Value();
                return Result<void>::Success();
            }

            Result<void> Restore(OperationId, SaveRuntimeGeneration) override {
                return Result<void>::Failure(MakeError(SaveErrors::LifecycleCallbackFailed));
            }

            void ReleaseRegistry() {
                participants_ = {};
            }

            std::optional<RuntimeSaveSnapshot> snapshot;

        private:
            SaveParticipantRegistrySnapshot participants_;
        };

        TEST_CASE("Headless safe-point capture survives scene replacement without a borrowed payload or stale callback",
                  "[unit][save][qualification]") {
            auto destructionCount = std::make_shared<int>(0);
            auto liveBytes = std::make_shared<std::vector<std::byte>>(std::initializer_list<std::byte>{std::byte{0x31}, std::byte{0x32}});
            CanonicalStateParticipantRegistry registry;
            const SaveRecordId record = Test::Id<SaveRecordId>(31);
            Register(registry, Descriptor("game.player.state", {record}),
                     std::make_shared<
                         CallbackCaptureAdapter>([liveBytes, record](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                const Result<void> written = sink.WriteCopied(record, *liveBytes);
                if (written.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }, destructionCount));
            auto participants = registry.Snapshot().Value();
            const SaveRuntimeGeneration generation{.runtime = 1, .scene = 7, .registry = participants.Generation()};
            auto created = SaveSafePointCoordinator::Create(generation, 2);
            REQUIRE(created.HasValue());
            auto coordinator = std::move(created).Value();
            SnapshotExecutor executor{participants};
            REQUIRE(coordinator->Admit({.operation = 31, .action = SaveSafePointAction::Capture, .generation = generation}).HasValue());
            const auto drain = coordinator->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, 1, executor);
            REQUIRE(drain.HasValue());
            REQUIRE(drain.Value().captured == 1);
            REQUIRE(executor.snapshot.has_value());
            REQUIRE(executor.snapshot->IsValid());
            REQUIRE(executor.snapshot->Provenance().sceneIncarnation == 7);
            (*liveBytes)[0] = std::byte{0xff};
            const SaveRuntimeGeneration next{.runtime = 1, .scene = 8, .registry = generation.registry};
            REQUIRE(coordinator->TransitionScene(next).HasValue());
            registry.Close();
            executor.ReleaseRegistry();
            participants = {};

            std::optional<Result<void>> completion;
            bool observedDetachedBytes{};
            std::thread worker([&] {
                observedDetachedBytes = executor.snapshot->Records().front().Segment(0)[0] == std::byte{0x31};
                completion = coordinator->PublishWorkerCompletion(
                    {.operation = 31, .generation = generation, .outcome = SaveWorkerCompletionOutcome::Succeeded});
            });
            worker.join();
            REQUIRE(completion.has_value());
            REQUIRE(completion->HasValue());
            CHECK(observedDetachedBytes);
            CHECK(coordinator->Snapshot(31).Value().state == SaveSafePointOperationState::Completed);
            RequireError(coordinator->PublishWorkerCompletion(
                             {.operation = 31, .generation = generation, .outcome = SaveWorkerCompletionOutcome::Succeeded}),
                         SaveErrors::CompletionInvalid);
            CHECK(*destructionCount == 0);
            executor.snapshot.reset();
            CHECK(*destructionCount == 1);
        }

        /** @brief Measures bounded owner-thread capture handoff for prebuilt immutable gameplay roots. */
        TEST_CASE("Representative gameplay capture handoff evidence", "[.benchmark][save][qualification]") {
            struct WorkloadPart final {
                const char *name;
                std::uint8_t record;
                std::size_t bytes;
            };

            constexpr std::array workload{
                WorkloadPart{"game.scene.entities", 41, 2048 * 32},
                WorkloadPart{"game.physics.motion", 42, 2048 * 64},
                WorkloadPart{"game.player.inventory", 43, 256 * 32},
            };
            auto destructionCount = std::make_shared<int>(0);
            CanonicalStateParticipantRegistry registry;
            for (const auto &part : workload) {
                const SaveRecordId record = Test::Id<SaveRecordId>(part.record);
                auto descriptor = Descriptor(part.name, {record});
                descriptor.limits.maximumPayloadBytes = part.bytes;
                auto payload = std::make_shared<SegmentedTestPayload>(
                    std::vector<std::vector<std::byte>>{std::vector<std::byte>(part.bytes, std::byte{part.record})});
                Register(registry, std::move(descriptor),
                         std::make_shared<
                             CallbackCaptureAdapter>([record, payload](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                    const Result<void> written = sink.WriteImmutable(record, payload);
                    if (written.HasError())
                        return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                    return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
                }, destructionCount));
            }
            const auto participants = registry.Snapshot().Value();
            RuntimeSaveCaptureLimits limits{.maximumParticipants = 3,
                                            .maximumRecords = 3,
                                            .maximumSegments = 3,
                                            .maximumPayloadBytes = 2048 * 32 + 2048 * 64 + 256 * 32};
            std::vector<std::chrono::nanoseconds> durations;
            durations.reserve(201);
            for (std::size_t iteration = 0; iteration < 201; ++iteration) {
                const auto start = std::chrono::steady_clock::now();
                auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants, 44), participants, limits).Value();
                REQUIRE(builder.CaptureParticipants().HasValue());
                const auto snapshot = builder.Seal().Value();
                const auto elapsed = std::chrono::steady_clock::now() - start;
                REQUIRE(snapshot.IsValid());
                REQUIRE(snapshot.PayloadByteLength() == limits.maximumPayloadBytes);
                if (iteration != 0)
                    durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
            }
            std::ranges::sort(durations);
            const auto micros = [](const std::chrono::nanoseconds value) {
                return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
            };
            std::cout << "save capture handoff: 2048 scene entities x 32B, 2048 physics states x 64B, "
                         "256 inventory entries x 32B; 200 measured iterations; 200KiB total; "
                         "p50="
                      << micros(durations[99]) << "us p95=" << micros(durations[189]) << "us max=" << micros(durations.back())
                      << "us; budget p95<=2000us\n";
        }
    }  // namespace
}  // namespace Horo::Runtime
