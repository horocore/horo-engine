#include "Horo/Network/NetworkPrefabLifecycle.h"
#include "NetworkTestUtils.h"

namespace Horo::Network {
    namespace {
        using TestSupport::RequireError;

        NetworkPrefabLifecycleRecord SparseSpawn() {
            const auto epoch = ReplicationAuthorityEpoch::Create(3).Value();
            const auto occurrence = NetworkObjectId::Create(epoch, 8, 2).Value();
            const auto other = NetworkObjectId::Create(epoch, 2, 7).Value();
            const auto schema = ReplicationSchemaId::Create(53).Value();
            return {NetworkSessionGeneration::Create(9).Value(), occurrence,
                    NetworkPrefabSpawnRecord{{Assets::AssetId::FromBytes({1}), Sha256Digest{{2}}, 3},
                                             {{0, occurrence, schema, {1, 0}}, {2, other, schema, {1, 7}}}}};
        }
    }  // namespace

    TEST_CASE("Prefab wire records round-trip sparse spawn and exact generation despawn", "[unit][network][replication][prefab]") {
        auto record = SparseSpawn();
        SECTION("spawn retains sparse recipe slots and independent network slot generations") {}
        SECTION("despawn contains only the exact occurrence") {
            record.spawn.reset();
        }
        const auto bytes = EncodeNetworkPrefabLifecycle(record).Value();
        CHECK(bytes.size() == (record.spawn ? 142 : 32));
        CHECK(DecodeNetworkPrefabLifecycle(bytes).Value() == record);
        for (std::size_t size = 0; size < bytes.size(); ++size)
            RequireError(DecodeNetworkPrefabLifecycle(std::span(bytes).first(size)), NetworkErrors::MessageEnvelopeInvalid);
        auto extended = bytes;
        extended.push_back(std::byte{});
        RequireError(DecodeNetworkPrefabLifecycle(extended), NetworkErrors::MessageEnvelopeInvalid);
    }

    TEST_CASE("Prefab wire codec rejects invalid framing and conflicting object identities", "[unit][network][replication][prefab]") {
        auto record = SparseSpawn();
        const auto reject = [&] {
            RequireError(EncodeNetworkPrefabLifecycle(record), NetworkErrors::MessageEnvelopeInvalid);
        };
        SECTION("duplicate prepared slot") {
            record.spawn->members.back().entitySlot = record.spawn->members.front().entitySlot;
            reject();
        }
        SECTION("duplicate object slot with another generation") {
            record.spawn->members.back().object = record.occurrence.NextGeneration().Value();
            reject();
        }
        SECTION("occurrence does not anchor the first member") {
            record.occurrence = record.spawn->members.back().object;
            reject();
        }
        SECTION("unknown version or record kind") {
            auto bytes = EncodeNetworkPrefabLifecycle(record).Value();
            bytes[1] = std::byte{2};
            RequireError(DecodeNetworkPrefabLifecycle(bytes), NetworkErrors::MessageEnvelopeInvalid);
            bytes[1] = std::byte{1};
            bytes[3] = std::byte{3};
            RequireError(DecodeNetworkPrefabLifecycle(bytes), NetworkErrors::MessageEnvelopeInvalid);
        }
        SECTION("byte budget is enforced before parsing") {
            RequireError(DecodeNetworkPrefabLifecycle(std::vector<std::byte>(MaximumNetworkPrefabLifecycleBytes + 1)),
                         NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
        SECTION("declared member count must match the available bytes") {
            auto bytes = EncodeNetworkPrefabLifecycle(record).Value();
            constexpr std::size_t memberCountLowByte = 32 + 16 + 32 + 4 + 1;
            bytes[memberCountLowByte] = std::byte{3};
            RequireError(DecodeNetworkPrefabLifecycle(bytes), NetworkErrors::MessageEnvelopeInvalid);
        }
    }

    TEST_CASE("Prefab wire codec accepts the complete bounded member inventory", "[unit][network][replication][prefab]") {
        auto record = SparseSpawn();
        record.spawn->members.resize(MaximumNetworkPrefabEntities);
        record.spawn->recipe.entityCount = static_cast<std::uint32_t>(MaximumNetworkPrefabEntities);
        const auto schema = record.spawn->members.front().schema;
        for (std::uint32_t slot = 0; slot < record.spawn->recipe.entityCount; ++slot)
            record.spawn->members[slot] = {slot, NetworkObjectId::Create(record.occurrence.Epoch(), 8 + slot, 2).Value(), schema, {1, 0}};
        const auto bytes = EncodeNetworkPrefabLifecycle(record).Value();
        CHECK(bytes.size() == MaximumNetworkPrefabLifecycleBytes);
        CHECK(DecodeNetworkPrefabLifecycle(bytes).Value() == record);
    }
}  // namespace Horo::Network
