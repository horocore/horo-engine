#include "Horo/Vfx/CpuParticleSpawnPipeline.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] VfxIdentityScope Scope() {
            return VfxIdentityScope::Create(42).Value();
        }

        [[nodiscard]] ParticleBufferId BufferId(const std::uint32_t slot = 20) {
            return MakeVfxIdentity<ParticleBufferIdentityTag>(Scope(), slot, 1).Value();
        }

        [[nodiscard]] EffectSystemId Activation() {
            return MakeVfxIdentity<EffectSystemIdentityTag>(Scope(), 10, 2).Value();
        }

        [[nodiscard]] ParticleSystemDescriptor Descriptor(const std::uint32_t capacity = 64, const double rate = 0.0,
                                                          const double lifetime = 10.0,
                                                          const ParticleEmitterShape shape = ParticleEmitterShape::Point) {
            auto data = Tests::ValidParticleDescriptorData();
            data.simulationPreference = SimulationPreference::RequireCPU;
            data.maximumParticles = capacity;
            data.shape = shape;
            data.spawnRate = {rate, rate};
            data.lifetimeSeconds = {lifetime, lifetime};
            data.initialSpeed = {2.0, 2.0};
            data.initialSize = {0.5, 0.5};
            data.initialOpacity = {0.75, 0.75};
            data.collisionMode = ParticleCollisionMode::None;
            return Tests::ParticleDescriptor(std::move(data));
        }

        [[nodiscard]] CpuParticleSpawnPipeline Pipeline(const ParticleSystemDescriptor &descriptor, const std::uint32_t bufferSlot = 20,
                                                        const std::uint64_t seed = 1234) {
            auto pipeline =
                CpuParticleSpawnPipeline::Create(descriptor, {.buffer = BufferId(bufferSlot),
                                                              .activation = Activation(),
                                                              .effectSeed = seed,
                                                              .maximumBurstParticles = CpuParticleSpawnHardLimits::BurstParticles});
            REQUIRE(pipeline.HasValue());
            return std::move(pipeline).Value();
        }

        template <typename Value> [[nodiscard]] bool HasError(const Result<Value> &result, const ErrorCodeDescriptor &descriptor) {
            return result.HasError() && result.ErrorValue().code.Value() == descriptor.code.Value();
        }
    }  // namespace

    TEST_CASE("CPU spawn pipeline combines continuous carry and bounded bursts", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(8, 2.0));

        CHECK(pipeline.Advance({.deltaSeconds = 0.25F}).Value().spawned == 0);
        CHECK(pipeline.Advance({.deltaSeconds = 0.25F}).Value().spawned == 1);
        const auto burst = pipeline.Advance({.burstCount = 3}).Value();

        CHECK(burst.requestedBirths == 3);
        CHECK(burst.spawned == 3);
        CHECK(burst.dropped == 0);
        CHECK(burst.active == 4);
        CHECK(pipeline.Statistics().spawned == 4);
        CHECK(pipeline.Statistics().nextSpawnOrdinal == 4);
    }

    TEST_CASE("CPU spawn initialization is reproducible for identical seeds", "[unit][vfx][particle-spawn]") {
        const ParticleSystemDescriptor descriptor = Descriptor(16, 0.0, 10.0, ParticleEmitterShape::Sphere);
        CpuParticleSpawnPipeline first = Pipeline(descriptor, 20, 0xA55A);
        CpuParticleSpawnPipeline second = Pipeline(descriptor, 21, 0xA55A);
        REQUIRE(first.Advance({.burstCount = 16}).HasValue());
        REQUIRE(second.Advance({.burstCount = 16}).HasValue());

        const CpuParticleSoAView left = first.View().Value();
        const CpuParticleSoAView right = second.View().Value();
        for (std::size_t index = 0; index < left.positionX.size(); ++index) {
            const auto leftHandle = first.HandleAtDenseIndex(static_cast<std::uint32_t>(index));
            const auto rightHandle = second.HandleAtDenseIndex(static_cast<std::uint32_t>(index));
            REQUIRE(leftHandle.HasValue());
            REQUIRE(rightHandle.HasValue());
            CHECK(leftHandle.Value().particle == rightHandle.Value().particle);
            CHECK(left.positionX[index] == right.positionX[index]);
            CHECK(left.positionY[index] == right.positionY[index]);
            CHECK(left.positionZ[index] == right.positionZ[index]);
            CHECK(left.velocityX[index] == right.velocityX[index]);
            CHECK(left.maximumAge[index] == right.maximumAge[index]);
            CHECK(left.packedColor[index] == right.packedColor[index]);
        }
    }

    TEST_CASE("CPU spawn shapes remain inside canonical unit geometry", "[unit][vfx][particle-spawn]") {
        const std::array shapes{ParticleEmitterShape::Point, ParticleEmitterShape::Sphere, ParticleEmitterShape::Box,
                                ParticleEmitterShape::Cone};
        for (std::uint32_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
            CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(32, 0.0, 10.0, shapes[shapeIndex]), 30 + shapeIndex);
            REQUIRE(pipeline.Advance({.burstCount = 32}).HasValue());
            const CpuParticleSoAView view = pipeline.View().Value();
            for (std::size_t index = 0; index < view.positionX.size(); ++index) {
                const float x = view.positionX[index];
                const float y = view.positionY[index];
                const float z = view.positionZ[index];
                CHECK(x >= -1.0F);
                CHECK(x <= 1.0F);
                CHECK(y >= -1.0F);
                CHECK(y <= 1.0F);
                CHECK(z >= -1.0F);
                CHECK(z <= 1.0F);
                if (shapes[shapeIndex] == ParticleEmitterShape::Sphere)
                    CHECK((x * x) + (y * y) + (z * z) <= 1.00001F);
                if (shapes[shapeIndex] == ParticleEmitterShape::Cone)
                    CHECK((x * x) + (y * y) <= (z * z) + 0.00001F);
            }
        }
    }

    TEST_CASE("CPU spawn pipeline expires and explicitly kills through stable handles", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(4, 0.0, 0.5));
        REQUIRE(pipeline.Advance({.burstCount = 3}).HasValue());
        const CpuParticleHandle explicitVictim = pipeline.HandleAtDenseIndex(1).Value();
        REQUIRE(pipeline.SignalKill(explicitVictim).HasValue());

        const auto firstKill = pipeline.Advance({.deltaSeconds = 0.25F}).Value();
        CHECK(firstKill.killed == 1);
        CHECK(firstKill.active == 2);
        CHECK(HasError(pipeline.SignalKill(explicitVictim), VfxErrors::ParticleHandleStale));

        const auto expiry = pipeline.Advance({.deltaSeconds = 0.25F}).Value();
        CHECK(expiry.killed == 2);
        CHECK(expiry.active == 0);
    }

    TEST_CASE("CPU spawn initialization clears recycled transient state", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(1, 0.0, 10.0));
        REQUIRE(pipeline.Advance({.burstCount = 1}).HasValue());
        CpuParticleSoAView first = pipeline.View().Value();
        first.age[0] = 9.0F;
        first.angularVelocity[0] = 17.0F;
        first.customFlags[0] = std::numeric_limits<std::uint32_t>::max();
        const CpuParticleHandle retired = pipeline.HandleAtDenseIndex(0).Value();
        REQUIRE(pipeline.SignalKill(retired).HasValue());
        REQUIRE(pipeline.Advance({}).Value().killed == 1);

        const auto replacement = pipeline.Advance({.burstCount = 1}).Value();
        REQUIRE(replacement.spawned == 1);
        REQUIRE(replacement.killed == 0);
        const CpuParticleSoAView recycled = pipeline.View().Value();
        CHECK(recycled.age[0] == 0.0F);
        CHECK(recycled.angularVelocity[0] == 0.0F);
        CHECK(recycled.customFlags[0] == 0U);
    }

    TEST_CASE("CPU spawn handles are limited to the active dense prefix", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(4));
        REQUIRE(pipeline.Advance({.burstCount = 1}).HasValue());

        REQUIRE(pipeline.HandleAtDenseIndex(0).HasValue());
        CHECK(HasError(pipeline.HandleAtDenseIndex(1), VfxErrors::ParticleHandleInvalid));
        CHECK(HasError(pipeline.HandleAtDenseIndex(3), VfxErrors::ParticleHandleInvalid));
    }

    TEST_CASE("CPU spawn capacity reports rejected births without replacing live particles", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(3));
        const auto result = pipeline.Advance({.burstCount = 8}).Value();

        CHECK(result.requestedBirths == 8);
        CHECK(result.spawned == 3);
        CHECK(result.dropped == 5);
        CHECK(result.active == 3);
        CHECK(pipeline.Statistics().dropped == 5);
    }

    TEST_CASE("CPU spawn steady-state burst churn performs no allocation", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(32, 0.0, 0.01));
        const std::size_t before = ::Horo::Tests::AllocationProbe::Count();
        bool succeeded = true;
        for (std::uint32_t step = 0; step < 10'000; ++step) {
            auto result = pipeline.Advance({.deltaSeconds = 0.01F, .burstCount = 32});
            succeeded = succeeded && result.HasValue() && result.Value().spawned == 32 && result.Value().killed == 32;
        }
        const std::size_t after = ::Horo::Tests::AllocationProbe::Count();

        CHECK(succeeded);
        CHECK(after == before);
        CHECK(pipeline.Statistics().spawned == 320'000);
        CHECK(pipeline.Statistics().killed == 320'000);
        CHECK(pipeline.View().Value().age.empty());
    }

    TEST_CASE("CPU spawn invalid cancellation and shutdown paths preserve state", "[unit][vfx][particle-spawn]") {
        const ParticleSystemDescriptor descriptor = Descriptor(4, 2.0);
        CHECK(CpuParticleSpawnPipeline::Create(descriptor, {}).HasError());
        auto unrepresentable = Tests::ValidParticleDescriptorData();
        unrepresentable.initialSpeed = {0.0, std::numeric_limits<double>::max()};
        const ParticleSystemDescriptor wideDescriptor = Tests::ParticleDescriptor(std::move(unrepresentable));
        CHECK(HasError(CpuParticleSpawnPipeline::Create(wideDescriptor,
                                                        {.buffer = BufferId(),
                                                         .activation = Activation(),
                                                         .maximumBurstParticles = CpuParticleSpawnHardLimits::BurstParticles}),
                       VfxErrors::ParticleSpawnStepInvalid));
        CpuParticleSpawnPipeline pipeline = Pipeline(descriptor);
        REQUIRE(pipeline.Advance({.burstCount = 1}).HasValue());

        CHECK(
            HasError(pipeline.Advance({.deltaSeconds = 0.25F, .burstCount = 1, .cancelled = true}), VfxErrors::ParticleSpawnStepCancelled));
        CHECK(HasError(pipeline.Advance({.deltaSeconds = CpuParticleSpawnHardLimits::DeltaSeconds + 1.0F}),
                       VfxErrors::ParticleSpawnStepInvalid));
        CHECK(pipeline.View().Value().age.size() == 1);

        REQUIRE(pipeline.Shutdown().HasValue());
        REQUIRE(pipeline.Shutdown().HasValue());
        CHECK(HasError(pipeline.Advance({}), VfxErrors::ParticleBufferShutDown));
    }

    TEST_CASE("CPU spawn pipeline rejects mutation away from its owner thread", "[unit][vfx][particle-spawn]") {
        CpuParticleSpawnPipeline pipeline = Pipeline(Descriptor(4));
        std::atomic<bool> rejected{};
        std::thread worker([&] {
            const auto result = pipeline.Advance({.burstCount = 1});
            rejected.store(HasError(result, VfxErrors::ParticleBufferThreadViolation));
        });
        worker.join();

        CHECK(rejected.load());
        CHECK(pipeline.View().Value().age.empty());
    }
}  // namespace Horo::Vfx
