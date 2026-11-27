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
            const NavigationTraversalLink link{activation, TestSupport::Id<NavigationLinkId>(3), 1, 1};
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

            Result<std::unique_ptr<NavigationTraversalCatalog>> CreateCatalog(const std::uint16_t capacity = 1) {
                const std::array profiles{profile};
                const std::array admissions{
                    NavigationTraversalAdmission{link.link, link.incarnation, NavigationLinkKind::Door, profiles, capacity}};
                return NavigationTraversalCatalog::Create(world, agents, admissions);
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
        CHECK_FALSE(link.Take(a, first, 1));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Entering, 2));
        CHECK_FALSE(link.Transition(a, first, NavigationTraversalState::Active, 1));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Active, 2));
        REQUIRE(link.Transition(a, first, NavigationTraversalState::Completed, 3));
        CHECK_FALSE(link.Transition(a, first, NavigationTraversalState::Cancelled, 3));

        const auto b = link.Reserve(second, Door, 3, 5).Value();
        REQUIRE(link.AdvanceAtSafePoint(5));
        CHECK_FALSE(link.Transition(b, second, NavigationTraversalState::Entering, 5));
        TestSupport::RequireError(link.Reserve(first, Door, 5, 6), NavigationErrors::CapacityExceeded);
        CHECK(link.AvailabilityAtSafePoint(5).Value() == NavigationTraversalAvailability::AtCapacity);

        const auto completion = link.Take(a, first, 5);
        REQUIRE(completion);
        CHECK(completion->state == NavigationTraversalState::Completed);
        CHECK(completion->reason == NavigationTraversalEnd::Completed);
        CHECK(link.AvailabilityAtSafePoint(5).Value() == NavigationTraversalAvailability::Available);
        CHECK_FALSE(link.Take(a, first, 5));
        const auto expiration = link.Take(b, second, 5);
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
        CHECK_FALSE(link.Take(prior, agent, 99));
        const auto retiredAgent = link.Take(prior, priorAgent, 2);
        REQUIRE(retiredAgent);
        CHECK(retiredAgent->reason == NavigationTraversalEnd::AgentRetired);
        const auto previousLink = link.Reserve(agent, Door, 2, 9).Value();
        link.RetireAtSafePoint();
        const auto retiredLink = link.Take(previousLink, agent, 2);
        REQUIRE(retiredLink);
        CHECK(retiredLink->reason == NavigationTraversalEnd::LinkRetired);
        TestSupport::RequireError(link.Reserve(agent, Door, 2, 9), NavigationErrors::StaleSnapshot);

        auto replacement = fixture.link;
        ++replacement.incarnation;
        auto newLink = fixture.Create(replacement);
        const auto current = newLink.Reserve(agent, Door, 2, 9).Value();
        TestSupport::RequireError(newLink.Reserve(priorAgent, Door, 99, 100), NavigationErrors::CapabilityUnavailable);
        CHECK_FALSE(newLink.IsLive(current, priorAgent, 99));
        CHECK_FALSE(newLink.Take(current, priorAgent, 99));
        CHECK_FALSE(newLink.Transition(current, priorAgent, NavigationTraversalState::Entering, 99));
        CHECK_FALSE(newLink.Transition(previousLink, agent, NavigationTraversalState::Entering, 2));
        REQUIRE(newLink.Transition(current, agent, NavigationTraversalState::Entering, 2));
        auto nextTopology = fixture.activation;
        nextTopology.topology = TestSupport::Id<NavigationGeneration>(18);
        REQUIRE(fixture.world.StageEmptyTopology(fixture.activation, nextTopology.topology).HasValue());
        REQUIRE(fixture.world.CommitStagedTopology(nextTopology).HasValue());
        CHECK_FALSE(newLink.Transition(current, agent, NavigationTraversalState::Active, 3));
        const auto retiredTopology = newLink.Take(current, agent, 3);
        REQUIRE(retiredTopology);
        CHECK(retiredTopology->reason == NavigationTraversalEnd::LinkRetired);
    }

    TEST_CASE("One catalog owns each topology and old grants cannot affect its replacement", "[unit][navigation][traversal]") {
        TraversalFixture fixture;
        TestSupport::RequireError(fixture.CreateCatalog(0), NavigationErrors::CapacityExceeded);
        auto created = fixture.CreateCatalog();
        REQUIRE(created.HasValue());
        auto catalog = std::move(created).Value();
        const auto a = fixture.Agent(1);
        const auto b = fixture.Agent(2);
        const auto topology = fixture.activation.topology;
        const auto authoredLink = fixture.link.link;
        const auto oldLink = catalog->ReadLink(authoredLink, topology);
        REQUIRE(oldLink);
        CHECK(catalog->AvailabilityAtSafePoint(authoredLink, topology, 1).Value() == NavigationTraversalAvailability::Available);
        const auto admitted = catalog->Reserve(authoredLink, topology, a, 1, 4);
        REQUIRE(admitted.HasValue());
        const auto oldToken = admitted.Value().reservation;
        CHECK(oldToken.link == *oldLink);
        TestSupport::RequireError(fixture.CreateCatalog(), NavigationErrors::AdmissionRejected);
        TestSupport::RequireError(catalog->Reserve(authoredLink, topology, b, 1, 4), NavigationErrors::AdmissionRejected);
        CHECK(catalog->AvailabilityAtSafePoint(authoredLink, topology, 1).Value() == NavigationTraversalAvailability::AtCapacity);
        catalog.reset();

        auto reopened = fixture.CreateCatalog();
        REQUIRE(reopened.HasValue());
        auto &replacement = *reopened.Value();
        const auto fresh = replacement.Reserve(authoredLink, topology, a, 1, 4);
        REQUIRE(fresh.HasValue());
        const auto token = fresh.Value().reservation;
        CHECK(token.sequence == oldToken.sequence);
        CHECK(token.link.catalogGeneration > oldToken.link.catalogGeneration);
        CHECK_FALSE(replacement.IsLive(oldToken, a, 99));
        CHECK_FALSE(replacement.Transition(oldToken, a, NavigationTraversalState::Entering, 99));
        CHECK_FALSE(replacement.Take(oldToken, a, 99));
        replacement.Abandon(oldToken, a);
        CHECK_FALSE(replacement.Retire(*oldLink));
        REQUIRE(replacement.IsLive(token, a, 2));
        REQUIRE(replacement.Transition(token, a, NavigationTraversalState::Entering, 2));
        REQUIRE(replacement.Retire(token.link));
        CHECK(replacement.AvailabilityAtSafePoint(authoredLink, topology, 2).Value() == NavigationTraversalAvailability::Retired);
        const auto completion = replacement.Take(token, a, 2);
        REQUIRE(completion);
        CHECK(completion->reason == NavigationTraversalEnd::LinkRetired);
    }

    TEST_CASE("Paused navigation cannot advertise an available traversal gate", "[unit][navigation][traversal]") {
        TraversalFixture fixture;
        auto created = fixture.CreateCatalog();
        REQUIRE(created.HasValue());
        auto &catalog = *created.Value();
        const auto topology = fixture.activation.topology;
        const auto link = fixture.link.link;
        REQUIRE(fixture.world.Pause(fixture.activation.world).HasValue());
        TestSupport::RequireError(catalog.AvailabilityAtSafePoint(link, topology, 1), NavigationErrors::CapabilityUnavailable);
        REQUIRE(fixture.world.Resume(fixture.activation.world).HasValue());
        CHECK(catalog.AvailabilityAtSafePoint(link, topology, 1).Value() == NavigationTraversalAvailability::Available);
    }

    TEST_CASE("A topology replacement admits one new catalog while the predecessor drains", "[unit][navigation][traversal]") {
        TraversalFixture fixture;
        auto created = fixture.CreateCatalog();
        REQUIRE(created.HasValue());
        auto oldCatalog = std::move(created).Value();
        const auto agent = fixture.Agent(1);
        const auto authoredLink = fixture.link.link;
        const auto prior = oldCatalog->Reserve(authoredLink, fixture.activation.topology, agent, 1, 4);
        REQUIRE(prior.HasValue());
        const auto priorToken = prior.Value().reservation;

        auto newTopology = fixture.activation;
        newTopology.topology = TestSupport::Id<NavigationGeneration>(18);
        REQUIRE(fixture.world.StageEmptyTopology(fixture.activation, newTopology.topology).HasValue());
        REQUIRE(fixture.world.CommitStagedTopology(newTopology).HasValue());
        auto createdReplacement = fixture.CreateCatalog();
        REQUIRE(createdReplacement.HasValue());
        auto &newCatalog = *createdReplacement.Value();
        const auto current = newCatalog.Reserve(authoredLink, newTopology.topology, agent, 2, 4);
        REQUIRE(current.HasValue());
        CHECK_FALSE(newCatalog.ReadLink(authoredLink, fixture.activation.topology));
        CHECK_FALSE(newCatalog.Retire(priorToken.link));
        CHECK_FALSE(newCatalog.IsLive(priorToken, agent, 2));
        const auto retired = oldCatalog->Take(priorToken, agent, 2);
        REQUIRE(retired);
        CHECK(retired->reason == NavigationTraversalEnd::LinkRetired);
        oldCatalog.reset();
        TestSupport::RequireError(fixture.CreateCatalog(), NavigationErrors::AdmissionRejected);
        CHECK(newCatalog.IsLive(current.Value().reservation, agent, 2));
    }
}  // namespace Horo::Navigation
