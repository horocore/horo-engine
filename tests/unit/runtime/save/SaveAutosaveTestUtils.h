#pragma once

#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <array>
#include <exception>

namespace Horo::Runtime::AutosaveTestSupport {
    using CaptureTestSupport::CallbackCaptureAdapter;
    using CaptureTestSupport::CaptureCallback;
    using CaptureTestSupport::Descriptor;
    using CaptureTestSupport::Participant;
    using CaptureTestSupport::Provenance;
    using CaptureTestSupport::Register;

    class HostClockFailure final : public std::exception {
    public:
        const char *what() const noexcept override {
            return "host clock failed";
        }
    };

    struct AdapterFailure final {};

    enum class HostClockException {
        Standard,
        Foreign,
        Integer
    };

    inline Duration Ns(const std::int64_t value) {
        return Duration::FromNanoseconds(value);
    }

    class TestClock final : public Clock {
    public:
        Duration now;
        bool throws{};
        HostClockException exception{HostClockException::Standard};

        [[nodiscard]] Duration MonotonicNow() const override {
            if (throws && exception == HostClockException::Integer)
                throw 42;
            if (throws && exception == HostClockException::Foreign)
                throw AdapterFailure{};
            if (throws)
                throw HostClockFailure{};
            return now;
        }
    };

    struct Fixture final {
        TestClock clock;
        CanonicalStateParticipantRegistry registry;
        SaveOperationArbiter arbiter{CreateSaveOperationArbiter({.maximumRetainedOperations = 8}).Value()};
        std::unique_ptr<SaveCaptureBarrier> barrier;
        SaveParticipantRegistrySnapshot participants;
        RuntimeSaveCaptureProvenance provenance;
        SaveRuntimeGeneration generation;
        std::array<std::byte, 2> bytes{std::byte{1}, std::byte{2}};
        CaptureCallback callback;
        unsigned captures{};
        std::unique_ptr<SaveAutosaveScheduler> scheduler;

        explicit Fixture(const SaveAutosavePolicy &policy = {.interval = Ns(100), .cooldown = {}},
                         const SaveCaptureBarrierPolicy barrierPolicy = {}) {
            barrier = SaveCaptureBarrier::Create(71, clock, 4, barrierPolicy).Value();
            const std::array names{"project.capture", "horo.jobs", "horo.scene.mutation", "horo.subsystem"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                REQUIRE(barrier->Register(Participant(names[index]), static_cast<SaveBarrierDomain>(index)).HasValue());
                REQUIRE(barrier->PublishReadiness(index, {.value = 41}).HasValue());
            }
            auto adapter =
                std::make_shared<CallbackCaptureAdapter>([this](const CanonicalCaptureContext &context, ICanonicalCaptureSink &sink) {
                ++captures;
                if (callback)
                    return callback(context, sink);
                if (const auto copied = sink.WriteCopied(Test::Id<SaveRecordId>(1), bytes); copied.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(copied.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }, std::make_shared<int>());
            Register(registry, Descriptor("project.capture", {Test::Id<SaveRecordId>(1)}), std::move(adapter));
            participants = registry.Snapshot().Value();
            provenance = Provenance(participants);
            generation = {.runtime = 3, .scene = provenance.sceneIncarnation, .registry = participants.Generation()};
            scheduler = SaveAutosaveScheduler::Create(policy, {.generation = generation}, arbiter, *barrier).Value();
        }

        SaveArbiterAddress Address() const {
            return {.nameSpace = {.product = Test::Id<ProductStorageId>(1),
                                  .environment = Test::Id<EnvironmentStorageId>(2),
                                  .owner = ServerWorldOwner{.owner = Test::Id<ServerStorageOwnerId>(3)}},
                    .slot = Test::Id<SaveGameSlotId>(1)};
        }

        SaveOperationDescriptor Operation(const OperationId id) const {
            return {.operation = id, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 2};
        }

        void Sample(const std::int64_t gameplay, const std::int64_t realtime = -1,
                    const SaveAutosaveActivity activity = SaveAutosaveActivity::Active) {
            clock.now = Ns(realtime < 0 ? gameplay : realtime);
            REQUIRE(scheduler->Sample({generation, Ns(gameplay), clock.now, activity}).HasValue());
        }

        SaveAutosaveSchedulerSnapshot Snapshot() const {
            const auto value = scheduler->Snapshot();
            REQUIRE(value.HasValue());
            return value.Value();
        }

        Result<std::optional<SaveAutosaveCapture>> Poll(const OperationId id = 91) {
            return scheduler->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, Operation(id), Address(),
                                                provenance, participants);
        }

        SaveAutosaveCapture Capture(const OperationId id = 91) {
            auto result = Poll(id);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            return *std::move(result).Value();
        }

        void Complete(const OperationId id) {
            REQUIRE(arbiter.Advance(id, SaveArbiterState::Committing, {1, 1}).HasValue());
            REQUIRE(arbiter.Complete(id).HasValue());
        }
    };
}  // namespace Horo::Runtime::AutosaveTestSupport
