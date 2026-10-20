#include "PrefabValueOverrideTestSupport.h"

#include <algorithm>

namespace Horo::Prefab {
    namespace {
        using namespace ValueTestSupport;

        /** @brief Retain exact provider-owned bytes rather than interpreting unknown component data in the codec. */
        RawComponentPayload ComponentPayload(const std::uint64_t instance, const std::string_view text = "{\"opaque\": [1, 2]}") {
            Gameplay::SerializedComponent component{.typeId = Type()};
            const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
            component.payload.assign(bytes.begin(), bytes.end());
            return {PrefabComponentInstanceId::Create(instance).Value(), std::move(component)};
        }

        /** @brief Build real pinned source/schema context while keeping all instance intent independently owned. */
        PrefabValueOverrideSet ComponentSet() {
            const auto source = Source();
            return {Asset(), source.Revision().rootSource, Schemas().Revision(), {}, {}, {}};
        }

        /** @brief Model a removed source occurrence with an exact stable identity; digest mismatch is a resolver concern. */
        PrefabComponentOverrideRecord Removal(const std::uint64_t instance) {
            return PrefabRemoveComponentOverride{{Object(), Type(), PrefabComponentInstanceId::Create(instance).Value()}, 1, {}};
        }

        TEST_CASE("Component add remove conflict and orphan records round trip opaque bytes in one envelope",
                  "[unit][prefab][override][codec]") {
            auto set = ComponentSet();
            const auto added = ComponentPayload(2);
            set.componentRecords = {PrefabAddComponentOverride{Object(), added}, Removal(3)};
            auto observed = set.authoredAgainst;
            observed.contentDigest.bytes.front() = 17;
            set.componentConflicts.emplace_back(Removal(5), set.authoredAgainst, set.schemaRevision, Sha256Digest{}, observed);
            set.componentOrphans.emplace_back(PrefabAddComponentOverride{Object(), ComponentPayload(4)}, set.authoredAgainst,
                                              set.schemaRevision, PrefabComponentOverrideIssue::SchemaUnavailable);
            const auto bytes = SerializePrefabValueOverrides(set, Limits());
            REQUIRE(bytes.HasValue());
            const auto decoded = ParsePrefabValueOverrides(bytes.Value(), Limits());
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value() == set);
            CHECK(decoded.Value().HasUnresolved());
            CHECK_FALSE(decoded.Value().IsEmpty());
            CHECK(std::get<PrefabAddComponentOverride>(decoded.Value().componentRecords.front()).payload == added);
            CHECK(SerializePrefabValueOverrides(decoded.Value(), Limits()).Value() == bytes.Value());
        }

        TEST_CASE("Mixed value and component records have canonical order independent from typed-vector arrival",
                  "[unit][prefab][override][codec]") {
            auto set = ComponentSet();
            set.records.emplace_back(Property(), 1, Sha256Digest{}, 3.0);
            set.componentRecords = {Removal(3), PrefabAddComponentOverride{Object(), ComponentPayload(2)}};
            const auto first = SerializePrefabValueOverrides(set, Limits());
            REQUIRE(first.HasValue());
            std::ranges::reverse(set.componentRecords);
            CHECK(SerializePrefabValueOverrides(set, Limits()).Value() == first.Value());
            const auto decoded = ParsePrefabValueOverrides(first.Value(), Limits());
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value().records.size() == 1);
            CHECK(decoded.Value().componentRecords.size() == 2);
            CHECK(SerializePrefabValueOverrides(decoded.Value(), Limits()).Value() == first.Value());
            CHECK_FALSE(decoded.Value().HasUnresolved());
        }

        TEST_CASE("Component occurrence claims reject duplicate types operations and same-layer value patches",
                  "[unit][prefab][override][codec]") {
            auto set = ComponentSet();
            set.componentRecords.emplace_back(PrefabAddComponentOverride{Object(), ComponentPayload(1)});
            SECTION("duplicate addition") {
                set.componentRecords.push_back(set.componentRecords.front());
            }
            SECTION("add and remove") {
                set.componentRecords.push_back(Removal(1));
            }
            SECTION("different types cannot share an occurrence") {
                auto other = ComponentPayload(1);
                other.component.typeId = Gameplay::ComponentTypeId::Parse("game.override.other").Value();
                set.componentRecords.emplace_back(PrefabAddComponentOverride{Object(), std::move(other)});
            }
            SECTION("ordinary property patch") {
                set.records.emplace_back(Property(), 1, Sha256Digest{}, 3.0);
            }
            SECTION("preserved property patch") {
                set.orphans.emplace_back(PrefabValueOverrideRecord{Property(), 1, {}, 3.0}, set.authoredAgainst, set.schemaRevision,
                                         PrefabValueOverrideIssue::SchemaUnavailable);
            }
            SECTION("normal and preserved component intent") {
                set.componentOrphans.emplace_back(set.componentRecords.front(), set.authoredAgainst, set.schemaRevision,
                                                  PrefabComponentOverrideIssue::TargetRemoved);
            }
            const auto validated = ValidatePrefabValueOverrideSet(set, Limits());
            REQUIRE(validated.HasError());
            CHECK(validated.ErrorValue().code.Value() == PrefabErrors::OverrideConflict.code.Value());
            CHECK(SerializePrefabValueOverrides(set, Limits()).HasError());
        }

        TEST_CASE("Component override decoding rejects unknown operation ranks and non-byte payload data",
                  "[unit][prefab][override][codec]") {
            auto set = ComponentSet();
            set.componentRecords.emplace_back(PrefabAddComponentOverride{Object(), ComponentPayload(1)});
            const auto encoded = SerializePrefabValueOverrides(set, Limits());
            REQUIRE(encoded.HasValue());
            auto bytes = encoded.Value();
            SECTION("unknown operation") {
                const std::string marker = "\"operation\": 5";
                const auto position = bytes.find(marker);
                REQUIRE(position != std::string::npos);
                bytes.replace(position, marker.size(), "\"operation\": 7");
            }
            SECTION("oversized byte") {
                const std::string marker = "\"bytes\": [";
                const auto position = bytes.find(marker);
                REQUIRE(position != std::string::npos);
                bytes.insert(position + marker.size(), "256,");
            }
            CHECK(ParsePrefabValueOverrides(bytes, Limits()).HasError());
            CHECK(std::get<PrefabAddComponentOverride>(set.componentRecords.front()).payload == ComponentPayload(1));
        }

        TEST_CASE("Value and component records share the same aggregate count and encoded byte budgets",
                  "[unit][prefab][override][codec]") {
            auto set = ComponentSet();
            set.records.emplace_back(Property(), 1, Sha256Digest{}, 3.0);
            set.componentRecords.emplace_back(PrefabAddComponentOverride{Object(), ComponentPayload(2)});
            auto policy = Limits().Policy();
            SECTION("shared record count") {
                policy.maximumOverrideRecords = 1;
                policy.maximumConflictAndOrphanRecords = 1;
            }
            SECTION("standalone opaque record bytes") {
                policy.maximumOverrideValueBytes = 64;
            }
            const auto limits = PrefabLimitProfile::Create(policy);
            REQUIRE(limits.HasValue());
            const auto validated = ValidatePrefabValueOverrideSet(set, limits.Value());
            REQUIRE(validated.HasError());
            CHECK(validated.ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
        }
    }  // namespace
}  // namespace Horo::Prefab
