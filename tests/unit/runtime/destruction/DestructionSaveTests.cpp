#include "DestructionSaveFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Destruction {
    namespace {
        using namespace SaveTest;

        struct Sink final : Runtime::ICanonicalCaptureSink {
            std::vector<std::byte> bytes;
            bool fail{};

            Result<void> WriteCopied(Runtime::SaveRecordId, std::span<const std::byte> value) override {
                if (fail)
                    return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
                bytes.assign(value.begin(), value.end());
                return Result<void>::Success();
            }

            Result<void> WriteImmutable(Runtime::SaveRecordId, std::shared_ptr<const Runtime::IImmutableCanonicalPayload>) override {
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
            }
        };

        struct Fixture final {
            std::shared_ptr<Source> source = std::make_shared<Source>();
            std::shared_ptr<DestructionSaveAdapter> adapter = DestructionSaveAdapter::Create(Binding(), source).Value();

            DestructionCanonicalArtifactView Artifact() const {
                return {Content(), source->chunks};
            }

            Runtime::RuntimeSaveSnapshot Capture() const {
                Runtime::CanonicalStateParticipantRegistry registry;
                REQUIRE(registry.Register(adapter->Descriptor(), adapter).HasValue());
                const auto participants = registry.Snapshot().Value();
                auto builder = Runtime::RuntimeSaveCaptureBuilder::Create(Provenance(participants.Generation()), participants).Value();
                REQUIRE(builder.CaptureParticipants().HasValue());
                return builder.Seal().Value();
            }
        };
    }  // namespace

    TEST_CASE("Destruction Save captures actual owner through canonical Save participant", "[unit][destruction][save]") {
        Fixture f;
        const auto before = f.source->runtime.Snapshot();
        const auto captured = f.Capture();
        REQUIRE(captured.Records().size() == 1);
        auto reader = Runtime::CanonicalValueReader::Create(captured.Records()[0].Segment(0)).Value();
        const auto decoded = DecodeDestructionSaveState(std::move(reader), f.Artifact());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().state == f.source->State());
        REQUIRE(decoded.Value().fence.epoch.value == 10);
        REQUIRE(decoded.Value().fence.sceneRevision == 2);
        REQUIRE(f.source->runtime.Snapshot() == before);
        REQUIRE_FALSE(f.source->runtime.LastResult());
        REQUIRE_FALSE(decoded.Value().state.effectiveFeatures.Contains(DestructionFeature::AuthoritativeReplication));
    }

    TEST_CASE("Destruction Save detached bytes survive owner shutdown and replacement", "[unit][destruction][save]") {
        Fixture f;
        const auto captured = f.Capture();
        f.source->runtime =
            f.source->runtime.Replace({Id<DestructionWorldId>(1), Id<DestructibleId>(1), Id<DestructionGeneration>(2)}, Descriptor())
                .Value();
        f.source->runtime = f.source->runtime.BeginShutdown();
        const DestructionSaveAdapter &adapter = *f.adapter;
        adapter.Shutdown();
        const auto decoded =
            DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(captured.Records()[0].Segment(0)).Value(), f.Artifact());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().state.target.generation.Value() == 1);
    }

    TEST_CASE("Destruction Save rejects divergent actual owner state without publication", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        const auto before = f.source->runtime.Snapshot();
        f.source->candidate = f.source->State();
        ++f.source->candidate->healthQ16;
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
        REQUIRE(f.source->runtime.Snapshot() == before);
        f.source->candidate = f.source->State();
        f.source->candidate->revision = Id<DestructionStateRevision>(2);
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
    }

    TEST_CASE("Destruction Save masks cannot imply absent paired Physics state", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        f.source->Damage();
        f.source->candidate = f.source->State();
        f.source->candidate->masks.broken[0] = std::byte{1};
        f.source->candidate->masks.active[0] = std::byte{1};
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
        auto invalid = f.source->State();
        invalid.masks.active[0] = std::byte{8};
        REQUIRE(ValidateCanonicalDestructionSnapshot(invalid, f.Artifact()).HasError());
    }

    TEST_CASE("Destruction Save budget rejects before invoking live owner", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        auto context = Context();
        context.admission.maximumCopiedRecordBytes = 1;
        REQUIRE(f.adapter->Capture(context, sink).HasError());
        REQUIRE(f.source->calls == 0);
        REQUIRE(sink.bytes.empty());
    }

    TEST_CASE("Destruction Save cancellation and shutdown preserve published owner", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        const auto before = f.source->runtime.Snapshot();
        f.source->cancellation.RequestCancellation();
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        f.adapter->Shutdown();
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
        REQUIRE(f.source->runtime.Snapshot() == before);
    }

    TEST_CASE("Destruction Save replacement and stale capture epoch fail closed", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        f.source->wrongEpoch = true;
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        f.source->wrongEpoch = false;
        f.source->runtime =
            f.source->runtime.Replace({Id<DestructionWorldId>(1), Id<DestructibleId>(1), Id<DestructionGeneration>(2)}, Descriptor())
                .Value();
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
    }

    TEST_CASE("Destruction Save foreign thread cannot invoke runtime source", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        bool rejected{};
        std::thread worker{[&] {
            rejected = f.adapter->Capture(Context(), sink).HasError();
        }};
        worker.join();
        REQUIRE(rejected);
        REQUIRE(f.source->calls == 0);
        REQUIRE(sink.bytes.empty());
    }

    TEST_CASE("Destruction Save sink rejection publishes no owner changes", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        sink.fail = true;
        const auto before = f.source->runtime.Snapshot();
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
        REQUIRE(f.source->runtime.Snapshot() == before);
    }

    TEST_CASE("Destruction Save malformed version trailing data and bounded profiles reject", "[unit][destruction][save]") {
        Fixture f;
        Sink sink;
        REQUIRE(f.adapter->Capture(Context(), sink).HasValue());
        auto trailing = sink.bytes;
        trailing.push_back(std::byte{1});
        REQUIRE(DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(trailing).Value(), f.Artifact()).HasError());
        Runtime::CanonicalValueWriter writer;
        REQUIRE(writer.WriteUInt32(2).HasValue());
        const auto encoded = std::move(writer).Finalize().Value();
        REQUIRE(DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(encoded.Bytes()).Value(), f.Artifact()).HasError());
        auto limits = DestructionCanonicalLimits{};
        limits.maximumChunks = 0;
        REQUIRE(DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(sink.bytes).Value(), f.Artifact(), limits).HasError());
    }

    TEST_CASE("Destruction Save immutable participant rollback does not seal partial records", "[unit][destruction][save]") {
        Fixture f;
        Runtime::CanonicalStateParticipantRegistry registry;
        REQUIRE(registry.Register(f.adapter->Descriptor(), f.adapter).HasValue());
        auto participants = registry.Snapshot().Value();
        auto builder = Runtime::RuntimeSaveCaptureBuilder::Create(Provenance(participants.Generation()), participants).Value();
        f.source->fail = true;
        REQUIRE(builder.CaptureParticipants().HasError());
        REQUIRE(builder.Seal().HasError());
        f.source->fail = false;
        REQUIRE(builder.CaptureParticipants().HasValue());
        REQUIRE(builder.Seal().HasValue());
    }

    TEST_CASE("Destruction Save pairs active semantics to explicitly named same epoch Physics participation", "[unit][destruction][save]") {
        Fixture f;
        f.source->Damage();
        f.source->candidate = f.source->State();
        f.source->candidate->masks.broken[0] = std::byte{1};
        f.source->candidate->masks.active[0] = std::byte{1};
        const auto physicsParticipant = Runtime::SaveParticipantId::Parse("horo.physics").Value();
        f.source->physics = DestructionPhysicsCaptureReceipt{physicsParticipant,
                                                             SaveId<Runtime::SaveRecordId>(2),
                                                             1,
                                                             {10},
                                                             f.source->runtime.Snapshot().target,
                                                             {Id<DestructionChunkId>(1)}};
        auto binding = Binding();
        binding.physicsParticipant = physicsParticipant;
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        Sink sink;
        REQUIRE(f.adapter->Capture(Context(), sink).HasValue());
        const auto decoded = DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(sink.bytes).Value(), f.Artifact());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().state == f.source->candidate.value());
        REQUIRE(decoded.Value().physics.has_value());
        REQUIRE(decoded.Value().physics->checkpoint == f.source->physics->checkpoint);
        REQUIRE(decoded.Value().physics->activeChunks == f.source->physics->activeChunks);
        Runtime::CanonicalStateParticipantRegistry registry;
        REQUIRE(registry.Register(f.adapter->Descriptor(), f.adapter).HasValue());
        REQUIRE(registry.Snapshot().HasError());
        sink.bytes.clear();
        ++f.source->physics->epoch.value;
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
    }

    TEST_CASE("Destruction Save rechecks narrower adapter limits after broader owner sealing", "[unit][destruction][save]") {
        Fixture f;
        const auto before = f.source->runtime.Snapshot();
        auto binding = Binding();
        binding.limits.maximumChunks = 2;
        binding.limits.maximumAnchors = 2;
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        Runtime::CanonicalStateParticipantRegistry registry;
        REQUIRE(registry.Register(f.adapter->Descriptor(), f.adapter).HasValue());
        auto participants = registry.Snapshot().Value();
        auto builder = Runtime::RuntimeSaveCaptureBuilder::Create(Provenance(participants.Generation()), participants).Value();
        const auto rejected = builder.CaptureParticipants();
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == DestructionErrors::LimitExceeded.code.Value());
        REQUIRE(builder.Seal().HasError());
        REQUIRE(f.source->runtime.Snapshot() == before);
        binding.limits.maximumChunks = 3;
        binding.limits.maximumAnchors = 3;
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        REQUIRE(f.Capture().Records().size() == 1);
        REQUIRE(f.source->runtime.Snapshot() == before);
    }

    TEST_CASE("Destruction Save preserves maximal semantic participant identity and exact byte budget", "[unit][destruction][save]") {
        Fixture f;
        f.source->Damage();
        f.source->candidate = f.source->State();
        f.source->candidate->masks.broken[0] = std::byte{1};
        f.source->candidate->masks.active[0] = std::byte{1};
        const std::string name = "physics." + std::string(Runtime::MaximumSaveParticipantIdBytes - 8, 'a');
        REQUIRE(name.size() == Runtime::MaximumSaveParticipantIdBytes);
        const auto participant = Runtime::SaveParticipantId::Parse(name);
        REQUIRE(participant.HasValue());
        REQUIRE(Runtime::SaveParticipantId::Parse(name + "a").HasError());
        f.source->physics = DestructionPhysicsCaptureReceipt{participant.Value(),
                                                             SaveId<Runtime::SaveRecordId>(2),
                                                             1,
                                                             {10},
                                                             f.source->runtime.Snapshot().target,
                                                             {Id<DestructionChunkId>(1)}};
        auto binding = Binding();
        binding.physicsParticipant = participant.Value();
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        Sink sink;
        REQUIRE(f.adapter->Capture(Context(), sink).HasValue());
        const auto bytes = sink.bytes;
        const auto decoded = DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(bytes).Value(), f.Artifact());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().physics.has_value());
        REQUIRE(decoded.Value().physics->participant.Value() == name);
        binding.limits.maximumBytes = bytes.size();
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        sink.bytes.clear();
        REQUIRE(f.adapter->Capture(Context(), sink).HasValue());
        REQUIRE(sink.bytes == bytes);
        auto oversized = bytes;
        const auto identityBytes = std::as_bytes(std::span{name.data(), name.size()});
        const auto identity = std::search(oversized.begin(), oversized.end(), identityBytes.begin(), identityBytes.end());
        REQUIRE(identity != oversized.end());
        REQUIRE(identity - oversized.begin() >= 4);
        *(identity - 4) = static_cast<std::byte>(Runtime::MaximumSaveParticipantIdBytes + 1);
        REQUIRE(DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(oversized).Value(), f.Artifact()).HasError());
        --binding.limits.maximumBytes;
        f.adapter = DestructionSaveAdapter::Create(binding, f.source).Value();
        sink.bytes.clear();
        REQUIRE(f.adapter->Capture(Context(), sink).HasError());
        REQUIRE(sink.bytes.empty());
        REQUIRE(DecodeDestructionSaveState(Runtime::CanonicalValueReader::Create(bytes).Value(), f.Artifact(), binding.limits).HasError());
    }

}  // namespace Horo::Destruction
