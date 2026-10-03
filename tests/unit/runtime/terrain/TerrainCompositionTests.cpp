#include "Horo/Terrain/TerrainComposition.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>

namespace Horo::Terrain {
    namespace {
        template <typename Revision> Revision RevisionFrom(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        constexpr std::size_t Index(const TerrainFoliageCapability capability) {
            return static_cast<std::size_t>(capability);
        }

        std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount> Facts(const bool available = true) {
            std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount> facts{};
            for (std::size_t index = 0; index < facts.size(); ++index)
                facts[index] = {static_cast<TerrainFoliageCapability>(index), available,
                                available ? RevisionFrom<TerrainHostCapabilityRevision>(index + 1U) : TerrainHostCapabilityRevision{}};
            return facts;
        }

        void SetAvailable(std::array<TerrainCapabilityFact, TerrainCompositionCapabilityCount> &facts,
                          const TerrainFoliageCapability capability, const bool available) {
            facts[Index(capability)].available = available;
            facts[Index(capability)].revision =
                available ? RevisionFrom<TerrainHostCapabilityRevision>(Index(capability) + 1U) : TerrainHostCapabilityRevision{};
        }

        TerrainCompositionRequest Request(const TerrainProductProfile profile, const std::span<const TerrainCapabilityFact> facts,
                                          const std::uint64_t revision = 7) {
            return {.revision = RevisionFrom<TerrainCapabilityRevision>(revision), .profile = profile, .capabilities = facts};
        }

        TerrainFoliageCapabilitySet Capabilities(const std::initializer_list<TerrainFoliageCapability> values) {
            return TerrainFoliageCapabilitySet::Create({values.begin(), values.size()}).Value();
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Terrain product policies distinguish null, headless, editor, runtime and unsupported", "[unit][terrain][composition]") {
        static_assert(Index(TerrainFoliageCapability::TerrainAuthoring) == 10);
        static_assert(Index(TerrainFoliageCapability::FoliageAuthoring) == 11);
        static_assert(!std::is_default_constructible_v<TerrainComposition>);
        static_assert(std::is_trivially_copyable_v<TerrainComposition>);

        for (const auto profile : {TerrainProductProfile::Null, TerrainProductProfile::Unsupported}) {
            const auto policy = GetTerrainProductProfilePolicy(profile);
            REQUIRE(policy.HasValue());
            for (const auto requirement : policy.Value().requirements)
                CHECK(requirement == TerrainCapabilityRequirement::Omitted);
        }
        const auto headless = GetTerrainProductProfilePolicy(TerrainProductProfile::Headless).Value();
        CHECK(headless.requirements[Index(TerrainFoliageCapability::TerrainRuntime)] == TerrainCapabilityRequirement::Required);
        CHECK(headless.requirements[Index(TerrainFoliageCapability::RenderExtraction)] == TerrainCapabilityRequirement::Omitted);
        CHECK(headless.requirements[Index(TerrainFoliageCapability::TerrainAuthoring)] == TerrainCapabilityRequirement::Omitted);
        CHECK(headless.requirements[Index(TerrainFoliageCapability::FoliageAuthoring)] == TerrainCapabilityRequirement::Omitted);

        const auto editor = GetTerrainProductProfilePolicy(TerrainProductProfile::Editor).Value();
        CHECK(editor.requirements[Index(TerrainFoliageCapability::TerrainAuthoring)] == TerrainCapabilityRequirement::Required);
        CHECK(editor.requirements[Index(TerrainFoliageCapability::FoliageAuthoring)] == TerrainCapabilityRequirement::Required);
        CHECK(editor.requirements[Index(TerrainFoliageCapability::RenderExtraction)] == TerrainCapabilityRequirement::Optional);
        const auto runtime = GetTerrainProductProfilePolicy(TerrainProductProfile::Runtime).Value();
        CHECK(runtime.requirements[Index(TerrainFoliageCapability::RenderExtraction)] == TerrainCapabilityRequirement::Required);
        CHECK(runtime.requirements[Index(TerrainFoliageCapability::TerrainAuthoring)] == TerrainCapabilityRequirement::Omitted);
        CHECK(runtime.requirements[Index(TerrainFoliageCapability::FoliageAuthoring)] == TerrainCapabilityRequirement::Omitted);
        RequireError(GetTerrainProductProfilePolicy(TerrainProductProfile::Count), TerrainErrors::CompositionInvalid);
    }

    TEST_CASE("Terrain compositions report exact host facts without installing forbidden grants", "[unit][terrain][composition]") {
        auto facts = Facts();
        std::swap(facts[0], facts[11]);
        for (const auto profile : {TerrainProductProfile::Null, TerrainProductProfile::Unsupported, TerrainProductProfile::Headless,
                                   TerrainProductProfile::Editor, TerrainProductProfile::Runtime}) {
            const auto result = TerrainComposition::Create(Request(profile, facts));
            REQUIRE(result.HasValue());
            CHECK(result.Value().Revision() == RevisionFrom<TerrainCapabilityRevision>(7));
            for (std::size_t index = 0; index < TerrainCompositionCapabilityCount; ++index) {
                const auto decision = result.Value().Capabilities()[index];
                CHECK(decision.capability == static_cast<TerrainFoliageCapability>(index));
                CHECK(decision.requirement == result.Value().Policy().requirements[index]);
                CHECK((decision.state == TerrainCapabilityState::Bound) == (decision.requirement != TerrainCapabilityRequirement::Omitted));
                CHECK(decision.sourceRevision.IsValid() == (decision.state == TerrainCapabilityState::Bound));
                if (decision.state == TerrainCapabilityState::Bound)
                    CHECK(decision.sourceRevision == RevisionFrom<TerrainHostCapabilityRevision>(index + 1U));
            }
            CHECK(result.Value().Resolve(TerrainFoliageCapability::TerrainQuery).HasValue());
            RequireError(result.Value().Resolve(TerrainFoliageCapability::Count), TerrainErrors::CompositionInvalid);
        }
        const auto absent = Facts(false);
        for (const auto profile : {TerrainProductProfile::Null, TerrainProductProfile::Unsupported}) {
            const auto result = TerrainComposition::Create(Request(profile, absent));
            REQUIRE(result.HasValue());
            for (const auto &decision : result.Value().Capabilities())
                CHECK(decision.state == TerrainCapabilityState::Omitted);
        }
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Headless, absent)), TerrainErrors::CapabilityUnsupported);
    }

    TEST_CASE("Terrain compositions fail required renderer and authoring without a profile fallback", "[unit][terrain][composition]") {
        auto facts = Facts();
        SetAvailable(facts, TerrainFoliageCapability::RenderExtraction, false);
        const auto editor = TerrainComposition::Create(Request(TerrainProductProfile::Editor, facts));
        REQUIRE(editor.HasValue());
        CHECK(editor.Value().Resolve(TerrainFoliageCapability::RenderExtraction).Value().state == TerrainCapabilityState::Unavailable);
        CHECK(editor.Value().Resolve(TerrainFoliageCapability::CpuCulling).Value().state == TerrainCapabilityState::Unavailable);
        CHECK(editor.Value().Resolve(TerrainFoliageCapability::VertexWind).Value().state == TerrainCapabilityState::Unavailable);
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Runtime, facts)), TerrainErrors::CapabilityUnsupported);
        CHECK(TerrainComposition::Create(Request(TerrainProductProfile::Headless, facts)).HasValue());

        SetAvailable(facts, TerrainFoliageCapability::TerrainAuthoring, false);
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Editor, facts)), TerrainErrors::CapabilityUnsupported);
        CHECK(TerrainComposition::Create(Request(TerrainProductProfile::Runtime, facts)).HasError());
        SetAvailable(facts, TerrainFoliageCapability::RenderExtraction, true);
        const auto runtime = TerrainComposition::Create(Request(TerrainProductProfile::Runtime, facts));
        REQUIRE(runtime.HasValue());
        CHECK(runtime.Value().Resolve(TerrainFoliageCapability::TerrainAuthoring).Value().state == TerrainCapabilityState::Omitted);
    }

    TEST_CASE("Terrain composition rejects incomplete duplicate and contradictory bounded evidence", "[unit][terrain][composition]") {
        auto facts = Facts();
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Headless, std::span{facts}.first(11))),
                     TerrainErrors::CompositionInvalid);
        facts[1].capability = facts[0].capability;
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Headless, facts)), TerrainErrors::CompositionInvalid);
        facts = Facts();
        facts[0].capability = TerrainFoliageCapability::Count;
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Headless, facts)), TerrainErrors::CompositionInvalid);
        facts = Facts();
        facts[0].available = false;
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Headless, facts)), TerrainErrors::CompositionInvalid);
        facts = Facts(false);
        facts[0].revision = RevisionFrom<TerrainHostCapabilityRevision>(1);
        RequireError(TerrainComposition::Create(Request(TerrainProductProfile::Null, facts)), TerrainErrors::CompositionInvalid);
        facts = Facts();
        auto request = Request(TerrainProductProfile::Headless, facts);
        request.contractVersion = 2;
        RequireError(TerrainComposition::Create(request), TerrainErrors::CompositionInvalid);
        request = Request(TerrainProductProfile::Count, facts);
        RequireError(TerrainComposition::Create(request), TerrainErrors::CompositionInvalid);
        request = Request(TerrainProductProfile::Headless, facts);
        request.revision = {};
        RequireError(TerrainComposition::Create(request), TerrainErrors::CompositionInvalid);
    }

    TEST_CASE("Terrain composition replacement and admission preserve exact revision and lifecycle", "[unit][terrain][composition]") {
        auto facts = Facts();
        const auto old = TerrainComposition::Create(Request(TerrainProductProfile::Headless, facts)).Value();
        auto nextRequest = Request(TerrainProductProfile::Editor, facts, 8);
        const auto expected = RevisionFrom<TerrainCapabilityRevision>(7);
        RequireError(TerrainComposition::Replace(old, RevisionFrom<TerrainCapabilityRevision>(6), nextRequest),
                     TerrainErrors::RevisionStale);
        RequireError(TerrainComposition::Replace(old, expected, Request(TerrainProductProfile::Editor, facts, 7)),
                     TerrainErrors::RevisionStale);
        SetAvailable(facts, TerrainFoliageCapability::FoliageAuthoring, false);
        RequireError(TerrainComposition::Replace(old, expected, Request(TerrainProductProfile::Editor, facts, 8)),
                     TerrainErrors::CapabilityUnsupported);
        CHECK(old.Revision() == expected);
        CHECK(old.Policy().profile == TerrainProductProfile::Headless);
        SetAvailable(facts, TerrainFoliageCapability::FoliageAuthoring, true);
        const auto next = TerrainComposition::Replace(old, expected, nextRequest);
        REQUIRE(next.HasValue());
        CHECK(next.Value().Revision() == RevisionFrom<TerrainCapabilityRevision>(8));
        RequireError(ValidateTerrainCompositionAdmission(old, next.Value().Revision(), TerrainCompositionLifecycle::Active,
                                                         Capabilities({TerrainFoliageCapability::TerrainRuntime})),
                     TerrainErrors::RevisionStale);
        CHECK(ValidateTerrainCompositionAdmission(next.Value(), next.Value().Revision(), TerrainCompositionLifecycle::Active,
                                                  Capabilities({TerrainFoliageCapability::TerrainAuthoring}))
                  .HasValue());
        RequireError(ValidateTerrainCompositionAdmission(next.Value(), next.Value().Revision(), TerrainCompositionLifecycle::Cancelling,
                                                         Capabilities({TerrainFoliageCapability::TerrainAuthoring})),
                     TerrainErrors::CompositionCancelled);
        for (const auto lifecycle : {TerrainCompositionLifecycle::ShuttingDown, TerrainCompositionLifecycle::Closed})
            RequireError(ValidateTerrainCompositionAdmission(next.Value(), next.Value().Revision(), lifecycle,
                                                             Capabilities({TerrainFoliageCapability::TerrainAuthoring})),
                         TerrainErrors::LifecycleUnavailable);
        RequireError(ValidateTerrainCompositionAdmission(next.Value(), next.Value().Revision(), TerrainCompositionLifecycle::Count,
                                                         TerrainFoliageCapabilitySet::Empty()),
                     TerrainErrors::CompositionInvalid);
        RequireError(ValidateTerrainCompositionAdmission(old, old.Revision(), TerrainCompositionLifecycle::Active,
                                                         Capabilities({TerrainFoliageCapability::RenderExtraction})),
                     TerrainErrors::CapabilityUnsupported);
        const auto unsupported = TerrainComposition::Create(Request(TerrainProductProfile::Unsupported, facts)).Value();
        RequireError(ValidateTerrainCompositionAdmission(unsupported, unsupported.Revision(), TerrainCompositionLifecycle::Active,
                                                         TerrainFoliageCapabilitySet::Empty()),
                     TerrainErrors::CompositionProfileUnsupported);
    }
}  // namespace Horo::Terrain
