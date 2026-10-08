#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;

        [[nodiscard]] std::unique_ptr<Runtime::RuntimeScene> MakeScene(const std::size_t entities = 24,
                                                                       const std::uint64_t incarnation = 77) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
            for (std::size_t index = 1; index <= entities; ++index) {
                Runtime::RuntimeEntityDefinition entity;
                entity.object = Runtime::SceneObjectId{index};
                builder.Add(entity);
            }
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{incarnation});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        [[nodiscard]] Runtime::EntityRef Entity(const Runtime::RuntimeSceneView &view, const std::uint64_t object) {
            const auto entity = view.Find(Runtime::SceneObjectId{object});
            REQUIRE(entity.has_value());
            return *entity;
        }

        [[nodiscard]] PerceptionSpatialListener Listener(const Runtime::EntityRef entity) {
            PerceptionSpatialListener listener{.entity = entity};
            listener.senses[0] = SenseTypeIds::Sight;
            listener.senses[1] = SenseTypeIds::Hearing;
            listener.senseCount = 2;
            return listener;
        }

        [[nodiscard]] PerceptionSpatialSource Source(const Runtime::EntityRef entity, const std::int64_t x, const std::uint64_t layers = 1,
                                                     const std::uint64_t affiliation = 1) {
            PerceptionSpatialSource source{.entity = entity,
                                           .position = Math::WorldCoordinate64::FromMillimeters(x, 0, 0),
                                           .layers = layers,
                                           .affiliation = affiliation};
            source.senses[0] = SenseTypeIds::Sight;
            source.senseCount = 1;
            return source;
        }

        [[nodiscard]] PerceptionSpatialQuery SightQuery(const Runtime::EntityRef listener) {
            return {.listener = listener,
                    .sense = SenseTypeIds::Sight,
                    .radiusMillimeters = 50'000,
                    .visibleLayers = 1,
                    .maximumCandidates = 2};
        }

        TEST_CASE("Perception broadphase prunes far records and truncates in stable identity order", "[unit][ai][perception][spatial]") {
            auto scene = MakeScene();
            const auto view = scene->View();
            const auto listener = Listener(Entity(view, 1));
            std::vector<PerceptionSpatialSource> sources;
            for (std::uint64_t object = 24; object >= 2; --object)
                sources.push_back(Source(Entity(view, object), object <= 5 ? static_cast<std::int64_t>(object * 10'000)
                                                                           : static_cast<std::int64_t>(object * 100'000)));
            PerceptionSpatialBroadphase broadphase;
            const auto published = broadphase.Publish(view, 1, std::span{&listener, 1}, sources);
            REQUIRE(published.HasValue());
            const auto query = SightQuery(listener.entity);
            const auto result = published.Value()->Query(query);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().count == 2);
            CHECK(result.Value().truncated);
            CHECK(result.Value().candidates[0].entity == Entity(view, 2));
            CHECK(result.Value().candidates[1].entity == Entity(view, 3));
            CHECK(result.Value().examinedSources < sources.size());
            CHECK(published.Value()->Query(query).Value().candidates[0].entity == result.Value().candidates[0].entity);
            CHECK(published.Value()->ListenerPosition(listener.entity).Value() == listener.position);
            ExpectError(published.Value()->ListenerPosition(sources[0].entity), AIErrors::PerceptionSpatialListenerMissing);
        }

        TEST_CASE("Perception broadphase applies range layer affiliation and both sense declarations", "[unit][ai][perception][spatial]") {
            auto scene = MakeScene(5);
            const auto view = scene->View();
            const auto listener = Listener(Entity(view, 1));
            auto soundOnly = Source(Entity(view, 2), 50'000, 1, 7);
            soundOnly.senses[0] = SenseTypeIds::Hearing;
            const std::array sources{soundOnly, Source(Entity(view, 3), 49'000, 2, 7), Source(Entity(view, 4), 50'001, 1, 8),
                                     Source(Entity(view, 5), 50'000, 1, 7)};
            PerceptionSpatialBroadphase broadphase;
            const auto snapshot = broadphase.Publish(view, 1, std::span{&listener, 1}, sources);
            REQUIRE(snapshot.HasValue());
            auto query = SightQuery(listener.entity);
            query.affiliationFilter = PerceptionAffiliationFilter::Same;
            query.affiliation = 7;
            CHECK(snapshot.Value()->Query(query).Value().candidates[0].entity == Entity(view, 5));
            query.visibleLayers = 2;
            CHECK(snapshot.Value()->Query(query).Value().candidates[0].entity == Entity(view, 3));
            query.visibleLayers = 1;
            query.sense = SenseTypeIds::Hearing;
            CHECK(snapshot.Value()->Query(query).Value().candidates[0].entity == Entity(view, 2));
            query.affiliationFilter = PerceptionAffiliationFilter::Different;
            query.affiliation = 7;
            CHECK(snapshot.Value()->Query(query).Value().count == 0);
        }

        TEST_CASE("Perception broadphase publishes spawn and despawn together at the safe point", "[unit][ai][perception][spatial]") {
            auto scene = MakeScene(2);
            const auto before = scene->View();
            const auto listener = Listener(Entity(before, 1));
            const auto original = Source(Entity(before, 2), 1'000);
            PerceptionSpatialBroadphase broadphase;
            const auto first = broadphase.Publish(before, 1, std::span{&listener, 1}, std::span{&original, 1});
            REQUIRE(first.HasValue());
            Runtime::SceneCommandBuffer commands;
            commands.Destroy(original.entity);
            const auto deferred = commands.Create({});
            REQUIRE(deferred.IsValid());
            const auto committed = scene->Commit(commands);
            REQUIRE(committed.HasValue());
            REQUIRE(committed.Value().created.size() == 1);
            const auto replacement = Source(committed.Value().created[0].entity, 2'000);
            CHECK(first.Value()->Query(SightQuery(listener.entity)).Value().candidates[0].entity == original.entity);
            ExpectError(broadphase.Publish(before, 2, std::span{&listener, 1}, std::span{&replacement, 1}),
                        AIErrors::PerceptionSpatialStale);
            const auto second = broadphase.Publish(scene->View(), 2, std::span{&listener, 1}, std::span{&replacement, 1});
            REQUIRE(second.HasValue());
            CHECK(second.Value() == broadphase.Current());
            CHECK(second.Value()->Query(SightQuery(listener.entity)).Value().candidates[0].entity == replacement.entity);
            CHECK(first.Value()->Query(SightQuery(listener.entity)).Value().candidates[0].entity == original.entity);
        }

        TEST_CASE("Perception broadphase rejects malformed and duplicate publications without replacement",
                  "[unit][ai][perception][spatial]") {
            auto scene = MakeScene(3);
            const auto view = scene->View();
            const auto listener = Listener(Entity(view, 1));
            const auto source = Source(Entity(view, 2), 1'000);
            PerceptionSpatialBroadphase broadphase;
            const auto first = broadphase.Publish(view, 1, std::span{&listener, 1}, std::span{&source, 1});
            REQUIRE(first.HasValue());
            const std::array duplicates{source, source};
            ExpectError(broadphase.Publish(view, 2, std::span{&listener, 1}, duplicates), AIErrors::PerceptionSpatialConflict);
            auto malformed = source;
            malformed.layers = 0;
            ExpectError(broadphase.Publish(view, 2, std::span{&listener, 1}, std::span{&malformed, 1}), AIErrors::PerceptionSpatialInvalid);
            malformed = source;
            malformed.entity.entity.generation += 1;
            ExpectError(broadphase.Publish(view, 2, std::span{&listener, 1}, std::span{&malformed, 1}), AIErrors::PerceptionSpatialStale);
            CHECK(broadphase.Current() == first.Value());
            ExpectError(broadphase.Publish(view, 1, std::span{&listener, 1}, std::span{&source, 1}), AIErrors::PerceptionSpatialStale);
        }

        TEST_CASE("Perception broadphase rejects invalid queries and missing listeners", "[unit][ai][perception][spatial]") {
            auto scene = MakeScene(2);
            const auto view = scene->View();
            const auto listener = Listener(Entity(view, 1));
            const auto source = Source(Entity(view, 2), 1'000);
            PerceptionSpatialBroadphase broadphase;
            const auto snapshot = broadphase.Publish(view, 1, std::span{&listener, 1}, std::span{&source, 1});
            REQUIRE(snapshot.HasValue());
            auto query = SightQuery(listener.entity);
            query.maximumCandidates = 0;
            ExpectError(snapshot.Value()->Query(query), AIErrors::PerceptionSpatialInvalid);
            query = SightQuery(listener.entity);
            query.radiusMillimeters = PerceptionSpatialLimits::RadiusMillimeters + 1;
            ExpectError(snapshot.Value()->Query(query), AIErrors::PerceptionSpatialInvalid);
            query = SightQuery(source.entity);
            ExpectError(snapshot.Value()->Query(query), AIErrors::PerceptionSpatialListenerMissing);
        }

        TEST_CASE("Perception broadphase bounds extreme global coordinates and admission capacity", "[unit][ai][perception][spatial]") {
            auto scene = MakeScene(2);
            const auto view = scene->View();
            auto listener = Listener(Entity(view, 1));
            listener.position = Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min(), 0, 0);
            const auto source = Source(Entity(view, 2), std::numeric_limits<std::int64_t>::max());
            PerceptionSpatialBroadphase broadphase;
            const auto snapshot = broadphase.Publish(view, 1, std::span{&listener, 1}, std::span{&source, 1});
            REQUIRE(snapshot.HasValue());
            auto query = SightQuery(listener.entity);
            query.radiusMillimeters = PerceptionSpatialLimits::RadiusMillimeters;
            const auto result = snapshot.Value()->Query(query);
            REQUIRE(result.HasValue());
            CHECK(result.Value().count == 0);
            CHECK(result.Value().examinedSources == 0);
            std::vector overLimit(PerceptionSpatialLimits::Sources + 1, source);
            ExpectError(broadphase.Publish(view, 2, std::span{&listener, 1}, overLimit), AIErrors::PerceptionSpatialLimitExceeded);
            CHECK(broadphase.Current() == snapshot.Value());
        }

        TEST_CASE("Perception broadphase scene replacement fences old workers without retaining Scene", "[unit][ai][perception][spatial]") {
            auto firstScene = MakeScene(2, 77);
            const auto firstView = firstScene->View();
            const auto firstListener = Listener(Entity(firstView, 1));
            const auto firstSource = Source(Entity(firstView, 2), 1'000);
            PerceptionSpatialBroadphase broadphase;
            const auto first = broadphase.Publish(firstView, 1, std::span{&firstListener, 1}, std::span{&firstSource, 1});
            REQUIRE(first.HasValue());
            auto secondScene = MakeScene(2, 88);
            const auto secondView = secondScene->View();
            const auto secondListener = Listener(Entity(secondView, 1));
            const auto secondSource = Source(Entity(secondView, 2), 2'000);
            const auto second = broadphase.Publish(secondView, 1, std::span{&secondListener, 1}, std::span{&secondSource, 1});
            REQUIRE(second.HasValue());
            firstScene.reset();
            CHECK(second.Value()->Scene() == secondView.RuntimeId());
            CHECK(first.Value()->Query(SightQuery(firstListener.entity)).Value().count == 1);
            CHECK(first.Value()->ListenerPosition(firstListener.entity).Value() == firstListener.position);
            ExpectError(second.Value()->ListenerPosition(firstListener.entity), AIErrors::PerceptionSpatialInvalid);
            ExpectError(second.Value()->Query(SightQuery(firstListener.entity)), AIErrors::PerceptionSpatialInvalid);
        }
    }  // namespace
}  // namespace Horo::AI
