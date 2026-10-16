#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabValueOverrideTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Prefab {
    namespace {
        using namespace ValueTestSupport;

        /** @brief Resolve actual parent or repeated nested source composition without provider mocks. */
        EffectivePrefabCandidate ComposedSource(const bool variant) {
            const auto base = Sources();
            const auto original = base.Sources()[0];
            auto bytes = std::array<std::uint8_t, 16>{};
            bytes.back() = 92;
            const auto outer = Assets::AssetId::FromBytes(bytes);
            Assets::AssetRegistry registry;
            REQUIRE(
                registry
                    .Publish({{Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(),
                               ProjectPath::Parse("assets/value.prefab").Value(), ProjectPath::Parse("assets/value.prefab.horo").Value()},
                              {outer, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse("assets/outer.prefab").Value(),
                               ProjectPath::Parse("assets/outer.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
            PrefabComposition composition;
            std::vector<PrefabObjectNode> objects;
            if (variant) {
                composition.variantParent = PrefabAssetReference::Create(Asset()).Value();
                composition.variantAuthoredAgainst = original.sourceRevision;
            } else {
                objects.push_back({.localId = {0}, .name = "Outer"});
                for (const auto slot : {3U, 4U})
                    composition.nestedPlacements.push_back({{slot},
                                                            {},
                                                            PrefabAssetReference::Create(Asset()).Value(),
                                                            original.sourceRevision,
                                                            Math::Transform{.translation = {10, 0, 0}}});
            }
            auto document = PrefabDocument::Create({.projectVersion = original.sourceRevision.projectVersion,
                                                    .assetId = outer,
                                                    .objects = std::move(objects),
                                                    .composition = std::move(composition),
                                                    .referencedAssets = {Asset()}},
                                                   Limits());
            REQUIRE(document.HasValue());
            auto revision = original.sourceRevision;
            revision.contentDigest.bytes.front() = 1;
            auto sources =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{original}, {std::move(document).Value(), revision}}, Limits());
            REQUIRE(sources.HasValue());
            return sources.Value().Resolve(outer, PrefabInstanceId::Create(9).Value(), Limits()).Value();
        }

        /** @brief Known canonical identity order exercises every persisted comparison dimension. */
        std::vector<PrefabValueOverrideTarget> CanonicalTargets() {
            const auto property = [](const PrefabObjectAddress &object, const char *type, std::uint32_t occurrence,
                                     std::uint32_t id) -> PrefabValueOverrideTarget {
                const auto component = Gameplay::ComponentTypeId::Parse(type);
                REQUIRE(component.HasValue());
                return PrefabPropertyAddress::Create(object, component.Value(), PrefabComponentInstanceId::Create(occurrence).Value(),
                                                     PrefabPropertyId::Create(id).Value())
                    .Value();
            };
            const std::array scope3{LocalObjectId{3}}, scope4{LocalObjectId{4}};
            return std::vector<PrefabValueOverrideTarget>{Object(),
                                                          property(Object(), "game.order.a", 2, 1),
                                                          property(Object(), "game.order.a", 3, 1),
                                                          property(Object(), "game.order.z", 1, 1),
                                                          property(Object(), "game.order.z", 1, 2),
                                                          property(PrefabObjectAddress::Create({}, {1}).Value(), "game.order.a", 1, 1),
                                                          property(PrefabObjectAddress::Create(scope3, {0}).Value(), "game.order.a", 1, 1),
                                                          property(PrefabObjectAddress::Create(scope4, {0}).Value(), "game.order.a", 1, 1)};
        }

        TEST_CASE("Inherited values and repeated nested scopes preserve independent stable override identity",
                  "[unit][prefab][override][identity]") {
            const auto schemas = Schemas();
            const auto inherited = ComposedSource(true);
            auto inspected =
                PreparePrefabValueEdit(inherited, std::nullopt, {PrefabValueEditKind::Rebase, Object(), false}, &schemas, Limits());
            REQUIRE(inspected.HasValue());
            CHECK(inspected.Value().provenance.back().origin == PrefabValueOrigin::Inherited);
            CHECK(inspected.Value().provenance.back().sourcePrefab == Asset());
            const auto nested = ComposedSource(false);
            const std::array scope{LocalObjectId{3}};
            const auto target = PrefabObjectAddress::Create(scope, {0}).Value();
            Math::Transform moved{.translation = {3, 0, 0}};
            auto changed = PreparePrefabValueEdit(nested, std::nullopt, {PrefabValueEditKind::Assign, target, moved}, &schemas, Limits());
            REQUIRE(changed.HasValue());
            REQUIRE(changed.Value().effective);
            CHECK(changed.Value().effective->Objects()[1].effectiveLocalTransform.translation == Math::Vec3{13, 0, 0});
            CHECK(changed.Value().effective->Objects()[2].effectiveLocalTransform.translation == Math::Vec3{10, 0, 0});
            CHECK(nested.Objects()[1].effectiveLocalTransform.translation == Math::Vec3{10, 0, 0});
        }

        TEST_CASE("Prefab typed value edits resolve real source components with independent provenance", "[unit][prefab][override]") {
            const auto source = Source();
            const auto schemas = Schemas();
            auto prepared =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 5.0}, &schemas, Limits());
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value().effective);
            CHECK(prepared.Value().rebased.records.size() == 1);
            CHECK(source.ValueOverridesDigest() == Sha256Digest{});
            CHECK(prepared.Value().effective->ValueOverridesDigest() != Sha256Digest{});
            const auto &provenance = prepared.Value().provenance;
            REQUIRE(provenance.size() == 2);
            const auto &property = provenance.back();
            CHECK(property.origin == PrefabValueOrigin::Instance);
            CHECK(std::get<double>(property.sourceValue) == 2.0);
            CHECK(std::get<double>(property.effectiveValue) == 5.0);
            CHECK(std::get<double>(*property.schemaDefault) == 1.0);
            const auto &bytes = prepared.Value().effective->Objects()[0].object.components[0].component.payload;
            const std::string encoded(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            CHECK(encoded == "{\"speed\":5.0}");
            const auto wire = SerializePrefabValueOverrides(prepared.Value().rebased, Limits());
            REQUIRE(wire.HasValue());
            const auto parsed = ParsePrefabValueOverrides(wire.Value(), Limits());
            REQUIRE(parsed.HasValue());
            CHECK(parsed.Value() == prepared.Value().rebased);
            auto reverted =
                PreparePrefabValueEdit(source, parsed.Value(), {PrefabValueEditKind::Revert, Property(), false}, &schemas, Limits());
            REQUIRE(reverted.HasValue());
            REQUIRE(reverted.Value().effective);
            CHECK(reverted.Value().rebased.records.empty());
            CHECK(reverted.Value().effective->Objects()[0].object.components == source.Objects()[0].object.components);
        }

        TEST_CASE("Prefab value rebase preserves source conflicts and module-unavailable orphan intent",
                  "[unit][prefab][override][lifecycle]") {
            const auto schemas = Schemas();
            const auto source = Source();
            auto assigned =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 5.0}, &schemas, Limits()).Value();
            const auto changed = Source("{\"speed\":3.0}", 2);
            auto conflict = ResolvePrefabValueOverrides(changed, assigned.rebased, &schemas, Limits());
            REQUIRE(conflict.HasValue());
            CHECK_FALSE(conflict.Value().effective);
            REQUIRE(conflict.Value().rebased.conflicts.size() == 1);
            CHECK(conflict.Value().rebased.conflicts[0].original == assigned.rebased.records[0]);
            auto unavailable = ResolvePrefabValueOverrides(source, assigned.rebased, nullptr, Limits());
            REQUIRE(unavailable.HasValue());
            CHECK_FALSE(unavailable.Value().effective);
            REQUIRE(unavailable.Value().rebased.orphans.size() == 1);
            auto removed = ResolvePrefabValueOverrides(Source("{}", 2), assigned.rebased, &schemas, Limits());
            REQUIRE(removed.HasValue());
            REQUIRE(removed.Value().rebased.orphans.size() == 1);
            CHECK(removed.Value().rebased.orphans[0].reason == PrefabValueOverrideIssue::TargetRemoved);
            auto changedProperties = std::vector<PrefabValuePropertySchema>{schemas.Properties().begin(), schemas.Properties().end()};
            changedProperties[0].maximum = 9.0;
            const auto newSchemas = PrefabValueSchemaSnapshot::Create(std::move(changedProperties), Limits()).Value();
            auto schemaChanged = ResolvePrefabValueOverrides(source, assigned.rebased, &newSchemas, Limits());
            REQUIRE(schemaChanged.HasValue());
            REQUIRE(schemaChanged.Value().rebased.orphans.size() == 1);
            CHECK(schemaChanged.Value().rebased.orphans[0].reason == PrefabValueOverrideIssue::SchemaChanged);
            const auto preserved = SerializePrefabValueOverrides(unavailable.Value().rebased, Limits()).Value();
            auto restored = ResolvePrefabValueOverrides(source, ParsePrefabValueOverrides(preserved, Limits()).Value(), &schemas, Limits());
            REQUIRE(restored.HasValue());
            REQUIRE(restored.Value().effective);
            CHECK(restored.Value().rebased.orphans.empty());
            auto redundant = ResolvePrefabValueOverrides(Source("{\"speed\":5.0}", 3), assigned.rebased, &schemas, Limits());
            REQUIRE(redundant.HasValue());
            REQUIRE(redundant.Value().effective);
            CHECK(redundant.Value().redundantRecords == 1);
            CHECK(redundant.Value().rebased.records.empty());
        }

        TEST_CASE("Prefab override validation rejects malformed values duplicates budgets and cancellation atomically",
                  "[unit][prefab][override][boundary]") {
            const auto source = Source();
            const auto schemas = Schemas();
            for (PrefabOverrideValue value :
                 {PrefabOverrideValue{true}, PrefabOverrideValue{11.0}, PrefabOverrideValue{std::numeric_limits<double>::infinity()}})
                CHECK(PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), std::move(value)}, &schemas,
                                             Limits())
                          .HasError());
            auto assigned =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 5.0}, &schemas, Limits()).Value();
            auto duplicate = assigned.rebased;
            duplicate.records.push_back(duplicate.records[0]);
            CHECK(ValidatePrefabValueOverrideSet(duplicate, Limits()).HasError());
            PrefabProjectPolicy policy;
            policy.maximumOverrideValueBytes = 1;
            const auto tiny = PrefabLimitProfile::Create(policy);
            REQUIRE(tiny.HasValue());
            CHECK(SerializePrefabValueOverrides(assigned.rebased, tiny.Value()).HasError());
            auto wire = SerializePrefabValueOverrides(assigned.rebased, Limits()).Value();
            CHECK(ParsePrefabValueOverrides("{\"schema\":1,\"schema\":1}", Limits()).HasError());
            CHECK(ParsePrefabValueOverrides(wire + " ", Limits()).HasError());
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            CHECK(ResolvePrefabValueOverrides(source, assigned.rebased, &schemas, Limits(), cancellation.Token()).HasError());
            CHECK(source.ValueOverridesDigest() == Sha256Digest{});
        }

        TEST_CASE("Prefab override canonical order compares object scope component type occurrence and property",
                  "[unit][prefab][override][identity]") {
            const auto source = Source();
            const auto schemas = Schemas();
            auto set = PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 5.0}, &schemas, Limits())
                           .Value()
                           .rebased;
            const auto propertyRecord = set.records.front();
            const auto rootRecord =
                PreparePrefabValueEdit(source, std::nullopt,
                                       {PrefabValueEditKind::Assign, Object(), Math::Transform{.translation = {1, 0, 0}}}, &schemas,
                                       Limits())
                    .Value()
                    .rebased.records.front();
            const auto expected = CanonicalTargets();
            set.records.clear();
            for (const auto &target : expected) {
                auto record = std::holds_alternative<PrefabObjectAddress>(target) ? rootRecord : propertyRecord;
                record.target = target;
                set.records.push_back(std::move(record));
            }
            const auto canonical = SerializePrefabValueOverrides(set, Limits());
            REQUIRE(canonical.HasValue());
            std::ranges::reverse(set.records);
            CHECK(SerializePrefabValueOverrides(set, Limits()).Value() == canonical.Value());
            const auto parsed = ParsePrefabValueOverrides(canonical.Value(), Limits());
            REQUIRE(parsed.HasValue());
            REQUIRE(parsed.Value().records.size() == expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i)
                CHECK(parsed.Value().records[i].target == expected[i]);
        }

        TEST_CASE("Prefab transforms preserve absent properties without applying future registry defaults", "[unit][prefab][override]") {
            const auto source = Source("{}");
            const auto schemas = Schemas();
            auto initial =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Rebase, Object(), false}, &schemas, Limits()).Value();
            CHECK(initial.provenance.size() == 1);
            CHECK(std::get<double>(schemas.Properties()[0].defaultValue) == 1.0);
            CHECK(PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Property(), 5.0}, &schemas, Limits())
                      .HasError());
            Math::Transform transform;
            transform.translation = {1, 2, 3};
            auto assigned =
                PreparePrefabValueEdit(source, std::nullopt, {PrefabValueEditKind::Assign, Object(), transform}, &schemas, Limits());
            REQUIRE(assigned.HasValue());
            REQUIRE(assigned.Value().effective);
            CHECK(assigned.Value().effective->Objects()[0].effectiveLocalTransform == transform);
            CHECK(source.Objects()[0].effectiveLocalTransform == Math::Transform{});
        }
    }  // namespace
}  // namespace Horo::Prefab
