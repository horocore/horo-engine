#include "AllocationProbe.h"
#include "ReplicationCaptureTestSupport.h"

namespace Horo::Network {
    using namespace CaptureTestSupport;

    namespace {
        struct ScheduledFixture final {
            ReplicationWorldLifecycle lifecycle{Lifecycle()};
            std::shared_ptr<CountingCodec> codec{std::make_shared<CountingCodec>()};
            std::array<std::shared_ptr<Owner>, 6> owners;
            std::unique_ptr<ReplicationStateCapture> capture;

            ScheduledFixture() {
                std::array<ReplicationCaptureTarget, 6> targets;
                for (std::size_t index{}; index < targets.size(); ++index) {
                    owners[index] = std::make_shared<Owner>();
                    targets[index] = {Object(index + 1), owners[index]};
                    REQUIRE(lifecycle.RegisterObject(World().scene, World().session, targets[index].object).HasValue());
                }
                ReplicationCaptureLimits limits;
                limits.maximumTargetsPerTick = 2;
                capture = std::move(ReplicationStateCapture::Prepare(Read(lifecycle), Registry(codec), targets, limits)).Value();
            }

            Result<ReplicationCaptureReport> Tick(const std::uint64_t tick) {
                for (const auto &owner : owners)
                    owner->Commit(tick, static_cast<double>(tick));
                const auto read =
                    lifecycle.AcquireCaptureRead({World().scene, World().session, Runtime::RuntimePhase::NetworkFlush, tick, {}});
                if (read.HasError())
                    return Result<ReplicationCaptureReport>::Failure(read.ErrorValue());
                return capture->CaptureAtCommit(read.Value(), tick);
            }
        };
    }  // namespace

    TEST_CASE("Dirty hints advance distant targets before ordinary reconciliation reaches them", "[network][capture][hints]") {
        ScheduledFixture fixture;
        REQUIRE(fixture.capture->MarkDirty(Object(6).object).HasValue());
        const auto report = fixture.Tick(1);
        REQUIRE(report.HasValue());
        REQUIRE(report.Value().considered == 2);
        REQUIRE(fixture.owners[0]->captures == 1);
        REQUIRE(fixture.owners[5]->captures == 1);
        REQUIRE(fixture.owners[1]->captures == 0);
    }

    TEST_CASE("Duplicate dirty hints enqueue once and cannot capture the same target twice in a tick", "[network][capture][hints]") {
        ScheduledFixture fixture;
        for (std::size_t count{}; count < 100; ++count)
            REQUIRE(fixture.capture->MarkDirty(Object(6).object).HasValue());
        REQUIRE(fixture.Tick(1).Value().considered == 2);
        REQUIRE(fixture.owners[5]->captures == 1);
        REQUIRE(fixture.Tick(2).Value().considered == 2);
        REQUIRE(fixture.owners[5]->captures == 1);
        REQUIRE(fixture.capture->MarkDirty(Object(4).object).HasValue());
        REQUIRE(fixture.Tick(3).Value().considered == 2);
        REQUIRE(fixture.owners[3]->captures == 1);
    }

    TEST_CASE("Reserved reconciliation recovers lost hints under continuous dirty target load", "[network][capture][hints]") {
        ScheduledFixture fixture;
        for (std::uint64_t tick = 1; tick <= 6; ++tick) {
            REQUIRE(fixture.capture->MarkDirty(Object(6).object).HasValue());
            REQUIRE(fixture.Tick(tick).Value().considered <= 2);
        }
        for (const auto &owner : fixture.owners)
            REQUIRE(owner->captures > 0);
        for (std::size_t index{}; index < fixture.owners.size(); ++index)
            REQUIRE(fixture.capture->Latest(Object(index + 1).object).HasValue());
    }

    TEST_CASE("Dirty queue admission service and reconciliation allocate and reclaim no hot storage", "[network][capture][allocation]") {
        ScheduledFixture fixture;
        REQUIRE(fixture.Tick(1).HasValue());
        const auto before = Tests::AllocationProbe::Count();
        const auto frees = Tests::AllocationProbe::FreeCount();
        bool passed = true;
        for (std::uint64_t tick = 2; tick <= 64; ++tick) {
            const auto hint = fixture.capture->MarkDirty(Object(6).object);
            const auto report = fixture.Tick(tick);
            if (hint.HasError() || report.HasError() || report.Value().failed > 0) {
                passed = false;
                break;
            }
        }
        const auto after = Tests::AllocationProbe::Count();
        const auto reclaimed = Tests::AllocationProbe::FreeCount();
        REQUIRE(passed);
        REQUIRE(after == before);
        REQUIRE(reclaimed == frees);
    }

    TEST_CASE("Throwing owner and codec callbacks become typed failures and preserve the prior pin", "[network][capture][fault]") {
        Fixture fixture;
        fixture.Capture(1, 4.0);
        const auto baseline = fixture.Pin();
        std::uint64_t tick{2};
        using enum CallbackThrow;
        for (const auto mode : {Allocation, Unexpected, Foreign}) {
            const auto &expected = mode == Allocation ? ReplicationCaptureErrors::Capacity : ReplicationCaptureErrors::CallbackFault;
            fixture.owner->throwBegin = mode;
            const auto beginFailure = fixture.Capture(tick++);
            REQUIRE(beginFailure.firstError->code.Value() == expected.code.Value());
            fixture.owner->throwBegin = CallbackThrow::None;
            fixture.owner->throwCapture = mode;
            const auto captureFailure = fixture.Capture(tick++);
            REQUIRE(captureFailure.firstError->code.Value() == expected.code.Value());
            fixture.owner->throwCapture = CallbackThrow::None;
            fixture.codec->onCompare = [mode] {
                ThrowCallback(mode);
            };
            const auto codecFailure = fixture.Capture(tick++);
            const auto &codecExpected =
                mode == Allocation ? NetworkErrors::ReplicationSerializerCapacityExceeded : ReplicationCaptureErrors::CallbackFault;
            REQUIRE(codecFailure.firstError->code.Value() == codecExpected.code.Value());
            fixture.codec->onCompare = nullptr;
            REQUIRE(fixture.Pin() == baseline);
            REQUIRE_FALSE(fixture.owner->reading);
            REQUIRE(fixture.owner->begins == fixture.owner->ends);
        }
        REQUIRE(fixture.Capture(tick, 9.0).published == 1);
    }
}  // namespace Horo::Network
