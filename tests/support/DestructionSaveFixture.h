#pragma once
#include "Horo/Destruction/DestructionSaveCapture.h"

#include <array>
#include <cmath>
#include <stdexcept>

namespace Horo::Destruction::SaveTest {
    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    template <typename T> T SaveId(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T::FromBytes(bytes).Value();
    }

    inline FractureArtifactContentIdentity Content() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        Sha256Digest digest{};
        digest.bytes.back() = 1;
        return FractureArtifactContentIdentity::Create(FractureAssetId::Create(Assets::AssetId::FromBytes(bytes)).Value(),
                                                       Id<FractureContentRevision>(1), digest)
            .Value();
    }

    inline DestructibleDescriptor Descriptor() {
        auto descriptor = DestructibleDescriptor::Create(
            {.destructible = Id<DestructibleId>(1),
             .content = Content(),
             .configurationRevision = Id<DestructionConfigurationRevision>(1),
             .features = {.required = {.bits = DestructionFeatureBit<DestructionFeature::PreCookedFracture>}},
             .limits = GetDestructionTierProfile(DestructionFeatureTier::Baseline).Value().limits,
             .behavior = {.support = DestructionSupportPolicy::Disabled}});
        if (descriptor.HasError())
            throw std::logic_error("Destruction Save fixture descriptor admission failed: " + descriptor.ErrorValue().message);
        return std::move(descriptor).Value();
    }

    struct Source final : IDestructionSaveSource {
        DestructionDamageRuntime runtime =
            DestructionDamageRuntime::Create(Descriptor(), {Id<DestructionWorldId>(1), Id<DestructibleId>(1), Id<DestructionGeneration>(1)},
                                             Id<DestructionStateRevision>(1))
                .Value();
        std::vector<DestructionChunkId> chunks{Id<DestructionChunkId>(1), Id<DestructionChunkId>(2), Id<DestructionChunkId>(3)};
        mutable std::uint32_t calls{};
        std::optional<CanonicalDestructionSnapshot> candidate;
        std::optional<DestructionPhysicsCaptureReceipt> physics;
        CancellationSource cancellation;
        bool fail{};
        bool wrongEpoch{};

        CanonicalDestructionSnapshot State() const {
            const auto &live = runtime.Snapshot();
            return {.target = live.target,
                    .content = live.content,
                    .configuration = live.configurationRevision,
                    .effectiveFeatures = live.effectiveFeatures,
                    .authority = Id<DestructionAuthorityEpoch>(1),
                    .revision = live.revision,
                    .phase = live.phase,
                    .healthQ16 = static_cast<std::uint32_t>(std::round(live.health * 65536.0)),
                    .seed = {1, 0, 0},
                    .masks = {3, {std::byte{}}, {std::byte{}}, {std::byte{}}, {std::byte{}}},
                    .supportAnchors = {},
                    .supportCursor = 0};
        }

        DestructionDamageContext Context(const std::uint64_t tick) const {
            const auto &live = runtime.Snapshot();
            return {.target = live.target,
                    .content = live.content,
                    .configurationRevision = live.configurationRevision,
                    .capabilityRevision = Id<DestructionCapabilityRevision>(1),
                    .authority = {Id<DestructionAuthorityId>(1),
                                  Id<DestructionAuthorityRevision>(1),
                                  {.bits = DestructionCommandCapabilityBit<DestructionCommandCapability::Damage>}},
                    .simulationTick = tick};
        }

        void Damage(const float amount = 30.0F) {
            const auto context = Context(9);
            DestructionCommandHeader header{.id = {runtime.Snapshot().target, Id<DestructionCommandValue>(1)},
                                            .expectedRevision = runtime.Snapshot().revision,
                                            .capabilityRevision = context.capabilityRevision,
                                            .eligibleSimulationTick = 9,
                                            .authority = context.authority};
            auto command = DestructionCommand::Damage(header, amount).Value();
            auto prepared = runtime.Prepare(command, context).Value();
            runtime = runtime.Commit(prepared, context).Value();
        }

        Result<CapturedDestructionState> Capture(const Runtime::CanonicalCaptureContext &context) const override {
            ++calls;
            if (fail)
                return Result<CapturedDestructionState>::Failure(MakeError(DestructionErrors::StateInvalid));
            const DestructionCanonicalArtifactView artifact{runtime.Snapshot().content, chunks};
            auto fence = DestructionSaveCaptureFence{context.provenance.sceneIncarnation,
                                                     context.provenance.sceneRevision,
                                                     context.provenance.epoch,
                                                     Id<DestructionCapabilityRevision>(1),
                                                     Id<DestructionAuthorityEpoch>(1),
                                                     cancellation.Token()};
            if (wrongEpoch)
                ++fence.epoch.value;
            return CapturedDestructionState::Capture({runtime, Context(context.provenance.epoch.value), artifact},
                                                     candidate.value_or(State()), fence, physics);
        }
    };

    inline DestructionSaveBinding Binding() {
        return {Runtime::SaveParticipantId::Parse("horo.destruction").Value(),
                SaveId<Runtime::SaveRecordId>(1),
                1,
                {Id<DestructionWorldId>(1), Id<DestructibleId>(1), Id<DestructionGeneration>(1)},
                Content(),
                Id<DestructionCapabilityRevision>(1),
                Id<DestructionAuthorityEpoch>(1),
                {},
                {}};
    }

    inline Runtime::RuntimeSaveCaptureProvenance Provenance(const std::uint64_t registry) {
        return {SaveId<Runtime::CapturedStateId>(1), {10}, 1, 2, registry};
    }

    inline Runtime::CanonicalCaptureContext Context() {
        return {Provenance(1),
                Binding().participant,
                Runtime::ParticipantSchemaVersion::Create(1).Value(),
                Runtime::SaveParticipantScope::RuntimeScene,
                {1, 1, 16384, 1, 16384, 16384}};
    }
}  // namespace Horo::Destruction::SaveTest
