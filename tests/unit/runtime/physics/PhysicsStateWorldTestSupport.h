#pragma once

#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

namespace Horo::Physics::StateTest {
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Check(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
        REQUIRE(result.HasValue());
    }

    /** @brief Explicitly admits 32 native shapes/query fixtures; collider slots are a separate policy. */
    [[nodiscard]] inline PhysicsWorldSettings StateWorldSettings() {
        auto descriptor = Test::SmallWorldSettings().Values();
        descriptor.budgets.maximumShapes = 32;
        return Take(PhysicsWorldSettings::Capture(descriptor));
    }

    struct WorldLayout final {
        std::array<PhysicsStateBodyBinding, 3> bodies;
        std::array<PhysicsStateShapeBinding, 2> shapes;
    };

    [[nodiscard]] WorldLayout PopulateStateWorld(PhysicsWorld &world, const PhysicsSurfaceWorldBinding &surfaces = {});
    [[nodiscard]] PhysicsStateRuntimeBudget StateRuntimeBudget(
        std::uint32_t owners = 8, std::size_t bytes = 256U * 1024U * 1024U,
        PhysicsStateConstructionTracking tracking = PhysicsStateConstructionTracking::Required);

    /** @brief Genuine public World fixture with verified current process image, real generated facts, real Jolt and retained geometry. */
    class WorldFixture final {
    public:
        explicit WorldFixture(const PhysicsWorldSettings &settings = StateWorldSettings(),
                              PhysicsStateConstructionTracking tracking = PhysicsStateConstructionTracking::Required,
                              PhysicsSurfaceWorldBinding surfaces = {}, std::uint64_t filterGeneration = 1);
        ~WorldFixture();
        void Advance(std::uint32_t count = 1);
        [[nodiscard]] PhysicsStateCaptureConfiguration Configuration(std::uint32_t slots = 4) const;
        [[nodiscard]] std::size_t Configure(std::uint32_t slots = 4);
        [[nodiscard]] PhysicsStateSnapshotLease Capture();
        void Shutdown() noexcept;

        std::unique_ptr<PhysicsRuntime> runtime;
        std::unique_ptr<PhysicsWorld> world;
        std::shared_ptr<const std::string> content;
        std::shared_ptr<const NormalizedCollisionSchema> schema;
        PhysicsSurfaceWorldBinding surfaces;
        std::array<PhysicsStateBodyBinding, 3> bodies;
        std::array<PhysicsStateShapeBinding, 2> shapes;
        std::size_t configuredBytes{};

    private:
        void Build();
    };

    struct Transcript final {
        std::array<PhysicsEventRecord, 8> events;
        std::uint32_t count{};
    };

    [[nodiscard]] Transcript ReadTranscript(PhysicsWorld &world);
    [[nodiscard]] bool SamePublication(const PhysicsPublishedTick &one, const PhysicsPublishedTick &two) noexcept;
    [[nodiscard]] PhysicsStateBuildBinding ActualStaticBinding();
}  // namespace Horo::Physics::StateTest
