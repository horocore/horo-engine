#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::Prefab {
    TEST_CASE("Prefab project policy defaults resolve at the canonical hard ceilings", "[unit][prefab][limits]") {
        const auto profile = PrefabLimitProfile::Create({});
        REQUIRE(profile.HasValue());
        const PrefabProjectPolicy &policy = profile.Value().Policy();
        REQUIRE(policy.maximumHierarchyDepth == PrefabHardLimits::SourceHierarchyDepth);
        REQUIRE(policy.maximumObjectCount == PrefabHardLimits::SourceObjectCount);
        REQUIRE(policy.maximumSourcePayloadBytes == PrefabHardLimits::SourcePayloadBytes);
        REQUIRE(policy.maximumExpandedPayloadBytes == PrefabHardLimits::ExpandedPayloadBytes);
        REQUIRE(policy.maximumCookedPayloadBytes == PrefabHardLimits::CookedPayloadBytes);
        REQUIRE(policy.maximumComponentsPerObject == PrefabHardLimits::ComponentsPerObject);
        REQUIRE(policy.maximumReferencedAssets == PrefabHardLimits::ReferencedAssets);
        REQUIRE(policy.maximumDirectNestedPlacements == PrefabHardLimits::DirectNestedPlacements);
        REQUIRE(policy.maximumVariantInheritanceDepth == PrefabHardLimits::VariantInheritanceDepth);
        REQUIRE(policy.maximumNestedPrefabDepth == PrefabHardLimits::NestedPrefabDepth);
        REQUIRE(policy.maximumOverrideRecords == PrefabHardLimits::OverrideRecords);
        REQUIRE(policy.maximumConflictAndOrphanRecords == PrefabHardLimits::ConflictAndOrphanRecords);
        REQUIRE(policy.maximumPropertyPathSegments == PrefabHardLimits::PropertyPathSegments);
        REQUIRE(policy.maximumOverrideValueBytes == PrefabHardLimits::OverrideValueBytes);
        REQUIRE(policy.maximumOverrideSetBytes == PrefabHardLimits::OverrideSetBytes);
        REQUIRE(policy.maximumBindingSlots == PrefabHardLimits::BindingSlots);
        REQUIRE(policy.maximumBindingUses == PrefabHardLimits::BindingUses);
        REQUIRE(policy.maximumInstanceBindings == PrefabHardLimits::InstanceBindings);
        REQUIRE(policy.maximumRuntimeSpawnDepth == PrefabHardLimits::RuntimeSpawnDepth);
    }

    TEST_CASE("Prefab policy rejects empty and above-hard independent limits", "[unit][prefab][limits]") {
        struct Field final {
            std::size_t PrefabProjectPolicy::*member;
            std::size_t maximum;
        };

        const std::array fields{
            Field{&PrefabProjectPolicy::maximumHierarchyDepth, PrefabHardLimits::SourceHierarchyDepth},
            Field{&PrefabProjectPolicy::maximumObjectCount, PrefabHardLimits::SourceObjectCount},
            Field{&PrefabProjectPolicy::maximumSourcePayloadBytes, PrefabHardLimits::SourcePayloadBytes},
            Field{&PrefabProjectPolicy::maximumExpandedPayloadBytes, PrefabHardLimits::ExpandedPayloadBytes},
            Field{&PrefabProjectPolicy::maximumCookedPayloadBytes, PrefabHardLimits::CookedPayloadBytes},
            Field{&PrefabProjectPolicy::maximumComponentsPerObject, PrefabHardLimits::ComponentsPerObject},
            Field{&PrefabProjectPolicy::maximumReferencedAssets, PrefabHardLimits::ReferencedAssets},
            Field{&PrefabProjectPolicy::maximumDirectNestedPlacements, PrefabHardLimits::DirectNestedPlacements},
            Field{&PrefabProjectPolicy::maximumVariantInheritanceDepth, PrefabHardLimits::VariantInheritanceDepth},
            Field{&PrefabProjectPolicy::maximumNestedPrefabDepth, PrefabHardLimits::NestedPrefabDepth},
            Field{&PrefabProjectPolicy::maximumOverrideRecords, PrefabHardLimits::OverrideRecords},
            Field{&PrefabProjectPolicy::maximumConflictAndOrphanRecords, PrefabHardLimits::ConflictAndOrphanRecords},
            Field{&PrefabProjectPolicy::maximumPropertyPathSegments, PrefabHardLimits::PropertyPathSegments},
            Field{&PrefabProjectPolicy::maximumOverrideValueBytes, PrefabHardLimits::OverrideValueBytes},
            Field{&PrefabProjectPolicy::maximumOverrideSetBytes, PrefabHardLimits::OverrideSetBytes},
            Field{&PrefabProjectPolicy::maximumBindingSlots, PrefabHardLimits::BindingSlots},
            Field{&PrefabProjectPolicy::maximumBindingUses, PrefabHardLimits::BindingUses},
            Field{&PrefabProjectPolicy::maximumInstanceBindings, PrefabHardLimits::InstanceBindings},
            Field{&PrefabProjectPolicy::maximumRuntimeSpawnDepth, PrefabHardLimits::RuntimeSpawnDepth},
        };
        for (const Field field : fields) {
            for (const std::size_t invalid : {std::size_t{0}, field.maximum + 1}) {
                PrefabProjectPolicy policy;
                policy.*field.member = invalid;
                const auto result = PrefabLimitProfile::Create(policy);
                REQUIRE(result.HasError());
                REQUIRE(result.ErrorValue().code.Value() == PrefabErrors::LimitProfileInvalid.code.Value());
            }
        }
    }

    TEST_CASE("Prefab policy rejects impossible cross-field relationships", "[unit][prefab][limits]") {
        PrefabProjectPolicy overrides;
        overrides.maximumOverrideSetBytes = overrides.maximumOverrideValueBytes - 1;
        REQUIRE(PrefabLimitProfile::Create(overrides).HasError());

        overrides = {};
        overrides.maximumOverrideSetBytes = overrides.maximumOverrideValueBytes;
        REQUIRE(PrefabLimitProfile::Create(overrides).HasValue());
    }

    TEST_CASE("Prefab expansion work is a checked derivation of the validated policy", "[unit][prefab][limits]") {
        PrefabProjectPolicy policy;
        policy.maximumObjectCount /= 2;
        policy.maximumComponentsPerObject /= 2;
        policy.maximumReferencedAssets /= 2;
        policy.maximumDirectNestedPlacements /= 2;
        policy.maximumOverrideRecords /= 2;
        policy.maximumConflictAndOrphanRecords /= 2;
        policy.maximumBindingUses /= 2;
        policy.maximumBindingSlots /= 2;
        policy.maximumInstanceBindings /= 2;

        const auto profile = PrefabLimitProfile::Create(policy);
        REQUIRE(profile.HasValue());
        const std::size_t expected = policy.maximumObjectCount + policy.maximumObjectCount * policy.maximumComponentsPerObject +
                                     policy.maximumReferencedAssets + policy.maximumDirectNestedPlacements +
                                     policy.maximumVariantInheritanceDepth + policy.maximumNestedPrefabDepth +
                                     policy.maximumOverrideRecords * policy.maximumPropertyPathSegments +
                                     policy.maximumConflictAndOrphanRecords + policy.maximumBindingUses;
        REQUIRE(profile.Value().MaximumExpansionWorkItems() == expected);
    }

    TEST_CASE("Prefab expansion exhaustion is typed and preserves the remaining budget", "[unit][prefab][limits]") {
        const auto profile = PrefabLimitProfile::Create({});
        REQUIRE(profile.HasValue());
        PrefabExpansionBudget budget{profile.Value()};
        const std::size_t capacity = budget.Remaining();

        REQUIRE(budget.Consume(capacity).HasValue());
        REQUIRE(budget.Remaining() == 0);
        const auto exhausted = budget.Consume(1);
        REQUIRE(exhausted.HasError());
        REQUIRE(exhausted.ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
        REQUIRE(budget.Remaining() == 0);
    }

    TEST_CASE("Prefab oversized work charge rejects without wrapping or consuming subsequent admission", "[unit][prefab][limits][stress]") {
        const auto profile = PrefabLimitProfile::Create({});
        REQUIRE(profile.HasValue());
        PrefabExpansionBudget budget{profile.Value()};
        const auto capacity = budget.Remaining();
        const auto rejected = budget.Consume(std::numeric_limits<std::size_t>::max());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::WorkBudgetExceeded.code.Value());
        CHECK(budget.Remaining() == capacity);
        REQUIRE(budget.Consume(0).HasValue());
        CHECK(budget.Remaining() == capacity);
        REQUIRE(budget.Consume(capacity).HasValue());
        REQUIRE(budget.Consume(0).HasValue());
        CHECK(budget.Remaining() == 0);
    }

    TEST_CASE("Prefab expansion budget move transfers its single work authority", "[unit][prefab][limits]") {
        const auto profile = PrefabLimitProfile::Create({});
        REQUIRE(profile.HasValue());
        PrefabExpansionBudget source{profile.Value()};
        const std::size_t capacity = source.Remaining();
        REQUIRE(source.Consume(1).HasValue());

        PrefabExpansionBudget constructed{std::move(source)};
        REQUIRE(source.Remaining() == 0);
        REQUIRE(constructed.Remaining() == capacity - 1);

        PrefabExpansionBudget assigned{profile.Value()};
        assigned = std::move(constructed);
        REQUIRE(constructed.Remaining() == 0);
        REQUIRE(assigned.Remaining() == capacity - 1);
    }
}  // namespace Horo::Prefab
