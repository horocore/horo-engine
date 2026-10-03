#include "SaveCaptureSnapshotTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace CaptureTestSupport;

        TEST_CASE("Empty optional capture publishes omission without payload records", "[unit][save][capture]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            const SaveRecordId record = Test::Id<SaveRecordId>(26);
            Register(registry, Descriptor("project.capture.edge_optional", {record}, false), destructionCount);
            const SaveParticipantRegistrySnapshot participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants, 94), participants).Value();

            const RuntimeSaveSnapshot snapshot = builder.Seal().Value();
            REQUIRE(snapshot.Records().empty());
            REQUIRE(snapshot.Participants().size() == 1);
            REQUIRE(snapshot.Participants().front().disposition == CanonicalCaptureDisposition::Omitted);
        }

        TEST_CASE("Invalid participant disposition cannot silently omit a captured record", "[unit][save][capture][hostile]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            const SaveRecordId record = Test::Id<SaveRecordId>(27);
            Register(registry, Descriptor("project.capture.invalid_disposition", {record}, false),
                     std::make_shared<CallbackCaptureAdapter>([record](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                const std::array bytes{std::byte{0x27}};
                REQUIRE(sink.WriteCopied(record, bytes).HasValue());
                return Result<CanonicalCaptureDisposition>::Success(static_cast<CanonicalCaptureDisposition>(0xff));
            }, destructionCount));
            const SaveParticipantRegistrySnapshot participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants, 95), participants).Value();

            RequireError(builder.CaptureParticipants(), SaveErrors::CaptureAdapterContractInvalid);
            REQUIRE(builder.Seal().Value().Records().empty());
        }

        TEST_CASE("Rejected immutable payload returns its lease and rolls back earlier participant writes",
                  "[unit][save][capture][hostile]") {
            auto events = std::make_shared<std::vector<std::string>>();
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            const SaveRecordId first = Test::Id<SaveRecordId>(28);
            const SaveRecordId second = Test::Id<SaveRecordId>(29);
            Register(registry, Descriptor("project.capture.a_first", {first}),
                     std::make_shared<CallbackCaptureAdapter>([first](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                const std::array bytes{std::byte{0x28}};
                const Result<void> written = sink.WriteCopied(first, bytes);
                if (written.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }, destructionCount));
            Register(registry, Descriptor("project.capture.z_hostile", {second}),
                     std::make_shared<
                         CallbackCaptureAdapter>([second, events](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                auto payload =
                    std::make_shared<SegmentedTestPayload>(std::vector<std::vector<std::byte>>{{std::byte{0x01}}, {std::byte{0x02}}},
                                                           events);
                static_cast<void>(sink.WriteImmutable(second, std::move(payload)));
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }, destructionCount));
            const SaveParticipantRegistrySnapshot participants = registry.Snapshot().Value();
            RuntimeSaveCaptureLimits limits;
            limits.maximumSegments = 1;
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants, 96), participants, limits).Value();

            RequireError(builder.CaptureParticipants(), SaveErrors::CaptureAdapterContractInvalid);
            REQUIRE(*events == std::vector<std::string>{"payload"});
            RequireError(builder.Seal(), SaveErrors::CaptureIncomplete);
        }

        TEST_CASE("Capture execution and sealed projection are stable across participant registration order",
                  "[unit][save][capture][ordering]") {
            const std::array names{"game.scene.state", "game.physics.state", "game.player.state"};
            const std::array records{Test::Id<SaveRecordId>(51), Test::Id<SaveRecordId>(52), Test::Id<SaveRecordId>(53)};
            auto capture = [&](const std::array<std::size_t, 3> &registrationOrder) {
                auto destructionCount = std::make_shared<int>();
                auto execution = std::make_shared<std::vector<std::string>>();
                CanonicalStateParticipantRegistry registry;
                for (const std::size_t index : registrationOrder) {
                    auto descriptor = Descriptor(names[index], {records[index]});
                    if (index == 0)
                        descriptor.dependencies = {{Participant(names[1]), SaveParticipantDependencyRequirement::Required,
                                                    SaveParticipantDependencyPhase::Capture}};
                    Register(registry, std::move(descriptor),
                             std::make_shared<CallbackCaptureAdapter>(
                                 [execution, record = records[index]](const CanonicalCaptureContext &context, ICanonicalCaptureSink &sink) {
                        execution->push_back(context.participant.Value());
                        const std::array bytes{std::byte{0x51}};
                        const Result<void> written = sink.WriteCopied(record, bytes);
                        if (written.HasError())
                            return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                        return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
                    }, destructionCount));
                }
                const auto snapshot = CaptureRegisteredParticipants(registry).Seal().Value();
                std::vector<std::string> projection;
                for (const auto &entry : snapshot.Participants())
                    projection.push_back(entry.participant.Value());
                return std::pair{*execution, projection};
            };

            const auto first = capture({0, 1, 2});
            const auto reversed = capture({2, 1, 0});
            CHECK(first == reversed);
            CHECK(first.first == std::vector<std::string>{"game.physics.state", "game.player.state", "game.scene.state"});
            CHECK(first.second == std::vector<std::string>{"game.physics.state", "game.player.state", "game.scene.state"});
        }
    }  // namespace
}  // namespace Horo::Runtime
