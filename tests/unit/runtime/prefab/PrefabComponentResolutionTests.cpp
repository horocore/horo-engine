#include "Horo/Prefab/PrefabValueSnapshot.h"
#include "PrefabValueOverrideTestSupport.h"

namespace Horo::Prefab {
    namespace {
        using namespace ValueTestSupport;

        /** @brief Explicit complete topology policy; ordinary value schemas never imply attachment permission. */
        PrefabValueSchemaSnapshot TopologySchema(const std::uint32_t maximum = 1, const bool removal = true) {
            Gameplay::ComponentDescriptor descriptor{Type(),
                                                     1,
                                                     "Values",
                                                     "Tests",
                                                     {{Gameplay::ComponentPropertyId::Parse("speed").Value(), "Speed",
                                                       Gameplay::ComponentPropertyKind::Number, true}},
                                                     {}};
            return PrefabValueSchemaSnapshot::Create({}, Limits(), {{std::move(descriptor), maximum, true, removal, {}, {}, {}}}).Value();
        }

        RawComponentPayload Added(const std::uint64_t id = 2, const std::string_view text = "{\"speed\":3.0}") {
            RawComponentPayload payload{PrefabComponentInstanceId::Create(id).Value(), {.typeId = Type()}};
            const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
            payload.component.payload.assign(bytes.begin(), bytes.end());
            return payload;
        }

        PrefabComponentOverrideTarget Target(const std::uint64_t id = 1) {
            return {Object(), Type(), PrefabComponentInstanceId::Create(id).Value()};
        }

        TEST_CASE("Immutable prefab diffs retain supported component effects conflicts and orphans without phantom effective properties",
                  "[prefab][value-snapshot][topology]") {
            const auto values = Schemas();
            const auto topology = TopologySchema();
            auto properties = std::vector<PrefabValuePropertySchema>{values.Properties().begin(), values.Properties().end()};
            properties[0].required = true;
            auto createdSchema = PrefabValueSchemaSnapshot::Create(std::move(properties), Limits(),
                                                                   {topology.Components().begin(), topology.Components().end()});
            REQUIRE(createdSchema.HasValue());
            const auto schema = std::move(createdSchema).Value();
            const auto source = Source();
            const auto removed = PreparePrefabComponentEdit(source, {}, PrefabRemoveComponentEdit{Target()}, &schema, Limits()).Value();
            const auto snapshot = PrefabValueSnapshot::Prepare(source, removed.rebased, &schema, Limits()).Value().snapshot;
            REQUIRE(snapshot->Components().size() == 1);
            CHECK(std::holds_alternative<PrefabComponentOverrideRecord>(snapshot->Components()[0]));
            CHECK(snapshot->Values().back().disposition == PrefabValueDisposition::RemovedByInstance);
            CHECK(snapshot->Values().back().origin == PrefabValueOrigin::Instance);
            const auto removedAssignment =
                PreparePrefabValueEdit(source, removed.rebased, {PrefabValueEditKind::Assign, Property(), 7.0}, &schema, Limits());
            REQUIRE(removedAssignment.HasError());
            CHECK(removedAssignment.ErrorValue().code.Value() == PrefabErrors::OverrideOrphan.code.Value());
            const auto assignment =
                PreparePrefabValueEdit(source, {}, {PrefabValueEditKind::Assign, Property(), 7.0}, &schema, Limits()).Value();
            auto conflicting = removed.rebased;
            conflicting.records = assignment.rebased.records;
            CHECK(ResolvePrefabValueOverrides(source, conflicting, &schema, Limits()).HasError());
            const auto replaced =
                PreparePrefabComponentEdit(source, {}, PrefabReplaceComponentEdit{Target(), Added()}, &schema, Limits()).Value();
            const auto replacement = PrefabValueSnapshot::Prepare(source, replaced.rebased, &schema, Limits()).Value();
            CHECK(replacement.snapshot->Components().size() == 2);
            REQUIRE(replacement.resolution.effective);
            CHECK(replacement.resolution.effective->Objects()[0].object.components == std::vector{Added()});
            const auto conflict =
                PrefabValueSnapshot::Prepare(Source("{\"speed\":4.0}", 2), removed.rebased, &schema, Limits()).Value().snapshot;
            REQUIRE(conflict->Components().size() == 1);
            CHECK(std::holds_alternative<PrefabComponentOverrideConflict>(conflict->Components()[0]));
            CHECK(conflict->Values().back().disposition == PrefabValueDisposition::Present);
            const auto orphan = PrefabValueSnapshot::Prepare(source, removed.rebased, nullptr, Limits()).Value().snapshot;
            REQUIRE(orphan->Components().size() == 1);
            CHECK(std::holds_alternative<PrefabComponentOverrideOrphan>(orphan->Components()[0]));
        }

