#include "AllocationProbe.h"
#include "PhysicsStateSolverCodecInternal.h"
#include "PhysicsStateSolverStorage.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Typed format fixture only; genuine solver execution is covered
         * separately by native tests. */
        struct Values final {
            std::array<PhysicsStateBody, 2> bodies{{{.id = 9, .hasMotion = true, .active = true}, {.id = 42}}};
            std::array<PhysicsStateObjectId, 1> active{9};
            std::array<PhysicsStateConstraint, 1> joints{{{.id = 71, .kind = PhysicsStateConstraintKind::Fixed, .enabled = true}}};
            std::array<PhysicsStateBodyPair, 1> pairs{{{.first = 9, .second = 42, .manifoldCount = 1}}};
            std::array<PhysicsStateManifold, 1> manifolds{{{.key = {9, 42, ~0U, ~0U}, .normal = {0, 1, 0}, .pointCount = 1}}};

            Values() {
                bodies[0].motion.linearVelocity = {-0.0F, std::numeric_limits<float>::denorm_min(), 2.0F};
                bodies[0].motion.force = {0, 3, 0};
                bodies[0].motion.sleepTimer = 0.25F;
                bodies[0].motion.allowSleeping = true;
                joints[0].rotationLambda = {1, 2, 3};
                joints[0].pointLambda = {4, 5, 6};
                manifolds[0].points[0].normalLambda = 7.0F;
                manifolds[0].frictionLambda = {-2, 3};
                manifolds[0].angularFrictionLambda = -0.5F;
            }

            PhysicsStateSolverView View() const noexcept {
                return {1.0F / 60.0F, {0, -9.81F, 0}, bodies, active, joints, pairs, manifolds, {}};
            }
        };

        PhysicsStateLimits Limits() noexcept {
            return {4, 4, 4, 4, 4, 16 * 1024};
        }

        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : std::string{"success"}));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

    TEST_CASE("Physics state solver wire roundtrips every hidden scalar with exact "
              "float bits",
              "[physics][state][codec]") {
        Values values;
        std::array<std::uint8_t, 4> scratch{};
        std::array<std::byte, 2048> encoded{}, again{};
        auto storage = Take(PrepareSolverStorage(Limits()));
        const auto size = Take(EncodeSolverState(values.View(), Limits(), scratch, encoded));
        auto decoded = Take(DecodeSolverState(std::span{encoded}.first(size), *storage));
        REQUIRE(std::bit_cast<std::uint32_t>(decoded.bodies[0].motion.linearVelocity[0]) == 0x80000000U);
        REQUIRE(std::bit_cast<std::uint32_t>(decoded.bodies[0].motion.linearVelocity[1]) == 1U);
        REQUIRE(decoded.constraints[0].pointLambda == values.joints[0].pointLambda);
        REQUIRE(decoded.manifolds[0].frictionLambda == values.manifolds[0].frictionLambda);
        const auto second = Take(EncodeSolverState(decoded, Limits(), scratch, again));
        REQUIRE(second == size);
        CHECK(std::ranges::equal(std::span{encoded}.first(size), std::span{again}.first(size)));
    }

    TEST_CASE("Physics solver wire rejects every truncation and required-version "
              "mismatch",
              "[physics][state][malformed]") {
        Values values;
        std::array<std::uint8_t, 4> scratch{};
        std::array<std::byte, 2048> encoded{};
        const auto size = Take(EncodeSolverState(values.View(), Limits(), scratch, encoded));
        auto storage = Take(PrepareSolverStorage(Limits()));
        for (std::size_t length = 0; length < size; ++length)
            REQUIRE(DecodeSolverState(std::span{encoded}.first(length), *storage).HasError());
        encoded[4] = std::byte{2};
        REQUIRE(DecodeSolverState(std::span{encoded}.first(size), *storage).HasError());
        encoded[4] = std::byte{1};
        REQUIRE(DecodeSolverState(std::span{encoded}.first(size + 1), *storage).HasError());
    }

    TEST_CASE("Physics solver format rejects ignored channels and contradictory activity", "[physics][state][malformed]") {
        Values values;
        std::array<std::uint8_t, 4> scratch{};
        SECTION("static motion tail") {
            values.bodies[1].motion.torque[0] = 1;
        }
        SECTION("unused fixed-joint motor") {
            values.joints[0].motorState = 1;
        }
        SECTION("unused manifold point") {
            values.manifolds[0].points[1].normalLambda = 1;
        }
        SECTION("missing active body") {
            values.active[0] = 42;
        }
        SECTION("unordered body records") {
            std::swap(values.bodies[0], values.bodies[1]);
        }
        SECTION("non-finite force") {
            values.bodies[0].motion.force[0] = std::numeric_limits<float>::infinity();
        }
        REQUIRE(ValidatePhysicsStateSolver(values.View(), Limits(), scratch).HasError());
    }

    TEST_CASE("Physics solver wire reports insufficient output before changing its bytes", "[physics][state][bounds]") {
        Values values;
        std::array<std::uint8_t, 4> scratch{};
        std::array<std::byte, 2048> expected{};
        const auto size = Take(EncodeSolverState(values.View(), Limits(), scratch, expected));
        std::array<std::byte, 2048> output;
        output.fill(std::byte{0xA5});
        REQUIRE(EncodeSolverState(values.View(), Limits(), scratch, std::span{output}.first(size - 1)).HasError());
        CHECK(std::ranges::all_of(output, [](auto byte) {
            return byte == std::byte{0xA5};
        }));
    }

    TEST_CASE("Physics solver codec reuses cold storage without heap allocation", "[physics][state][allocation]") {
        Values values;
        std::array<std::uint8_t, 4> scratch{};
        std::array<std::byte, 2048> encoded{}, again{};
        auto storage = Take(PrepareSolverStorage(Limits()));
        const auto size = Take(EncodeSolverState(values.View(), Limits(), scratch, encoded));
        Take(DecodeSolverState(std::span{encoded}.first(size), *storage));
        Tests::AllocationProbe::Measurement measured;
        bool success{};
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto first = EncodeSolverState(values.View(), Limits(), scratch, encoded);
            auto decoded = DecodeSolverState(std::span{encoded}.first(size), *storage);
            if (decoded.HasValue()) {
                auto second = EncodeSolverState(decoded.Value(), Limits(), scratch, again);
                success = first.HasValue() && second.HasValue();
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(success);
        CHECK(measured.requests == 0);
    }

    TEST_CASE("Physics solver storage failures preserve an existing independent capture", "[physics][state][allocation]") {
        auto retained = Take(PrepareSolverStorage(Limits()));
        retained->bodies[0].id = 901;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto prepared = PrepareSolverStorage(Limits());
            measured = measurement.Snapshot();
        }
        REQUIRE(measured.requests > 0);
        for (std::size_t allocation = 0; allocation < measured.requests; ++allocation) {
            bool rejected{};
            {
                Tests::AllocationProbe::ScopedFailure failure{allocation};
                rejected = PrepareSolverStorage(Limits()).HasError();
            }
            REQUIRE(rejected);
            CHECK(retained->bodies[0].id == 901);
        }
    }
}  // namespace Horo::Physics::StateDetail
