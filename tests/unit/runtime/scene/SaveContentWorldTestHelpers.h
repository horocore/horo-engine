#pragma once

#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "SaveContentTestHelpers.h"

#include <algorithm>

namespace Horo::Runtime::SceneContentWorldTest {
    class CaptureClock final : public Clock {
    public:
        [[nodiscard]] Duration MonotonicNow() const override {
            return {};
        }
    };

    class WorldCaptureAdapter final : public ICanonicalStateAdapter {
    public:
        mutable std::size_t calls{};
        bool skip{};
        std::array<std::byte, 2> bytes{std::byte{1}, std::byte{2}};

        Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) const override {
            ++calls;
            if (skip)
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Omitted);
            auto written = sink.WriteCopied(SceneTest::Id<SaveRecordId>(11), bytes);
            return written.HasError() ? Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue())
                                      : Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
        }
    };

    struct WorldFixture final {
        SceneContentTest::Fixture source;
        std::shared_ptr<RuntimeSceneService> service{std::make_shared<RuntimeSceneService>()};
        std::optional<SaveContentWorld> world;
        CanonicalStateParticipantRegistry registry;
        std::shared_ptr<WorldCaptureAdapter> adapter{std::make_shared<WorldCaptureAdapter>()};
        CaptureClock clock;
        std::unique_ptr<SaveCaptureBarrier> barrier;
        SaveParticipantRegistrySnapshot participants;
        SaveRuntimeGeneration generation;

        using NativeBinding = std::pair<CanonicalStateParticipantDescriptor, std::shared_ptr<const ICanonicalStateAdapter>>;

        explicit WorldFixture(std::vector<GameplayPersistenceInstallation> installations = {},
                              std::vector<NativeBinding> nativeBindings = {}, std::optional<ImmutableSaveArchive> archive = {},
                              std::optional<SaveContentProjectPolicy> policy = {},
                              std::shared_ptr<const Assets::AssetArchiveProvider> provider = {}) {
            if (archive)
                source.archive = std::move(*archive);
            if (policy)
                source.policy = std::move(*policy);
            const bool replacedProvider = static_cast<bool>(provider);
            if (provider)
                source.provider = std::move(provider);
            if (!installations.empty() || replacedProvider)
                REQUIRE(source.installed->Replace(source.provider, std::move(installations)).HasValue());
            PublishSourceWorld();
            AdmitCaptureOwners(std::move(nativeBindings));
        }

        /** @brief Uses the actual content-owned queue and publication receipt before binding a captureable world. */
        void PublishSourceWorld() {
            REQUIRE(service->Startup(source.cancellation.Token()).HasValue());
            auto prepared = source.Prepare();
            if (prepared.HasError()) {
                INFO("world preparation error: " << prepared.ErrorValue().code.Value() << ": " << prepared.ErrorValue().message);
                std::string diagnostics;
                for (const auto &diagnostic : prepared.ErrorValue().diagnostics)
                    diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
                INFO("world preparation diagnostics: " << diagnostics);
                REQUIRE(prepared.HasValue());
            }
            REQUIRE(prepared.HasValue());
            auto queuedResult = std::move(prepared).Value().Queue(service);
            REQUIRE(queuedResult.HasValue());
            auto queued = std::move(queuedResult).Value();
            FrameContext context{1, {}, 0.0, 0, {}, false, source.cancellation.Token()};
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
            auto bound = queued.BindPublishedWorld();
            REQUIRE(bound.HasValue());
            world = std::move(bound).Value();
        }

        /** @brief Pins actual registry owners and publishes each owner's coherent barrier epoch. */
        void AdmitCaptureOwners(std::vector<NativeBinding> nativeBindings) {
            REQUIRE(world->RegisterRequirements(registry).HasValue());
            CanonicalStateParticipantDescriptor descriptor{.participant = SaveParticipantId::Parse("project.state").Value(),
                                                           .schemaVersion = SceneContentTest::V<ParticipantSchemaVersion>(),
                                                           .scope = SaveParticipantScope::RuntimeScene,
                                                           .roles = SaveParticipantRole::Capture,
                                                           .required = true,
                                                           .limits = {2, 1, 1},
                                                           .dependencies = {},
                                                           .ownedRecords = {SceneTest::Id<SaveRecordId>(11)}};
            REQUIRE(registry.Register(std::move(descriptor), adapter).HasValue());
            for (auto &binding : nativeBindings)
                REQUIRE(registry.Register(std::move(binding.first), std::move(binding.second)).HasValue());
            auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            participants = std::move(snapshot).Value();
            generation = {.runtime = 3, .scene = service->ActiveScene()->RuntimeId().value, .registry = participants.Generation()};
            auto created = SaveCaptureBarrier::Create(17, clock, 8);
            REQUIRE(created.HasValue());
            barrier = std::move(created).Value();
            const std::array names{"project.capture", "horo.jobs", "horo.scene.mutation", "horo.subsystem"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                auto owner = barrier->Register(SaveParticipantId::Parse(names[index]).Value(), static_cast<SaveBarrierDomain>(index));
                REQUIRE(owner.HasValue());
                REQUIRE(barrier->PublishReadiness(owner.Value(), {.value = 41}).HasValue());
            }
            // Each actual capture owner publishes the same epoch, in addition to host domain readiness.
            for (const auto &binding : participants.CaptureBindings()) {
                auto owner = barrier->Register(binding.Descriptor().participant, SaveBarrierDomain::Subsystem);
                REQUIRE(owner.HasValue());
                REQUIRE(barrier->PublishReadiness(owner.Value(), {.value = 41}).HasValue());
            }
        }

        ~WorldFixture() {
            service->Shutdown();
        }

        SaveContentSnapshot Capture(std::uint8_t identity = 12, SaveDegradedWorldPolicy policy = SaveDegradedWorldPolicy::Reject) {
            REQUIRE(barrier->Request(91, generation).HasValue());
            auto captured = world->CaptureAtSafePoint(*barrier,
                                                      SaveContentCaptureRequest{.phase = RuntimePhase::CommitDeferredLifecycleChanges,
                                                                                .generation = generation,
                                                                                .capturedState = SceneTest::Id<CapturedStateId>(identity),
                                                                                .epoch = {.value = 41}},
                                                      participants, policy);
            if (captured.HasError()) {
                INFO("content capture admission: " << captured.ErrorValue().code.Value() << ": " << captured.ErrorValue().message);
                REQUIRE(captured.HasValue());
            }
            REQUIRE(captured.HasValue());
            REQUIRE(captured.Value().barrier.state == SaveBarrierState::Captured);
            REQUIRE(captured.Value().capture);
            auto value = std::move(captured).Value();
            REQUIRE(barrier->Acknowledge(91).HasValue());
            return std::move(*value.capture);
        }

        SaveArchiveHeader Header() const {
            auto read = SaveArchiveReader{}.Read(source.archive.bytes);
            REQUIRE(read.HasValue());
            return read.Value().Header();
        }
    };

    /** @brief Rebuilds a deliberately altered schema2 test archive from actual admitted raw records, authenticating its new root. */
    inline ImmutableSaveArchive RewriteRequirements(ImmutableSaveArchive archive, std::span<const SaveContentRequirement> requirements) {
        auto admitted = SaveArchiveReader{}.Read(archive.bytes);
        REQUIRE(admitted.HasValue());
        auto encoded = EncodeSaveContentRequirements(requirements);
        REQUIRE(encoded.HasValue());
        std::vector<PreservedSaveChunk> chunks;
        for (const auto &entry : admitted.Value().Directory().Entries()) {
            REQUIRE(entry.codec == SaveChunkCodec::Raw);
            const auto bytes = admitted.Value().Payload().subspan(static_cast<std::size_t>(entry.offset),
                                                                  static_cast<std::size_t>(entry.storedByteLength));
            chunks.push_back({entry, {bytes.begin(), bytes.end()}});
            if (entry.record == SaveContentRequirementsRecord()) {
                chunks.back().storedBytes.assign(encoded.Value().Bytes().begin(), encoded.Value().Bytes().end());
                chunks.back().entry.storedByteLength = chunks.back().storedBytes.size();
                chunks.back().entry.decodedByteLength = chunks.back().storedBytes.size();
                chunks.back().entry.decodedHash = ComputeSha256(chunks.back().storedBytes);
            }
        }
        auto manifest = admitted.Value().Manifest();
        std::vector<std::vector<SaveSceneCanonicalRecord>> records;
        records.reserve(manifest.participants.size());
        std::vector<SaveSceneCanonicalParticipant> owners;
        owners.reserve(manifest.participants.size());
        for (const auto &owner : manifest.participants) {
            records.emplace_back();
            for (const auto record : owner.chunks) {
                const auto chunk = std::ranges::find(chunks, record, [](const PreservedSaveChunk &value) {
                    return value.entry.record;
                });
                REQUIRE(chunk != chunks.end());
                records.back().push_back({record, std::span<const std::byte>{chunk->storedBytes}});
            }
            owners.push_back({owner.participant, owner.schemaVersion, records.back()});
        }
        auto canonical = EncodeSaveSceneCanonicalState(admitted.Value().Header(), owners);
        REQUIRE(canonical.HasValue());
        manifest.canonicalState = ComputeCanonicalStateHash(canonical.Value().Bytes());
        auto written = SaveArchiveContainerWriter::Write(admitted.Value().Header(), manifest, chunks,
                                                         admitted.Value().Preamble().archiveFormatVersion);
        REQUIRE(written.HasValue());
        return written.Value().Archive();
    }

}  // namespace Horo::Runtime::SceneContentWorldTest
