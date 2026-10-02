#include "Horo/Destruction/DestructionErrors.h"
#include "Horo/Destruction/DestructionReplication.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationDescriptorRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Horo::Destruction {
    namespace {
        template <typename Identity> [[nodiscard]] Identity Id(const std::uint64_t value) {
            auto identity = Identity::Create(value);
            REQUIRE(identity.HasValue());
            return identity.Value();
        }

        [[nodiscard]] FractureArtifactContentIdentity Content(const std::uint8_t value = 1) {
            std::array<std::uint8_t, 16> assetBytes{};
            assetBytes.back() = value;
            auto asset = FractureAssetId::Create(Assets::AssetId::FromBytes(assetBytes));
            REQUIRE(asset.HasValue());
            Sha256Digest digest{};
            digest.bytes.back() = value;
            auto content = FractureArtifactContentIdentity::Create(asset.Value(), Id<FractureContentRevision>(2), digest);
            REQUIRE(content.HasValue());
            return content.Value();
        }

        [[nodiscard]] Network::NetworkObjectId Object(const std::uint64_t epoch = 7, const std::uint32_t generation = 1) {
            return Network::NetworkObjectId::Create(Network::ReplicationAuthorityEpoch::Create(epoch).Value(), 12, generation).Value();
        }

        [[nodiscard]] Network::ReplicationRoleBinding Role(const Network::ReplicationExecutionRole role, const std::uint64_t session = 1) {
            const auto schema = MakeDestructionReplicationDescriptor();
            REQUIRE(schema.HasValue());
            const auto owner = Network::NetworkPeerId::Create(9).Value();
            const auto local = Network::NetworkPeerId::Create(role == Network::ReplicationExecutionRole::SimulatedClient ? 10 : 9).Value();
            return {.revision = Network::ReplicationRoleRevision::Create(1).Value(),
                    .session = Network::NetworkSessionGeneration::Create(session).Value(),
                    .object = Object(),
                    .schema = schema.Value().id,
                    .schemaVersion = DestructionReplicationVersion,
                    .role = role,
                    .localPeer = role == Network::ReplicationExecutionRole::AuthorityServer ? std::nullopt
                                                                                            : std::optional<Network::NetworkPeerId>{local},
                    .autonomousOwner = owner};
        }

        [[nodiscard]] DestructionReplicationState State() {
            return {.target = {Id<DestructionWorldId>(3), Id<DestructibleId>(4), Id<DestructionGeneration>(5)},
                    .content = Content(),
                    .configuration = Id<DestructionConfigurationRevision>(6),
                    .effectiveFeatures = {.bits = DestructionFeatureBit<DestructionFeature::AuthoritativeReplication>},
                    .authority = Network::ReplicationAuthorityEpoch::Create(7).Value(),
                    .revision = Id<DestructionStateRevision>(8),
                    .phase = DestructionStatePhase::Damaged,
                    .healthQ16 = 0x00098000U,
                    .seed = {.version = 1, .value = 0, .cursor = 42},
                    .masks = {.chunkCount = 10,
                              .broken = {std::byte{0x0F}, std::byte{0x01}},
                              .active = {std::byte{0x01}, std::byte{}},
                              .supported = {std::byte{0x02}, std::byte{}},
                              .dormant = {std::byte{0x04}, std::byte{}}},
                    .supportAnchors = {Id<DestructionChunkId>(100), Id<DestructionChunkId>(200)},
                    .supportCursor = 2};
        }

        [[nodiscard]] DestructionReplicationFence Fence(const DestructionReplicationState &state, const std::uint64_t session = 1) {
            return {.session = Network::NetworkSessionGeneration::Create(session).Value(),
                    .object = Object(),
                    .target = state.target,
                    .content = state.content,
                    .configuration = state.configuration,
                    .effectiveFeatures = state.effectiveFeatures,
                    .authority = state.authority};
        }

        [[nodiscard]] std::vector<DestructionChunkId> ChunkTable() {
            std::vector<DestructionChunkId> chunks;
            for (std::uint64_t value = 1; value <= 8; ++value)
                chunks.push_back(Id<DestructionChunkId>(value));
            chunks.push_back(Id<DestructionChunkId>(100));
            chunks.push_back(Id<DestructionChunkId>(200));
            return chunks;
        }

        [[nodiscard]] Result<DestructionReplicationPayload> EncodeDestructionReplication(const DestructionReplicationState &state,
                                                                                         const Network::ReplicationRoleBinding &role,
                                                                                         const DestructionReplicationLimits &limits) {
            const auto table = ChunkTable();
            return Horo::Destruction::EncodeDestructionReplication(state, {state.content, table}, role, limits);
        }

        [[nodiscard]] Result<DestructionReplicationState> DecodeDestructionReplication(const DestructionReplicationPayload &payload,
                                                                                       const DestructionReplicationFence &fence,
                                                                                       const Network::ReplicationRoleBinding &role,
                                                                                       const DestructionReplicationLimits &limits) {
            const auto table = ChunkTable();
            return Horo::Destruction::DecodeDestructionReplication(payload, fence, {fence.content, table}, role, limits);
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("DFR declares a closed bounded Network schema and pins exact typed serializers",
                  "[unit][destruction][replication][descriptor]") {
            const auto declared = MakeDestructionReplicationDescriptor();
            REQUIRE(declared.HasValue());
            CHECK(declared.Value().fields.size() == 10);
            for (const auto &field : declared.Value().fields) {
                CHECK(field.condition == Network::ReplicationCondition::Always);
                CHECK(field.requirement == Network::ReplicationFieldRequirement::Required);
                CHECK(field.writePolicy == Network::ReplicationWritePolicy::AuthorityServerOnly);
            }
            const auto snapshots =
                Network::BuildReplicationDescriptorSnapshot(std::array{declared.Value()}, {.maximumSchemas = 1,
                                                                                           .maximumFieldsPerSchema = 10,
                                                                                           .maximumOwnerIdentityBytes = 64,
                                                                                           .maximumDefaultBytesPerField = 1,
                                                                                           .maximumTotalDefaultBytes = 1});
            REQUIRE(snapshots.HasValue());
            const auto serializers = MakeDestructionReplicationSerializers();
            REQUIRE(serializers.HasValue());
            REQUIRE(serializers.Value().size() == 10);
            const auto registry = Network::ReplicationSerializerRegistry::Create(snapshots.Value(), serializers.Value());
            REQUIRE(registry.HasValue());
            CHECK(registry.Value().Schemas()->Fingerprint() == snapshots.Value()->Fingerprint());
        }

        TEST_CASE("Equivalent destruction semantics produce identical canonical fields and detached round-trip values",
                  "[unit][destruction][replication][canonical]") {
            const auto state = State();
            const auto writer = Role(Network::ReplicationExecutionRole::AuthorityServer);
            const auto receiver = Role(Network::ReplicationExecutionRole::SimulatedClient);
            const auto first = EncodeDestructionReplication(state, writer, {});
            const auto second = EncodeDestructionReplication(state, writer, {});
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(first.Value().fields.size() == 10);
            CHECK(first.Value().fields == second.Value().fields);
            const auto decoded = DecodeDestructionReplication(first.Value(), Fence(state), receiver, {});
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value() == state);
            CHECK(first.Value().fields.front().canonical.size() == SerializedDestructionHandle{}.size());
            CHECK(first.Value().fields[1].canonical.size() == SerializedFractureArtifactContentIdentity{}.size());
        }

        TEST_CASE("Clients cannot originate DFR authority fields and stale session or authority cannot decode",
                  "[unit][destruction][replication][authority]") {
            const auto state = State();
            const auto server = Role(Network::ReplicationExecutionRole::AuthorityServer);
            const auto client = Role(Network::ReplicationExecutionRole::AutonomousClient);
            ExpectError(EncodeDestructionReplication(state, client, {}), Network::NetworkErrors::ReplicationAuthorityDenied);
            const auto payload = EncodeDestructionReplication(state, server, {});
            REQUIRE(payload.HasValue());
            ExpectError(DecodeDestructionReplication(payload.Value(), Fence(state), server, {}),
                        Network::NetworkErrors::ReplicationAuthorityDenied);
            ExpectError(DecodeDestructionReplication(payload.Value(), Fence(state, 2), client, {}),
                        DestructionErrors::ReplicationIncompatible);
            auto stale = Fence(state);
            stale.authority = Network::ReplicationAuthorityEpoch::Create(8).Value();
            ExpectError(DecodeDestructionReplication(payload.Value(), stale, client, {}), DestructionErrors::ReplicationStaleAuthority);
        }

        TEST_CASE("DFR wire admission rejects malformed sets, fields and finite limits before publication",
                  "[unit][destruction][replication][hostile]") {
            auto state = State();
            const auto server = Role(Network::ReplicationExecutionRole::AuthorityServer);
            const auto client = Role(Network::ReplicationExecutionRole::SimulatedClient);
            state.masks.active[0] = std::byte{0x04};
            ExpectError(EncodeDestructionReplication(state, server, {}), DestructionErrors::ReplicationInvalidChunkMask);
            state = State();
            state.masks.broken[1] = std::byte{0x80};
            ExpectError(EncodeDestructionReplication(state, server, {}), DestructionErrors::ReplicationInvalidChunkMask);
            state = State();
            state.supportAnchors = {Id<DestructionChunkId>(200), Id<DestructionChunkId>(100)};
            ExpectError(EncodeDestructionReplication(state, server, {}), DestructionErrors::ReplicationInvalid);
            state = State();
            state.effectiveFeatures = {};
            ExpectError(EncodeDestructionReplication(state, server, {}), DestructionErrors::ReplicationInvalid);
            state = State();
            state.effectiveFeatures.bits |= 0x80000000U;
            ExpectError(EncodeDestructionReplication(state, server, {}), DestructionErrors::ReplicationInvalid);
            state = State();
            ExpectError(EncodeDestructionReplication(state, server, {.maximumChunks = 8}), DestructionErrors::ReplicationLimitExceeded);
            auto payload = EncodeDestructionReplication(state, server, {});
            REQUIRE(payload.HasValue());
            auto malformed = payload.Value();
            malformed.fields.pop_back();
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationInvalid);
            malformed = payload.Value();
            malformed.fields[1].id = malformed.fields[0].id;
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationInvalid);
            malformed = payload.Value();
            malformed.fields[0].canonical.resize(25);
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationLimitExceeded);
            malformed = payload.Value();
            malformed.fields[7].canonical[0] = std::byte{0x01};
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationLimitExceeded);
            malformed = payload.Value();
            malformed.fields[6].canonical[3] = std::byte{};
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationInvalid);
            malformed = payload.Value();
            malformed.fields[9].canonical.pop_back();
            ExpectError(DecodeDestructionReplication(malformed, Fence(state), client, {}), DestructionErrors::ReplicationInvalid);
        }

        TEST_CASE("Maximum admitted chunk mask is bounded and canonical at its last bit", "[unit][destruction][replication][boundary]") {
            auto state = State();
            state.phase = DestructionStatePhase::Intact;
            state.masks.chunkCount = DestructionHardLimits::ChunksPerDestructible;
            state.masks.broken.assign(128, std::byte{});
            state.masks.active.assign(128, std::byte{});
            state.masks.supported.assign(128, std::byte{});
            state.masks.dormant.assign(128, std::byte{});
            state.supportAnchors.clear();
            std::vector<DestructionChunkId> table;
            table.reserve(state.masks.chunkCount);
            for (std::uint64_t value = 1; value <= state.masks.chunkCount; ++value)
                table.push_back(Id<DestructionChunkId>(value));
            const DestructionReplicationArtifactView artifact{state.content, table};
            const auto server = Role(Network::ReplicationExecutionRole::AuthorityServer);
            const auto client = Role(Network::ReplicationExecutionRole::SimulatedClient);
            const auto encoded = Horo::Destruction::EncodeDestructionReplication(state, artifact, server, {});
            REQUIRE(encoded.HasValue());
            const auto decoded = Horo::Destruction::DecodeDestructionReplication(encoded.Value(), Fence(state), artifact, client, {});
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value() == state);
            ExpectError(Horo::Destruction::EncodeDestructionReplication(state, artifact, server, {.maximumChunks = 1023}),
                        DestructionErrors::ReplicationLimitExceeded);
        }

        TEST_CASE("Replacement content, runtime generation and revision fence detached DFR candidates",
                  "[unit][destruction][replication][lifecycle]") {
            const auto state = State();
            const auto payload = EncodeDestructionReplication(state, Role(Network::ReplicationExecutionRole::AuthorityServer), {});
            REQUIRE(payload.HasValue());
            const auto client = Role(Network::ReplicationExecutionRole::SimulatedClient);
            auto fence = Fence(state);
            fence.content = Content(2);
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::ReplicationIncompatible);
            fence = Fence(state);
            fence.target.generation = Id<DestructionGeneration>(6);
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::StaleGeneration);
            fence = Fence(state);
            fence.configuration = Id<DestructionConfigurationRevision>(7);
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::StaleConfiguration);
            fence = Fence(state);
            fence.effectiveFeatures.bits |= DestructionFeatureBit<DestructionFeature::CookedSupport>;
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::ReplicationIncompatible);
            fence = Fence(state);
            fence.currentRevision = state.revision;
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::StaleRevision);
            fence = Fence(state);
            fence.session = {};  // Network owner has revoked session admission during shutdown.
            ExpectError(DecodeDestructionReplication(payload.Value(), fence, client, {}), DestructionErrors::ReplicationIncompatible);
            // Discarding a decoded candidate requires no rollback: neither encode nor decode owns live DFR state.
            const auto candidate = DecodeDestructionReplication(payload.Value(), Fence(state), client, {});
            REQUIRE(candidate.HasValue());
        }
    }  // namespace
}  // namespace Horo::Destruction
