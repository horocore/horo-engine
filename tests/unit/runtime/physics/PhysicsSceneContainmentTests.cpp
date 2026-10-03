#include "PhysicsSceneActivationTestSupport.h"
#include "PhysicsWorldInternal.h"

#include <limits>

namespace Horo::Physics {
    /** @brief Borrows the aggregate world solely for deterministic containment regression coverage. */
    struct PhysicsSceneContainmentTestAccess final {
        static PhysicsWorld &World(PhysicsSceneActivationCandidate &candidate) {
            return *candidate.physics_;
        }
    };

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        TEST_CASE("Scene quarantine retires authored bindings while preserving the finite aggregate", "[physics][scene][nonfinite]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
            AssetSceneFixture assets;
            const auto definition = PhysicsDefinition(assets.material, assets.materialType);
            const auto view = assets.Prepare(definition);
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings(16, PhysicsNonFinitePolicy::QuarantineBody)};
            auto prepared = participant.Prepare(definition, view).Value();
            auto *candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(prepared.get());
            REQUIRE(candidate != nullptr);
            auto &world = PhysicsSceneContainmentTestAccess::World(*candidate);
            const auto retired = candidate->FindBody({1}, {100}).value();
            const auto survivor = candidate->FindBody({2}, {101}).value();
            REQUIRE(PhysicsWorldContainmentTestAccess::Inject(world, retired, std::numeric_limits<float>::infinity()));
            REQUIRE(world.AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasValue());
            REQUIRE_FALSE(candidate->FindBody({1}, {100}).has_value());
            REQUIRE_FALSE(candidate->FindShape({1}, {200}).has_value());
            REQUIRE_FALSE(candidate->FindConstraint({1}, {500}).has_value());
            REQUIRE(candidate->ConstraintBindings().empty());
            const auto bodies = candidate->BodyBindings();
            REQUIRE(bodies.size() == 1);
            REQUIRE(bodies.front().handle == survivor);
            REQUIRE(candidate->ShapeBindings().size() == bodies.size());
            REQUIRE(candidate->FindBody({2}, {101}) == survivor);
            REQUIRE(world.ReadSceneBodyReconciliation(survivor).HasValue());
            REQUIRE(candidate->ValidatePublication().HasValue());
        }

    }  // namespace
#endif
}  // namespace Horo::Physics
