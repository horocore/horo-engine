#include "NavigationTraversalReservations.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <utility>

namespace Horo::Navigation {
    namespace {
        /** @brief Published Navigation world and matching agent registry used at their owner safe point. */
        struct TraversalFixture final {
            const NavigationWorldActivationDescriptor activation = TestSupport::Activation();
            const NavigationAgentProfileId profile = TestSupport::Id<NavigationAgentProfileId>(1);
            const NavigationTraversalLink link{activation, TestSupport::Id<NavigationLinkId>(3), 1};
            NavigationWorldLifecycle world = std::move(NavigationWorldLifecycle::Create(4)).Value();
            NavigationAgentRegistry agents = std::move(NavigationAgentRegistry::Create({.maximumAgents = 4})).Value();
            std::unique_ptr<NavigationAgentSceneCandidate> activationOwner;

            TraversalFixture() {
                CoordinatorTestSupport::Activate(world);
                const NavigationAgentSceneBinding binding{activation.world, activation.scene, activation.sceneGeneration};
                auto prepared = agents.PrepareScene(binding, {});
                REQUIRE(prepared.HasValue());
                activationOwner = std::move(prepared).Value();
                activationOwner->Publish();
            }

            CrowdAgentHandle Agent(const std::uint32_t index, const std::uint32_t generation = 1, const std::uint64_t selectedProfile = 1) {
                const auto registered = agents.RegisterAtSafePoint({.owner = {activation.scene, index, generation},
                                                                    .profile = TestSupport::Id<NavigationAgentProfileId>(selectedProfile),
                                                                    .filter = TestSupport::Id<NavigationFilterId>(1),
                                                                    .radiusOverride = {}});
                REQUIRE(registered.HasValue());
                return registered.Value();
            }

            NavigationTraversalReservations Create(const NavigationTraversalLink &admittedLink, const std::size_t slots = 2) const {
                auto reservations = NavigationTraversalReservations::Create(world, agents, admittedLink, NavigationLinkKind::Door,
                                                                            std::array{profile}, 1, slots);
                REQUIRE(reservations.HasValue());
                return std::move(reservations).Value();
            }
        };
    }  // namespace

    TEST_CASE("Off-mesh reservations release link capacity once and retain bounded terminal receipts", "[unit][navigation][traversal]") {
        using enum NavigationLinkKind;
        TraversalFixture fixture;
        auto link = fixture.Create(fixture.link);
        const auto first = fixture.Agent(1);
        const auto second = fixture.Agent(2);
        const auto incompatible = fixture.Agent(3, 1, 2);

        TestSupport::RequireError(link.Reserve(incompatible, Door, 1, 5), NavigationErrors::CapabilityUnavailable);
        TestSupport::RequireError(link.Reserve(first, Ladder, 1, 5), NavigationErrors::CapabilityUnavailable);
        const auto a = link.Reserve(first, Door, 1, 5).Value();
        TestSupport::RequireError(link.Reserve(second, Door, 1, 5), NavigationErrors::AdmissionRejected);
        CHECK_FALSE(link.Transition(a, second, NavigationTraversalState::Entering, 1));
        CHECK_FALSE(link.Transition(a, first, NavigationTraversalState::Active, 1));
        CHECK_FALSE(link.Take(a, first));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Entering, 2));
        CHECK_FALSE(link.Transition(a, first, NavigationTraversalState::Active, 1));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Active, 2));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Completed, 3));
        CHECK_FALSE(link.Transition(a, first, NavigationTraversalState::Cancelled, 3));

        const auto b = link.Reserve(second, Door, 3, 5).Value();
        REQUIRE(link.AdvanceAtSafePoint(5));
        CHECK_FALSE(link.Transition(b, second, NavigationTraversalState::Entering, 5));
        TestSupport::RequireError(link.Reserve(first, Door, 5, 6), NavigationErrors::CapacityExceeded);

        const auto completion = link.Take(a, first);
        REQUIRE(completion);
        CHECK(completion->state == NavigationTraversalState::Completed);
        CHECK(completion->reason == NavigationTraversalEnd::Completed);
        CHECK_FALSE(link.Take(a, first));
        const auto expiration = link.Take(b, second);
        REQUIRE(expiration);
        CHECK(expiration->state == NavigationTraversalState::Failed);
        CHECK(expiration->reason == NavigationTraversalEnd::Deadline);
        CHECK(link.Reserve(first, Door, 5, 6).HasValue());
    }

    TEST_CASE("Off-mesh reservations reject recycled agents and link incarnations", "[unit][navigation][traversal]") {
        using enum NavigationLinkKind;
        TraversalFixture fixture;
        auto link = fixture.Create(fixture.link);
        const auto priorAgent = fixture.Agent(1);
        const auto prior = link.Reserve(priorAgent, Door, 1, 9).Value();
        REQUIRE(fixture.agents.UnregisterAtSafePoint(priorAgent).HasValue());
        const auto agent = fixture.Agent(1, 2);
        CHECK_FALSE(link.Transition(prior, agent, NavigationTraversalState::Entering, 2));
        const auto retiredAgent = link.Take(prior, priorAgent);
        REQUIRE(retiredAgent);
        CHECK(retiredAgent->reason == NavigationTraversalEnd::AgentRetired);
        const auto previousLink = link.Reserve(agent, Door, 2, 9).Value();
        link.RetireAtSafePoint();
        const auto retiredLink = link.Take(previousLink, agent);
        REQUIRE(retiredLink);
        CHECK(retiredLink->reason == NavigationTraversalEnd::LinkRetired);
        TestSupport::RequireError(link.Reserve(agent, Door, 2, 9), NavigationErrors::StaleSnapshot);

        auto replacement = fixture.link;
        ++replacement.incarnation;
        auto newLink = fixture.Create(replacement);
        const auto current = newLink.Reserve(agent, Door, 2, 9).Value();
        CHECK_FALSE(newLink.Transition(previousLink, agent, NavigationTraversalState::Entering, 2));
        REQUIRE(newLink.Transition(current, agent, NavigationTraversalState::Entering, 2));
        auto nextTopology = fixture.activation;
        nextTopology.topology = TestSupport::Id<NavigationGeneration>(18);
        REQUIRE(fixture.world.StageEmptyTopology(fixture.activation, nextTopology.topology).HasValue());
        REQUIRE(fixture.world.CommitStagedTopology(nextTopology).HasValue());
        CHECK_FALSE(newLink.Transition(current, agent, NavigationTraversalState::Active, 3));
        const auto retiredTopology = newLink.Take(current, agent);
        REQUIRE(retiredTopology);
        CHECK(retiredTopology->reason == NavigationTraversalEnd::LinkRetired);
    }
}  // namespace Horo::Navigation
