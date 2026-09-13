#pragma once

#include "Horo/Runtime/Save/SaveArchiveMetadata.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Horo::Runtime::Test {
    template <typename Identity> Identity Id(const std::uint8_t suffix) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Identity::FromBytes(bytes).Value();
    }

    template <typename Version> Version V(const std::uint32_t value) {
        return Version::Create(value).Value();
    }

    inline Sha256Digest Digest(const std::uint8_t suffix) {
        Sha256Digest value{};
        value.bytes.back() = suffix;
        return value;
    }

    inline SaveStorageAddress Address(const std::uint8_t slot = 4) {
        return {.namespaceAccess = {.expected = {.product = Id<ProductStorageId>(1),
                                                 .environment = Id<EnvironmentStorageId>(2),
                                                 .owner = ServerWorldOwner{Id<ServerStorageOwnerId>(3)}},
                                    .expectedRevision = 7},
                .slot = Id<SaveGameSlotId>(slot)};
    }

    inline SaveSlotCatalogEntry Entry(const std::uint8_t slot, const std::uint8_t generation) {
        return {.publication = {.slot = Id<SaveGameSlotId>(slot),
                                .generation = Id<SlotGenerationId>(generation),
                                .kind = SaveSlotKind::Manual,
                                .savedAtUnixMilliseconds = 1'700'000'000'000ULL + generation,
                                .playTimeNanoseconds = 42,
                                .baseScene = Id<SaveBaseSceneId>(3),
                                .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                                .saveSchema = V<SaveSchemaVersion>(1),
                                .projectBuildId = "test-build",
                                .canonicalState = {.value = Digest(generation)},
                                .archiveContent = {.value = Digest(static_cast<std::uint8_t>(generation + 40))},
                                .cloudState = SaveSlotCloudState::LocalOnly},
                .display = {.displayName = "Slot"}};
    }

    inline std::vector<SaveManifestParticipant> StandardParticipants(const std::uint32_t sceneVersion,
                                                                     const std::uint32_t gameplayVersion) {
        return {{.participant = SaveParticipantId::Parse("horo.scene.core.v1").Value(),
                 .schemaVersion = V<ParticipantSchemaVersion>(sceneVersion),
                 .required = true,
                 .chunks = {Id<SaveRecordId>(20), Id<SaveRecordId>(21)}},
                {.participant = SaveParticipantId::Parse("project.gameplay.v1").Value(),
                 .schemaVersion = V<ParticipantSchemaVersion>(gameplayVersion),
                 .required = false,
                 .chunks = {Id<SaveRecordId>(22)}}};
    }
}  // namespace Horo::Runtime::Test
