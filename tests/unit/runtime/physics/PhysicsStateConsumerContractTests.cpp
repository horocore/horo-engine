#include "AllocationProbe.h"
#include "Horo/Network/PacketBuffer.h"
#include "PhysicsStateSaveContract.h"
#include "PhysicsStateWorldTestSupport.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace Horo::Physics::StateTest {
    namespace {
        /** @brief Actual already captured native lease, copied verbatim through the existing canonical Save sink. */
        class NativePhysicsCapture final : public Runtime::ICanonicalStateAdapter {
        public:
            explicit NativePhysicsCapture(PhysicsStateSnapshotLease snapshot) : snapshot_(std::move(snapshot)) {}

            Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &,
                                                                 Runtime::ICanonicalCaptureSink &sink) const override {
                using Outcome = Result<Runtime::CanonicalCaptureDisposition>;
                if (!snapshot_.IsValid())
                    return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
                if (auto copied = sink.WriteCopied(Detail::PhysicsStateSaveRecord(), snapshot_->Bytes()); copied.HasError())
                    return Outcome::Failure(copied.ErrorValue());
                return Outcome::Success(Runtime::CanonicalCaptureDisposition::Captured);
            }

        private:
            PhysicsStateSnapshotLease snapshot_;
        };

        TEST_CASE("Actual native Physics rollback lease Save record and Network packet share one exact canonical payload",
                  "[physics][native][state][consumer][save][network]") {
            WorldFixture fixture;
            static_cast<void>(fixture.Configure());
            auto checkpoint = fixture.Capture();
            const auto rollback = checkpoint;
            const std::vector<std::byte> original(checkpoint->Bytes().begin(), checkpoint->Bytes().end());
            auto packets = Take(Network::PacketBufferPool::Create({1, original.size(), 256}));
            Runtime::CanonicalStateParticipantRegistry registry;
            const auto registered =
                registry.Register(Detail::PhysicsStateSaveDescriptor(), std::make_shared<NativePhysicsCapture>(checkpoint));
            REQUIRE(registered.HasValue());
            const auto participants = Take(registry.Snapshot());
            const auto provenance = Runtime::CaptureTestSupport::Provenance(participants);
            auto builder = Take(Runtime::RuntimeSaveCaptureBuilder::Create(provenance, participants));
            Check(builder.CaptureParticipants());
            const auto saved = Take(builder.Seal());
            REQUIRE(saved.Records().size() == 1);
            CHECK(saved.Records()[0].Record().record == Detail::PhysicsStateSaveRecord());
            CHECK(std::ranges::equal(saved.Records()[0].Segment(0), original));
            Network::PacketBuffer packet;
            Tests::AllocationProbe::Measurement measured;
            bool retained{};
            {
                Tests::AllocationProbe::ScopedMeasurement scope;
                auto acquired = packets.Acquire(checkpoint->Bytes());
                retained = acquired.HasValue();
                if (retained)
                    packet = std::move(acquired).Value();
                measured = scope.Snapshot();
            }
            REQUIRE(retained);
            CHECK(measured.requests == 0);
            CHECK(packets.Outstanding() == 1);
            checkpoint = {};
            fixture.Advance(2);
            CHECK(std::ranges::equal(rollback->Bytes(), original));
            bool unchanged{};
            std::thread consumer([payload = std::move(packet), &unchanged, &original]() mutable {
                unchanged = std::ranges::equal(payload.Bytes(), original);
                payload.Reset();
            });
            consumer.join();
            CHECK(unchanged);
            CHECK(packets.Outstanding() == 0);
            const auto lastGood = fixture.world->PublishedTick();
            auto candidate = Take(fixture.world->PrepareStateRestore(rollback));
            CHECK(SamePublication(fixture.world->PublishedTick(), lastGood));
            Check(candidate.ValidatePublication());
            candidate.Publish();
            CHECK(fixture.world->PublishedTick().completedTick == rollback->World().completedTick);
            CHECK(std::ranges::equal(Take(fixture.world->CaptureState())->Bytes(), original));
            // This is actual native same-topology rewind plus real Save/Network byte ownership.
            // It does not advertise a structural-history rollback driver, network authority or transport send capability.
        }
    }  // namespace
}  // namespace Horo::Physics::StateTest
