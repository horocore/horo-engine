#include "PrefabValueOverrideTestSupport.h"

#include <algorithm>

namespace Horo::Prefab {
    namespace {
        using namespace ValueTestSupport;

        /** @brief Capture the same real stable property contract used by existing instance value tests. */
        std::vector<PrefabValuePropertySchema> Properties() {
            const auto schemas = Schemas();
            return {schemas.Properties().begin(), schemas.Properties().end()};
        }

        /** @brief Explicit inert component metadata; topology permissions are not inferred from a type name. */
        PrefabComponentOverridePolicy Policy(const char *type = "game.override.values") {
            PrefabComponentOverridePolicy policy;
            policy.component.typeId = Gameplay::ComponentTypeId::Parse(type).Value();
            policy.component.displayName = "Values";
            policy.component.category = "Prefab";
            policy.component.properties = {
                {Gameplay::ComponentPropertyId::Parse("speed").Value(), "Speed", Gameplay::ComponentPropertyKind::Number, false}};
            policy.allowAddition = true;
            policy.allowRemoval = true;
            return policy;
        }

        TEST_CASE("Component schema capture owns metadata and ignores presentation-only renames", "[unit][prefab][override][schema]") {
            auto policy = Policy();
            const auto first = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {policy});
            REQUIRE(first.HasValue());
            policy.component.displayName = "Renamed component";
            policy.component.category = "Other category";
            policy.component.properties.front().displayName = "Localized speed";
            const auto renamed = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {policy});
            REQUIRE(renamed.HasValue());
            CHECK(first.Value().Revision() == renamed.Value().Revision());
            REQUIRE(first.Value().FindComponent(Type()));
            CHECK(first.Value().FindComponent(Type())->component.displayName == "Values");
            CHECK(first.Value().FindComponent(Type())->component.properties.front().displayName == "Speed");
            CHECK(first.Value().Revision() != Schemas().Revision());
        }

        TEST_CASE("Component and behavior schema identity is independent from declaration order", "[unit][prefab][override][schema]") {
            auto values = Policy();
            const auto support = Policy("game.override.support");
            const auto other = Policy("game.override.other");
            values.requiredComponents = {support.component.typeId, other.component.typeId};
            PrefabBehaviorComponentPolicy behavior{Gameplay::BehaviorTypeId::Parse("game.override.behavior").Value(),
                                                   1,
                                                   {support.component.typeId, other.component.typeId}};
            const auto first = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {values, support, other}, {behavior});
            REQUIRE(first.HasValue());
            std::ranges::reverse(values.requiredComponents);
            std::ranges::reverse(behavior.requiredComponents);
            const auto reordered = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {other, support, values}, {behavior});
            REQUIRE(reordered.HasValue());
            CHECK(first.Value().Revision() == reordered.Value().Revision());
            REQUIRE(first.Value().FindBehavior(behavior.type));
            CHECK(first.Value().FindBehavior(behavior.type)->requiredComponents.size() == 2);
            CHECK(first.Value().Components().size() == 3);
        }

        TEST_CASE("Component schema identity fences topology permissions and multiplicity", "[unit][prefab][override][schema]") {
            const auto first = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {Policy()});
            REQUIRE(first.HasValue());
            auto changed = Policy();
            SECTION("addition permission") {
                changed.allowAddition = false;
            }
            SECTION("removal permission") {
                changed.allowRemoval = false;
            }
            SECTION("multiplicity") {
                changed.maximumInstances = 2;
            }
            const auto second = PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {changed});
            REQUIRE(second.HasValue());
            CHECK(first.Value().Revision() != second.Value().Revision());
        }

        TEST_CASE("Component schema capture rejects ambiguous or unsatisfied declarative constraints", "[unit][prefab][override][schema]") {
            auto policy = Policy();
            std::vector policies{policy};
            const auto dependency = Gameplay::ComponentTypeId::Parse("game.override.missing").Value();
            SECTION("duplicate type") {
                policies.push_back(policy);
            }
            SECTION("zero multiplicity") {
                policies.front().maximumInstances = 0;
            }
            SECTION("over-budget multiplicity") {
                policies.front().maximumInstances = static_cast<std::uint32_t>(Limits().Policy().maximumComponentsPerObject + 1);
            }
            SECTION("missing dependency schema") {
                policies.front().requiredComponents = {dependency};
            }
            SECTION("duplicate dependency") {
                policies.front().requiredComponents = {dependency, dependency};
            }
            SECTION("self dependency") {
                policies.front().requiredComponents = {Type()};
            }
            SECTION("self exclusion") {
                policies.front().incompatibleComponents = {Type()};
            }
            SECTION("required and excluded dependency") {
                policies.front().requiredComponents = {dependency};
                policies.front().incompatibleComponents = {dependency};
            }
            CHECK(PrefabValueSchemaSnapshot::Create(Properties(), Limits(), std::move(policies)).HasError());
        }

        TEST_CASE("Component schema capture rejects competing value schema authority", "[unit][prefab][override][schema]") {
            auto properties = Properties();
            SECTION("version mismatch") {
                properties.front().componentSchemaVersion = 2;
            }
            SECTION("required field mismatch") {
                properties.front().required = true;
            }
            SECTION("missing stable property") {
                properties.front().payloadProperty = Gameplay::ComponentPropertyId::Parse("other").Value();
            }
            CHECK(PrefabValueSchemaSnapshot::Create(std::move(properties), Limits(), {Policy()}).HasError());
        }

        TEST_CASE("Behavior compatibility capture requires exact identities and complete dependency schemas",
                  "[unit][prefab][override][schema]") {
            PrefabBehaviorComponentPolicy behavior{Gameplay::BehaviorTypeId::Parse("game.override.behavior").Value(), 1, {Type()}};
            std::vector behaviors{behavior};
            SECTION("missing type") {
                behaviors.front().type = {};
            }
            SECTION("zero schema") {
                behaviors.front().schemaVersion = 0;
            }
            SECTION("duplicate behavior") {
                behaviors.push_back(behavior);
            }
            SECTION("missing dependency") {
                behaviors.front().requiredComponents = {Gameplay::ComponentTypeId::Parse("game.override.missing").Value()};
            }
            CHECK(PrefabValueSchemaSnapshot::Create(Properties(), Limits(), {Policy()}, std::move(behaviors)).HasError());
        }
    }  // namespace
}  // namespace Horo::Prefab
