#pragma once

#include "Horo/Gameplay/SaveGameplayPersistence.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>

/** @brief Shared explicit descriptor and capture fixtures for gameplay persistence contracts. */
namespace Horo::Runtime::Test::GameplayPersistence {
    class NoDependencies final : public ICanonicalRestoreDependencyLookup {
    public:
        const ICanonicalRestorePreparedState *Find(const SaveParticipantId &) const noexcept override {
            return nullptr;
        }
    };

    [[nodiscard]] inline GameplayPersistenceDescriptor Descriptor(const GameplayPersistenceOwner owner, const std::uint8_t identity = 1) {
        GameplayPersistenceDescriptor descriptor;
        descriptor.participant.participant = SaveParticipantId::Parse("project.gameplay.state").Value();
        descriptor.participant.schemaVersion = Test::V<ParticipantSchemaVersion>(3);
        descriptor.participant.scope =
            owner == GameplayPersistenceOwner::Session ? SaveParticipantScope::SlotPlayer : SaveParticipantScope::RuntimeScene;
        descriptor.participant.roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore;
        descriptor.participant.limits = {.maximumPayloadBytes = 128, .maximumRecordCount = 1, .maximumNestingDepth = 4};
        descriptor.record = Test::Id<SaveRecordId>(identity);
        descriptor.participant.ownedRecords = {descriptor.record};
        descriptor.owner = owner;
        descriptor.moduleId = SaveParticipantId::Parse("project.gameplay.module").Value();
        descriptor.moduleVersion = 7;
        return descriptor;
    }

    [[nodiscard]] inline RuntimeSaveCaptureProvenance Provenance(const SaveParticipantRegistrySnapshot &participants) {
        return {.capturedState = Test::Id<CapturedStateId>(9),
                .epoch = CanonicalCaptureEpoch{11},
                .sceneIncarnation = 4,
                .sceneRevision = 5,
                .registryGeneration = participants.Generation()};
    }

    /** @brief Captures explicit registered state with no ambient save host. */
    [[nodiscard]] inline RuntimeSaveSnapshot CaptureRegisteredState(const std::shared_ptr<GameplayPersistenceAdapter> &adapter) {
        CanonicalStateParticipantRegistry registry;
        REQUIRE(registry.Register(adapter->Descriptor().participant, adapter).HasValue());
        const auto participants = registry.Snapshot().Value();
        auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
        REQUIRE(builder.CaptureParticipants().HasValue());
        return builder.Seal().Value();
    }

}  // namespace Horo::Runtime::Test::GameplayPersistence