        TEST_CASE("Atomic component replacement removes before adding at singleton capacity", "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto schema = TopologySchema();
            const auto replacement = Added();
            const auto prepared =
                PreparePrefabComponentEdit(source, {}, PrefabReplaceComponentEdit{Target(), replacement}, &schema, Limits());
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value().effective);
            CHECK(prepared.Value().rebased.componentRecords.size() == 2);
            CHECK(prepared.Value().effective->Objects().front().object.components == std::vector{replacement});
            CHECK(source.Objects().front().object.components.front().instance == Target().instance);
            const auto bytes = SerializePrefabValueOverrides(prepared.Value().rebased, Limits());
            REQUIRE(bytes.HasValue());
            const auto decoded = ParsePrefabValueOverrides(bytes.Value(), Limits());
            REQUIRE(decoded.HasValue());
            CHECK(ResolvePrefabValueOverrides(source, decoded.Value(), &schema, Limits()).Value().effective.has_value());
            const auto reverted = PreparePrefabComponentEdit(source, prepared.Value().rebased,
                                                             PrefabRevertComponentOverridesEdit{{Target(), Target(2)}}, &schema, Limits());
            REQUIRE(reverted.HasValue());
            CHECK(reverted.Value().rebased.IsEmpty());
            CHECK(reverted.Value().effective->Objects().front().object.components == source.Objects().front().object.components);
        }

        TEST_CASE("Topology authoring rejects forbidden duplicate and malformed complete payloads", "[unit][prefab][override][topology]") {
            const auto source = Source();
            auto schema = TopologySchema();
            PrefabComponentEdit edit = PrefabReplaceComponentEdit{Target(), Added()};
            SECTION("fresh occurrence required") {
                edit = PrefabReplaceComponentEdit{Target(), Added(1)};
            }
            SECTION("singleton add") {
                edit = PrefabAddComponentOverride{Object(), Added()};
            }
            SECTION("source collision") {
                edit = PrefabAddComponentOverride{Object(), Added(1)};
            }
            SECTION("required property") {
                edit = PrefabReplaceComponentEdit{Target(), Added(2, "{}")};
            }
            SECTION("wrong kind") {
                edit = PrefabReplaceComponentEdit{Target(), Added(2, "{\"speed\":true}")};
            }
            SECTION("duplicate key") {
                edit = PrefabReplaceComponentEdit{Target(), Added(2, "{\"speed\":1,\"speed\":2}")};
            }
            SECTION("no removal permission") {
                schema = TopologySchema(1, false);
            }
            CHECK(PreparePrefabComponentEdit(source, {}, edit, &schema, Limits()).HasError());
            CHECK(source.Objects().front().object.components.size() == 1);
        }

        TEST_CASE("Removal retains exact source conflicts and unavailable schema evidence across reload",
                  "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto schema = TopologySchema();
            const auto removed = PreparePrefabComponentEdit(source, {}, PrefabRemoveComponentEdit{Target()}, &schema, Limits());
            REQUIRE(removed.HasValue());
            CHECK(removed.Value().effective->Objects().front().object.components.empty());
            const auto changed = Source("{\"speed\":4.0}", 2);
            const auto conflict = ResolvePrefabValueOverrides(changed, removed.Value().rebased, &schema, Limits());
            REQUIRE(conflict.HasValue());
            CHECK_FALSE(conflict.Value().effective);
            REQUIRE(conflict.Value().rebased.componentConflicts.size() == 1);
            const auto unavailable = ResolvePrefabValueOverrides(changed, conflict.Value().rebased, nullptr, Limits());
            REQUIRE(unavailable.HasValue());
            REQUIRE(unavailable.Value().rebased.componentOrphans.size() == 1);
            CHECK(unavailable.Value().rebased.componentOrphans.front().reason == PrefabComponentOverrideIssue::SchemaUnavailable);
            CHECK(unavailable.Value().rebased.componentOrphans.front().authoredAgainst == source.Revision().rootSource);
            const auto recovered = ResolvePrefabValueOverrides(source, unavailable.Value().rebased, &schema, Limits());
            REQUIRE(recovered.HasValue());
            CHECK(recovered.Value().effective.has_value());
            CHECK_FALSE(recovered.Value().rebased.HasUnresolved());
        }

