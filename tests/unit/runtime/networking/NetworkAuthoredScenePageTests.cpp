#include "Horo/Network/NetworkAuthoredScenePage.h"
#include "NetworkTestUtils.h"

namespace Horo::Network {
    namespace {
        using TestSupport::RequireError;

        /** @brief Builds the same initial ordering as the authority mapping allocator. */
        ReplicationAuthoredSceneInventory Inventory(std::size_t count) {
            ReplicationAuthoredSceneInventory inventory{NetworkSessionGeneration::Create(16).Value(),
                                                        ReplicationAuthorityEpoch::Create(28).Value()};
            const auto schema = ReplicationSchemaId::Create(53).Value();
            for (std::size_t i = 0; i < count; ++i)
                inventory.objects.push_back({NetworkObjectId::Create(inventory.authority, i + 1, 1).Value(),
                                             {schema, {2, 1}, Runtime::SceneObjectId{100 + i * 2}}});
            return inventory;
        }
    }  // namespace

    TEST_CASE("Authored Scene pages round-trip the full bounded inventory", "[unit][network][replication][authored]") {
        const auto inventory = Inventory(MaximumNetworkAuthoredSceneObjects);
        NetworkAuthoredSceneAssembly assembly{inventory.session, inventory.authority};
        for (std::size_t first = 0; first < inventory.objects.size();) {
            const auto page = MakeNetworkAuthoredScenePage(inventory, first).Value();
            const auto bytes = EncodeNetworkAuthoredScenePage(page).Value();
            REQUIRE(bytes.size() <= MaximumNetworkAuthoredScenePageBytes);
            const auto decoded = DecodeNetworkAuthoredScenePage(bytes).Value();
            REQUIRE(decoded == page);
            REQUIRE(assembly.Append(decoded).HasValue());
            first += page.objects.size();
            CHECK(assembly.IsComplete() == (first == inventory.objects.size()));
        }
        const auto restored = assembly.Take().Value();
        CHECK(restored.objects == inventory.objects);
        CHECK(restored.session == inventory.session);
        CHECK(restored.authority == inventory.authority);
        CHECK_FALSE(assembly.IsComplete());
        RequireError(assembly.Take(), NetworkErrors::ReplicationWorldUnavailable);
    }

    TEST_CASE("An explicit empty authored Scene page completes an empty inventory", "[unit][network][replication][authored]") {
        const auto inventory = Inventory(0);
        NetworkAuthoredSceneAssembly assembly{inventory.session, inventory.authority};
        CHECK_FALSE(assembly.IsComplete());
        const auto page = MakeNetworkAuthoredScenePage(inventory, 0).Value();
        const auto bytes = EncodeNetworkAuthoredScenePage(page).Value();
        CHECK(bytes.size() == MinimumNetworkAuthoredScenePageBytes);
        REQUIRE(assembly.Append(DecodeNetworkAuthoredScenePage(bytes).Value()).HasValue());
        RequireError(assembly.Append(page), NetworkErrors::MessageEnvelopeInvalid);
        REQUIRE(assembly.IsComplete());
        CHECK(assembly.Take().Value().objects.empty());
        RequireError(assembly.Append(page), NetworkErrors::ReplicationWorldStale);
    }

    TEST_CASE("Authored Scene pages reject noncanonical framing and provenance", "[unit][network][replication][authored]") {
        const auto inventory = Inventory(3);
        auto page = MakeNetworkAuthoredScenePage(inventory, 1).Value();
        auto valid = EncodeNetworkAuthoredScenePage(page).Value();
        for (std::size_t size = 0; size < valid.size(); ++size)
            RequireError(DecodeNetworkAuthoredScenePage(std::span{valid}.first(size)), NetworkErrors::MessageEnvelopeInvalid);
        SECTION("wire version") {
            valid[1] = std::byte{2};
        }
        SECTION("reserved bits") {
            valid[3] = std::byte{1};
        }
        SECTION("mismatched row count") {
            valid[25] = std::byte{1};
        }
        SECTION("trailing bytes") {
            valid.push_back(std::byte{});
        }
        SECTION("unassigned authority epoch") {
            valid[19] = std::byte{};
        }
        RequireError(DecodeNetworkAuthoredScenePage(valid), NetworkErrors::MessageEnvelopeInvalid);
        RequireError(DecodeNetworkAuthoredScenePage(std::vector<std::byte>(MaximumNetworkAuthoredScenePageBytes + 1)),
                     NetworkErrors::MessageEnvelopeCapacityExceeded);
        page.objects.front().object = page.objects.front().object.NextGeneration().Value();
        RequireError(EncodeNetworkAuthoredScenePage(page), NetworkErrors::MessageEnvelopeInvalid);
        page.objects.front() = inventory.objects[1];
        page.objects.back().provenance.authoredObject = page.objects.front().provenance.authoredObject;
        RequireError(EncodeNetworkAuthoredScenePage(page), NetworkErrors::MessageEnvelopeInvalid);
    }

    TEST_CASE("Authored Scene assembly preserves staged pages when later delivery is rejected", "[unit][network][replication][authored]") {
        const auto inventory = Inventory(4);
        NetworkAuthoredSceneAssembly assembly{inventory.session, inventory.authority};
        const auto first = MakeNetworkAuthoredScenePage(inventory, 0, 2).Value();
        const auto second = MakeNetworkAuthoredScenePage(inventory, 2, 1).Value();
        const auto third = MakeNetworkAuthoredScenePage(inventory, 3).Value();
        RequireError(assembly.Append(second), NetworkErrors::MessageEnvelopeInvalid);
        REQUIRE(assembly.Append(first).HasValue());
        RequireError(assembly.Append(first), NetworkErrors::MessageEnvelopeInvalid);
        RequireError(assembly.Append(third), NetworkErrors::MessageEnvelopeInvalid);
        RequireError(assembly.Take(), NetworkErrors::ReplicationWorldUnavailable);
        auto wrong = second;
        wrong.session = NetworkSessionGeneration::Create(17).Value();
        RequireError(assembly.Append(wrong), NetworkErrors::ReplicationWorldStale);
        wrong = second;
        wrong.totalObjects++;
        RequireError(assembly.Append(wrong), NetworkErrors::MessageEnvelopeInvalid);
        wrong = second;
        wrong.objects.front().provenance.authoredObject = first.objects.back().provenance.authoredObject;
        RequireError(assembly.Append(wrong), NetworkErrors::MessageEnvelopeInvalid);
        REQUIRE(assembly.Append(second).HasValue());
        REQUIRE(assembly.Append(third).HasValue());
        CHECK(assembly.Take().Value().objects == inventory.objects);
    }
}  // namespace Horo::Network
