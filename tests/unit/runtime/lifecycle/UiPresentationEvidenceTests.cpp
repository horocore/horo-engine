#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "PresentationClockGeneration.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    class PresentationReader final : public RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
            if (phase == RuntimePhase::BeginFrame) {
                total = context.presentationAdmittedDuration;
                generation = context.presentationClockGeneration;
                frame = context.frameNumber;
            }
            if (failure == phase)
                return Result<void>::Failure({ErrorCode{"runtime.test.presentation"},
                                              ErrorDomainId{"runtime.test"},
                                              ErrorSeverity::Error,
                                              "Injected presentation failure.",
                                              {}});
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            ++shutdowns;
        }

        Duration total{};
        std::uint64_t generation{};
        std::uint64_t frame{};
        RuntimePhase failure{RuntimePhase::FixedUpdate};
        unsigned shutdowns{};
    };
}  // namespace

TEST_CASE("Scheduler cumulative presentation evidence preserves unread duration across failed phases", "[runtime][clock]") {
    DeterministicClock clock;
    RuntimeLifecycle lifecycle;
    CancellationSource cancellation;
    auto created = FrameScheduler::Create(clock);
    REQUIRE(created.HasValue());
    auto scheduler = std::move(created).Value();
    auto participant = std::make_unique<PresentationReader>();
    auto *reader = participant.get();
    REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
    REQUIRE(lifecycle.Startup(cancellation.Token()).HasValue());
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
    REQUIRE(reader->total == Duration{});
    SECTION("failure before VariableUpdate") {
        reader->failure = RuntimePhase::NetworkPoll;
    }
    SECTION("failure after VariableUpdate") {
        reader->failure = RuntimePhase::RenderExtraction;
    }
    clock.Advance(Duration::FromMilliseconds(20));
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasError());
    REQUIRE(reader->total == Duration::FromMilliseconds(20));
    reader->failure = RuntimePhase::FixedUpdate;
    clock.Advance(Duration::FromMilliseconds(7));
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
    REQUIRE(reader->total == Duration::FromMilliseconds(27));
    REQUIRE(reader->frame == 3);
    REQUIRE(scheduler->RunFrame(lifecycle, cancellation.Token(), false).HasValue());
    REQUIRE(reader->total == Duration::FromMilliseconds(27));
}

TEST_CASE("Actual host suspension changes presentation baseline without discarding cumulative admission", "[runtime][clock]") {
    DeterministicClock clock;
    auto created = RuntimeHost::Create(clock);
    REQUIRE(created.HasValue());
    auto host = std::move(created).Value();
    auto participant = std::make_unique<PresentationReader>();
    auto *reader = participant.get();
    REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    clock.Advance(Duration::FromMilliseconds(12));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->total == Duration::FromMilliseconds(12));
    REQUIRE(host->Suspend().HasValue());
    clock.Advance(Duration::FromMilliseconds(9000));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->total == Duration::FromMilliseconds(12));
    REQUIRE(host->Resume().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->generation == 3);
    REQUIRE(reader->total == Duration::FromMilliseconds(12));
    clock.Advance(Duration::FromMilliseconds(3));
    REQUIRE(host->RunFrame().HasValue());
    REQUIRE(reader->total == Duration::FromMilliseconds(15));
}

TEST_CASE("Actual cumulative admission helper latches exhaustion and preserves the last producer total", "[runtime][clock]") {
    Duration total = Duration::FromNanoseconds(std::numeric_limits<std::int64_t>::max() - 1);
    bool exhausted = false;
    REQUIRE(Internal::AdmitPresentationDuration(total, Duration::FromNanoseconds(1), exhausted).HasValue());
    REQUIRE(total.ToNanoseconds() == std::numeric_limits<std::int64_t>::max());
    const auto failed = Internal::AdmitPresentationDuration(total, Duration::FromNanoseconds(1), exhausted);
    REQUIRE(failed.HasError());
    REQUIRE(failed.ErrorValue().code.Value() == "runtime.scheduler.presentation_duration_exhausted");
    REQUIRE(exhausted);
    REQUIRE(total.ToNanoseconds() == std::numeric_limits<std::int64_t>::max());
    REQUIRE(Internal::AdmitPresentationDuration(total, {}, exhausted).HasError());
    REQUIRE(total.ToNanoseconds() == std::numeric_limits<std::int64_t>::max());
}