        TEST_CASE("Removing and replacing instance additions coalesces intent without source tombstones",
                  "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto schema = TopologySchema(2);
            const auto added = PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), Added()}, &schema, Limits());
            REQUIRE(added.HasValue());
            const auto replaced = PreparePrefabComponentEdit(source, added.Value().rebased, PrefabReplaceComponentEdit{Target(2), Added(3)},
                                                             &schema, Limits());
            REQUIRE(replaced.HasValue());
            REQUIRE(replaced.Value().rebased.componentRecords.size() == 1);
            CHECK(std::holds_alternative<PrefabAddComponentOverride>(replaced.Value().rebased.componentRecords.front()));
            const auto removed =
                PreparePrefabComponentEdit(source, replaced.Value().rebased, PrefabRemoveComponentEdit{Target(3)}, &schema, Limits());
            REQUIRE(removed.HasValue());
            CHECK(removed.Value().rebased.IsEmpty());
        }

        TEST_CASE("Replacement cannot reuse a peer instance addition identity", "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto schema = TopologySchema(3);
            const auto first = PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), Added(2)}, &schema, Limits());
            REQUIRE(first.HasValue());
            const auto peer = PreparePrefabComponentEdit(source, first.Value().rebased, PrefabAddComponentOverride{Object(), Added(3)},
                                                         &schema, Limits());
            REQUIRE(peer.HasValue());
            CHECK(
                PreparePrefabComponentEdit(source, peer.Value().rebased, PrefabReplaceComponentEdit{Target(2), Added(3)}, &schema, Limits())
                    .HasError());
            CHECK(peer.Value().rebased.componentRecords.size() == 2);
            CHECK(peer.Value().effective->Objects()[0].object.components.size() == 3);
        }

        TEST_CASE("Component dependency and exclusion policies validate the final candidate", "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto extraType = Gameplay::ComponentTypeId::Parse("game.override.extra").Value();
            PrefabComponentOverridePolicy existing{{Type(), 1, "Values", "Tests", {}, {}}, 1, true, true, {}, {}, {}};
            PrefabComponentOverridePolicy extra{{extraType, 1, "Extra", "Tests", {}, {}}, 1, true, true, {}, {}, {}};
            auto added = Added();
            added.component.typeId = extraType;
            SECTION("missing required component") {
                existing.requiredComponents = {extraType};
            }
            SECTION("incompatible existing component") {
                extra.incompatibleComponents = {Type()};
            }
            const auto schema = PrefabValueSchemaSnapshot::Create({}, Limits(), {existing, extra}).Value();
            if (!existing.requiredComponents.empty()) {
                const auto ok = PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), added}, &schema, Limits());
                REQUIRE(ok.HasValue());
                CHECK(PreparePrefabComponentEdit(source, ok.Value().rebased,
                                                 PrefabRemoveComponentEdit{{Object(), extraType, added.instance}}, &schema, Limits())
                          .HasError());
            } else
                CHECK(PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), added}, &schema, Limits()).HasError());
        }

        TEST_CASE("Behavior requirements and exclusions participate in component topology admission",
                  "[unit][prefab][override][topology]") {
            const auto initial = Sources();
            auto data = initial.Sources()[0].document.Data();
            const auto behaviorType = Gameplay::BehaviorTypeId::Parse("game.override.behavior").Value();
            data.objects[0].behaviors.push_back({{1}, behaviorType, 1, true, {}});
            const auto document = PrefabDocument::Create(data, Limits()).Value();
            Assets::AssetRegistry registry;
            REQUIRE(
                registry
                    .Publish({{Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(),
                               ProjectPath::Parse("assets/value.prefab").Value(), ProjectPath::Parse("assets/value.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
            const auto sources =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{document, initial.Sources()[0].sourceRevision}}, Limits()).Value();
            const auto source = sources.Resolve(Asset(), PrefabInstanceId::Create(9).Value(), Limits()).Value();
            PrefabComponentOverridePolicy policy{{Type(), 1, "Values", "Tests", {}, {}}, 2, true, true, {}, {}, {}};
            PrefabBehaviorComponentPolicy behavior{behaviorType, 1, {Type()}};
            SECTION("required by behavior") {
                const auto schema = PrefabValueSchemaSnapshot::Create({}, Limits(), {policy}, {behavior}).Value();
                CHECK(PreparePrefabComponentEdit(source, {}, PrefabRemoveComponentEdit{Target()}, &schema, Limits()).HasError());
            }
            SECTION("incompatible behavior") {
                policy.incompatibleBehaviors = {behaviorType};
                behavior.requiredComponents.clear();
                const auto schema = PrefabValueSchemaSnapshot::Create({}, Limits(), {policy}, {behavior}).Value();
                CHECK(PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), Added()}, &schema, Limits()).HasError());
            }
            SECTION("unavailable behavior metadata") {
                const auto schema = PrefabValueSchemaSnapshot::Create({}, Limits(), {policy}).Value();
                CHECK(PreparePrefabComponentEdit(source, {}, PrefabRemoveComponentEdit{Target()}, &schema, Limits()).HasError());
            }
        }

        TEST_CASE("Topology resolution respects final component and payload budgets and cancellation",
                  "[unit][prefab][override][topology]") {
            const auto source = Source();
            const auto schema = TopologySchema(2);
            auto policy = Limits().Policy();
            policy.maximumComponentsPerObject = 1;
            const auto limits = PrefabLimitProfile::Create(policy).Value();
            CHECK(PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), Added()}, &schema, limits).HasError());
            CHECK(PreparePrefabComponentEdit(source, {}, PrefabReplaceComponentEdit{Target(), Added()}, &schema, limits).HasValue());
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            CHECK(PreparePrefabComponentEdit(source, {}, PrefabRemoveComponentEdit{Target()}, &schema, Limits(), cancelled.Token())
                      .HasError());
            policy = Limits().Policy();
            policy.maximumExpandedPayloadBytes = source.Objects()[0].object.components[0].component.payload.size();
            CHECK(PreparePrefabComponentEdit(source, {}, PrefabAddComponentOverride{Object(), Added()}, &schema,
                                             PrefabLimitProfile::Create(policy).Value())
                      .HasError());
        }
    }  // namespace
}  // namespace Horo::Prefab
