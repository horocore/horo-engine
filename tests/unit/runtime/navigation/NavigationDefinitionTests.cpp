#include "Horo/Navigation/NavigationDefinitionSerialization.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Navigation {
    namespace {
        /** @brief Issues deterministic authoring IDs without tying them to input order or file names. */
        template <typename Identity> Identity Id(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        /** @brief Complete authored fixture; no production decoder synthesizes this policy. */
        NavigationDefinitionInput DefinitionInput() {
            const NavigationDescriptorSource source{.kind = NavigationDescriptorSourceKind::Project,
                                                    .id = Id<NavigationDescriptorSourceId>(20)};
            return {.profiles = {{.id = Id<NavigationAgentProfileId>(2), .displayName = "Large"},
                                 {.id = Id<NavigationAgentProfileId>(1), .displayName = "Grounded"}},
                    .areas = {{.id = Id<NavigationAreaId>(3), .source = source, .traversalCost = 1.5F, .flags = {1}}},
                    .filters = {{.id = Id<NavigationFilterId>(4),
                                 .source = source,
                                 .includedFlags = {1},
                                 .costOverrides = {{.area = Id<NavigationAreaId>(3), .traversalCost = 2.0F}}}},
                    .coordinates = {.system = NavigationSourceCoordinateSystem::RightHandedZUp, .metersPerUnit = 0.01F},
                    .tiles = {.tileSizeCells = 64, .maximumTiles = 128},
                    .scope = NavigationDefinitionScope::Project};
        }

        NavigationAuthoredRecord DefinitionRecord() {
            auto definition = NavigationDefinition::Create(DefinitionInput());
            REQUIRE(definition.HasValue());
            auto record = EncodeNavigationDefinitionRecord(definition.Value(), Id<NavigationAuthoredRecordId>(9));
            REQUIRE(record.HasValue());
            return std::move(record).Value();
        }
    }  // namespace

    TEST_CASE("navigation definition canonically round trips through the production HNAV support table", "[unit][navigation][definition]") {
        const auto record = DefinitionRecord();
        const std::array support{NavigationDefinitionRecordSupport()};
        const NavigationSourceLoadContext context{.supportedRecords = support};
        auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {record}, context);
        REQUIRE(source.HasValue());
        auto bytes = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(bytes.HasValue());
        auto decoded = DeserializeNavigationSourceRecords(bytes.Value(), context);
        REQUIRE(decoded.HasValue());
        auto definition = DecodeNavigationDefinitionRecord(decoded.Value().Records().front());
        REQUIRE(definition.HasValue());
        REQUIRE(definition.Value().Profiles().front().id == Id<NavigationAgentProfileId>(1));
        REQUIRE(definition.Value().Coordinates().system == NavigationSourceCoordinateSystem::RightHandedZUp);
        REQUIRE(definition.Value().Coordinates().metersPerUnit == 0.01F);
        REQUIRE(definition.Value().Tiles().tileSizeCells == 64);
        REQUIRE(definition.Value().Scope() == NavigationDefinitionScope::Project);
        REQUIRE(definition.Value().Registry().ResolveTraversal(Id<NavigationFilterId>(4), Id<NavigationAreaId>(3)).Value().traversalCost ==
                2.0F);
        REQUIRE(EncodeNavigationDefinitionRecord(definition.Value(), record.id).Value() == record);
        REQUIRE(DeserializeNavigationSourceRecords(bytes.Value()).HasError());
    }

    TEST_CASE("navigation definition rejects missing semantics and invalid stable descriptor references",
              "[unit][navigation][definition]") {
        auto input = DefinitionInput();
        SECTION("missing grounded profile") {
            input.profiles.clear();
        }
        SECTION("missing filter") {
            input.filters.clear();
        }
        SECTION("duplicate profile") {
            input.profiles.push_back(input.profiles.front());
        }
        SECTION("missing referenced area") {
            input.filters.front().costOverrides.front().area = Id<NavigationAreaId>(999);
        }
        SECTION("invalid coordinates") {
            input.coordinates.metersPerUnit = std::numeric_limits<float>::infinity();
        }
        REQUIRE(NavigationDefinition::Create(std::move(input)).HasError());
    }

    TEST_CASE("navigation definition rejects unsafe text and unqualified authoring limits", "[unit][navigation][definition]") {
        auto input = DefinitionInput();
        SECTION("zero grid") {
            input.tiles.tileSizeCells = 0;
        }
        SECTION("unbounded grid") {
            input.tiles.maximumTiles = 65537;
        }
        SECTION("invalid UTF8") {
            input.profiles.front().displayName = std::string(1, '\xff');
        }
        SECTION("embedded nul") {
            input.profiles.front().displayName = std::string("a\0b", 3);
        }
        SECTION("oversized name") {
            input.profiles.front().displayName.assign(257, 'a');
        }
        SECTION("oversized override table") {
            input.filters.front().costOverrides.resize(257);
        }
        REQUIRE(NavigationDefinition::Create(std::move(input)).HasError());
    }

    TEST_CASE("navigation definition never interprets opaque legacy or future required payloads", "[unit][navigation][definition]") {
        auto record = DefinitionRecord();
        SECTION("synthetic legacy type") {
            record.type = Id<NavigationAuthoredRecordTypeId>(100);
        }
        SECTION("future payload") {
            record.version = {1, 1};
        }
        SECTION("opaque optional") {
            record.required = false;
            record.opaque = true;
        }
        SECTION("invalid record identity") {
            record.id = {};
        }
        SECTION("trailing data") {
            record.payload.push_back(std::byte{0});
        }
        SECTION("truncated data") {
            record.payload.resize(23);
        }
        SECTION("hostile count") {
            std::fill(record.payload.begin() + 20, record.payload.begin() + 24, std::byte{0xff});
        }
        SECTION("unknown coordinate enum") {
            record.payload.front() = std::byte{0xff};
        }
        REQUIRE(DecodeNavigationDefinitionRecord(record).HasError());
    }

    TEST_CASE("navigation definition input order cannot change its durable canonical bytes", "[unit][navigation][definition]") {
        auto input = DefinitionInput();
        std::ranges::reverse(input.profiles);
        auto reordered = NavigationDefinition::Create(std::move(input));
        REQUIRE(reordered.HasValue());
        const auto reference = DefinitionRecord();
        REQUIRE(EncodeNavigationDefinitionRecord(reordered.Value(), reference.id).Value() == reference);
        REQUIRE(EncodeNavigationDefinitionRecord(reordered.Value(), {}).HasError());
    }
}  // namespace Horo::Navigation
