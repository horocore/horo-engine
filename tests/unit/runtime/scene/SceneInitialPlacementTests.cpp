#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        enum class PlacementFault {
            None,
            Duplicate,
            ForeignOwner,
            NonUnitHeading
        };

        struct PlacementProbe final {
            PlacementFault fault{};
            std::vector<int> phases;
            std::size_t published{}, retired{};
            Math::Vec3 target{10, 3, 0};
            Math::Quaternion heading{Math::Quaternion::Identity()};
            bool derivedSeesPlacement{};
        };

        class PlacementCandidate final : public SceneActivationCandidate {
        public:
            PlacementCandidate(PlacementProbe &probe, EntityRef owner) : probe_(probe) {
                facts_.push_back({owner, probe.target, probe.heading});
                if (probe.fault == PlacementFault::Duplicate)
                    facts_.push_back(facts_.front());
                if (probe.fault == PlacementFault::ForeignOwner)
                    ++facts_[0].owner.runtime.value;
                if (probe.fault == PlacementFault::NonUnitHeading)
                    facts_[0].heading = {0, 0, 0, 2};
            }

            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }

            std::span<const SceneInitialPlacement> InitialPlacements() const noexcept override {
                return facts_;
            }

            void Publish() noexcept override {
                ++probe_.published;
            }

            void Shutdown() noexcept override {
                if (!std::exchange(retired_, true))
                    ++probe_.retired;
            }

        private:
            PlacementProbe &probe_;
            std::vector<SceneInitialPlacement> facts_;
            bool retired_{};
        };

        class PlacementProducer final : public SceneActivationParticipant {
        public:
            explicit PlacementProducer(PlacementProbe &probe) : probe_(probe) {}

            SceneActivationPhase PreparationPhase() const noexcept override {
                return SceneActivationPhase::InitialPlacement;
            }

            Result<void> PrepareOwnedScene(RuntimeScene &) override {
                probe_.phases.push_back(1);
                return Result<void>::Success();
            }

            Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView scene) override {
                probe_.phases.push_back(2);
                const auto owner = scene.Find({2});
                REQUIRE(owner.has_value());
                return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<PlacementCandidate>(probe_, *owner));
            }

        private:
            PlacementProbe &probe_;
        };

        class DerivedCandidate final : public SceneActivationCandidate {
        public:
            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}
        };

        class PoseConsumer final : public SceneActivationParticipant {
        public:
            explicit PoseConsumer(PlacementProbe &probe) : probe_(probe) {}

            Result<void> PrepareOwnedScene(RuntimeScene &scene) override {
                probe_.phases.push_back(3);
                const auto owner = scene.View().Find({2});
                REQUIRE(owner.has_value());
                const auto local = scene.View().Get(*owner);
                REQUIRE(local.HasValue());
                probe_.derivedSeesPlacement = local.Value().localTransform->translation == Math::Vec3{4, 1.5F, 0};
                return Result<void>::Success();
            }

            Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &, RuntimeSceneView) override {
                probe_.phases.push_back(4);
                return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<DerivedCandidate>());
            }

        private:
            PlacementProbe &probe_;
        };

        class AdmissionCheck final : public ScenePublicationCheck {
        public:
            explicit AdmissionCheck(std::size_t &calls) : calls_(calls) {}

            Result<void> ValidatePublication() const override {
                ++calls_;
                return Result<void>::Success();
            }

        private:
            std::size_t &calls_;
        };

        class AdmissionRestore final : public SceneAggregateRestore {
        public:
            explicit AdmissionRestore(std::size_t &calls) : calls_(calls) {}

            Result<void> PrepareScene(RuntimeScene &) override {
                ++calls_;
                return Result<void>::Success();
            }

            Result<bool> PrepareOwners() override {
                ++calls_;
                return Result<bool>::Success(true);
            }

            Result<void> ValidatePublication(const RuntimeSceneView *) const override {
                ++calls_;
                return Result<void>::Success();
            }

            Result<void> Commit(IStagedRestoreAggregatePublication &) override {
                ++calls_;
                return Result<void>::Success();
            }

            void Rollback() noexcept override {
                ++calls_;
            }

        private:
            std::size_t &calls_;
        };

        RuntimeSceneDefinition PlacementDefinition(std::uint64_t revision, Math::Vec3 parentScale = {2, 2, 2},
                                                   Math::Quaternion parentHeading = Math::Quaternion::Identity()) {
            SceneDefinitionBuilder builder{{1}, {revision}};
            builder.Add({.object = {1}, .localTransform = {.translation = {2, 0, 0}, .rotation = parentHeading, .scale = parentScale}});
            builder.Add(
                {.object = {2}, .parent = SceneObjectId{1}, .localTransform = {.translation = {0, 1, 0}, .scale = {0.5F, 0.5F, 0.5F}}});
            builder.Add({.object = {3}, .parent = SceneObjectId{2}, .localTransform = {.translation = {0, 1, 0}}});
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        void Commit(RuntimeSceneService &service) {
            const CancellationToken cancellation;
            const FrameContext context{.frameNumber = 1, .cancellation = cancellation};
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
        }

        void RegisterPlacement(RuntimeSceneService &service, PlacementProbe &probe) {
            // Register in reverse dependency order to prove the phase contract, not incidental order.
            REQUIRE(service.AddActivationParticipant(std::make_unique<PoseConsumer>(probe)).HasValue());
            REQUIRE(service.AddActivationParticipant(std::make_unique<PlacementProducer>(probe)).HasValue());
            REQUIRE(service.Startup({}).HasValue());
        }
    }  // namespace

    TEST_CASE("Initial placement precedes derived mutable preparation and preserves authored hierarchy and scale", "[scene][placement]") {
        PlacementProbe probe;
        RuntimeSceneService service;
        RegisterPlacement(service, probe);
        const auto definition = PlacementDefinition(1);
        REQUIRE(service.QueuePreparation(definition).HasValue());
        Commit(service);
        CHECK_FALSE(service.TakeOperationError().has_value());
        REQUIRE(service.ActiveScene().has_value());
        CHECK(probe.phases == std::vector<int>{1, 2, 3, 4});
        CHECK(probe.derivedSeesPlacement);
        CHECK(probe.published == 1);
        const auto child = service.ActiveScene()->Get(*service.ActiveScene()->Find({2}));
        REQUIRE(child.HasValue());
        CHECK(child.Value().localTransform->scale == Math::Vec3{0.5F, 0.5F, 0.5F});
        CHECK(definition.Entities()[1].localTransform.translation == Math::Vec3{0, 1, 0});
        REQUIRE(service.QueueUnload().HasValue());
        Commit(service);
        CHECK_FALSE(service.ActiveScene().has_value());
        CHECK(probe.retired == 1);
        service.Shutdown();
        CHECK(probe.retired == 1);
    }

    TEST_CASE("Invalid initial placement rolls back before derived preparation and preserves the old Scene", "[scene][placement]") {
        for (const auto fault : {PlacementFault::Duplicate, PlacementFault::ForeignOwner, PlacementFault::NonUnitHeading}) {
            PlacementProbe probe;
            RuntimeSceneService service;
            RegisterPlacement(service, probe);
            REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasValue());
            Commit(service);
            REQUIRE(service.ActiveScene().has_value());
            const auto old = service.ActiveScene()->RuntimeId();
            probe.fault = fault;
            probe.phases.clear();
            const auto rejected = service.QueuePreparation(PlacementDefinition(2));
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "scene.runtime.invalid_candidate");
            REQUIRE(service.ActiveScene().has_value());
            CHECK(service.ActiveScene()->RuntimeId() == old);
            CHECK(probe.phases == std::vector<int>{1, 2});
            CHECK(probe.published == 1);
            CHECK(probe.retired == 1);
            service.Shutdown();
            CHECK(probe.retired == 2);
        }
    }

    TEST_CASE("Initial placement preserves representable mirrored and nonuniform rotated parent transforms", "[scene][placement]") {
        PlacementProbe probe;
        Math::Vec3 scale{-2, 2, 2};
        Math::Quaternion heading = Math::Quaternion::Identity();
        SECTION("mirrored parent") {}
        SECTION("nonuniform parent with aligned nonidentity heading") {
            scale = {2, 1, 1};
            heading = {0, 0, 0.70710677F, 0.70710677F};
            probe.heading = heading;
        }
        RuntimeSceneService service;
        RegisterPlacement(service, probe);
        const auto authored = PlacementDefinition(1, scale, heading);
        REQUIRE(service.QueuePreparation(authored).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene().has_value());
        const auto view = service.ActiveScene()->Get(*service.ActiveScene()->Find({2}));
        REQUIRE(view.HasValue());
        CHECK(Math::NearlyEqual(view.Value().localTransform->scale, Math::Vec3{0.5F, 0.5F, 0.5F}));
        CHECK(authored.Entities()[0].localTransform.scale == scale);
        CHECK(authored.Entities()[1].localTransform.translation == Math::Vec3{0, 1, 0});
        CHECK(probe.published == 1);
    }

    TEST_CASE("Initial placement rejects a near-singular parent before publishing any owner", "[scene][placement]") {
        PlacementProbe probe;
        RuntimeSceneService service;
        RegisterPlacement(service, probe);
        const auto authored = PlacementDefinition(1, {Math::DefaultEpsilon * 0.25F, 1, 1});
        REQUIRE(service.QueuePreparation(authored).HasError());
        CHECK_FALSE(service.ActiveScene());
        CHECK(probe.published == 0);
        CHECK(probe.retired == 1);
    }

    TEST_CASE("Initial placement rejects nonrepresentable local shear instead of rewriting authored scale", "[scene][placement]") {
        PlacementProbe probe;
        probe.heading = {0, 0, 0.38268343F, 0.9238795F};
        RuntimeSceneService service;
        RegisterPlacement(service, probe);
        const auto authored = PlacementDefinition(1, {2, 1, 1});
        const auto rejected = service.QueuePreparation(authored);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "scene.runtime.invalid_candidate");
        CHECK_FALSE(service.ActiveScene().has_value());
        CHECK(probe.published == 0);
        CHECK(probe.retired == 1);
        CHECK(authored.Entities()[0].localTransform.scale == Math::Vec3{2, 1, 1});
    }

    TEST_CASE("Explicit Scene identity seeds advance on replacement without changing default allocation", "[scene][identity]") {
        for (const std::uint64_t initial : {1ULL, 41ULL}) {
            PlacementProbe probe;
            RuntimeSceneService service{SceneRuntimeId{initial}};
            RegisterPlacement(service, probe);
            REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasValue());
            Commit(service);
            REQUIRE(service.ActiveScene().has_value());
            CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{initial});
            REQUIRE(service.QueuePreparation(PlacementDefinition(2)).HasValue());
            Commit(service);
            REQUIRE(service.ActiveScene().has_value());
            CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{initial + 1});
            CHECK(probe.published == 2);
            CHECK(probe.retired == 1);
            service.Shutdown();
            CHECK(probe.retired == 2);
        }
    }

    TEST_CASE("Zero Scene identity rejects every preparation entry before owner callbacks", "[scene][identity]") {
        PlacementProbe probe;
        RuntimeSceneService service{SceneRuntimeId{0}};
        RegisterPlacement(service, probe);
        std::size_t callbacks{};
        const auto ordinary = service.QueuePreparation(PlacementDefinition(1));
        REQUIRE(ordinary.HasError());
        CHECK(ordinary.ErrorValue().code.Value() == "scene.runtime.invalid_candidate");
        const auto checked =
            service.QueuePreparationWithPublicationCheck(PlacementDefinition(1), std::make_unique<AdmissionCheck>(callbacks));
        REQUIRE(checked.HasError());
        CHECK(checked.ErrorValue().code.Value() == ordinary.ErrorValue().code.Value());
        const auto restored = service.QueuePreparationWithRestore(PlacementDefinition(1), std::make_unique<AdmissionRestore>(callbacks));
        REQUIRE(restored.HasError());
        CHECK(restored.ErrorValue().code.Value() == ordinary.ErrorValue().code.Value());
        CHECK(callbacks == 0);
        CHECK(probe.phases.empty());
        CHECK(probe.published == 0);
        CHECK(probe.retired == 0);
        CHECK_FALSE(service.ActiveScene().has_value());
        service.Shutdown();
        CHECK(callbacks == 0);
    }

    TEST_CASE("Maximum Scene identity publishes once and exhaustion preserves the active aggregate", "[scene][identity]") {
        PlacementProbe probe;
        const SceneRuntimeId maximum{std::numeric_limits<std::uint64_t>::max()};
        RuntimeSceneService service{maximum};
        RegisterPlacement(service, probe);
        REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene().has_value());
        CHECK(service.ActiveScene()->RuntimeId() == maximum);
        probe.phases.clear();
        const auto replacement = service.QueuePreparation(PlacementDefinition(2));
        REQUIRE(replacement.HasError());
        CHECK(replacement.ErrorValue().code.Value() == "scene.runtime.invalid_candidate");
        CHECK(probe.phases.empty());
        CHECK(probe.published == 1);
        CHECK(probe.retired == 0);
        CHECK(service.ActiveScene()->RuntimeId() == maximum);
        service.Shutdown();
        CHECK(probe.retired == 1);
    }

    TEST_CASE("Failed private Scene aggregate preparation retains its identity for retry", "[scene][identity][placement]") {
        PlacementProbe probe;
        RuntimeSceneService service{SceneRuntimeId{41}};
        RegisterPlacement(service, probe);
        REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasValue());
        Commit(service);
        const auto old = service.ActiveScene()->RuntimeId();
        probe.fault = PlacementFault::Duplicate;
        REQUIRE(service.QueuePreparation(PlacementDefinition(2)).HasError());
        CHECK(service.ActiveScene()->RuntimeId() == old);
        CHECK(probe.published == 1);
        CHECK(probe.retired == 1);
        probe.fault = PlacementFault::None;
        REQUIRE(service.QueuePreparation(PlacementDefinition(2)).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene().has_value());
        CHECK(service.ActiveScene()->RuntimeId() == SceneRuntimeId{42});
        CHECK(probe.published == 2);
        CHECK(probe.retired == 2);
        service.Shutdown();
        CHECK(probe.retired == 3);
    }

    TEST_CASE("Failed maximum Scene identity preparation can retry before exhaustion", "[scene][identity][placement]") {
        PlacementProbe probe;
        probe.fault = PlacementFault::Duplicate;
        const SceneRuntimeId maximum{std::numeric_limits<std::uint64_t>::max()};
        RuntimeSceneService service{maximum};
        RegisterPlacement(service, probe);
        REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasError());
        CHECK_FALSE(service.ActiveScene().has_value());
        CHECK(probe.published == 0);
        CHECK(probe.retired == 1);
        probe.fault = PlacementFault::None;
        REQUIRE(service.QueuePreparation(PlacementDefinition(1)).HasValue());
        Commit(service);
        REQUIRE(service.ActiveScene().has_value());
        CHECK(service.ActiveScene()->RuntimeId() == maximum);
        REQUIRE(service.QueuePreparation(PlacementDefinition(2)).HasError());
        CHECK(service.ActiveScene()->RuntimeId() == maximum);
        CHECK(probe.published == 1);
        CHECK(probe.retired == 1);
        service.Shutdown();
        CHECK(probe.retired == 2);
    }
}  // namespace Horo::Runtime
