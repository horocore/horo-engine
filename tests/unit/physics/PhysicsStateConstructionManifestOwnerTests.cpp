#include "AllocationProbe.h"
#include "PhysicsStateConstructionManifestOwner.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        constexpr PhysicsStateFpControl Profile{PhysicsStateFpArchitecture::ArmFpcr, 0};

        PhysicsStateConstructionManifestLimits Limits() {
            return {.shapes = {4, 4, 4, 4, 32768}, .maximumRetainedBytes = 65536, .maximumEncodedBytes = 1024};
        }

        template <class T> T TakeOwner(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        PhysicsStateConstructionManifest ShapeOnly(ConstructionManifestOwner &owner) {
            auto staging = owner.Staging();
            staging.shapes.nodes[0] = {Profile, PhysicsShapeDescriptor{PhysicsBoxShape{}}};
            staging.shapes.roots[0] = {19, 0};
            return {.shapes = {staging.shapes.nodes.first(1), {}, staging.shapes.roots.first(1)}};
        }
    }  // namespace

    TEST_CASE("Cold manifest owner seals and decodes same canonical value without hot growth",
              "[unit][physics][state][construction-owner]") {
        auto source = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        const auto manifest = ShapeOnly(*source);
        const auto before = Tests::AllocationProbe::Count();
        auto sealed = source->Seal(manifest, Profile);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(sealed.HasValue());
        CHECK(before == after);
        CHECK(source->IsSealed());
        CHECK(source->RetainedPayloadBytes() <= Limits().maximumRetainedBytes);
        CHECK(source->Staging().shapes.nodes.empty());
        auto receiver = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        const auto decodeBefore = Tests::AllocationProbe::Count();
        auto decoded = receiver->Decode(source->Bytes(), Profile);
        const auto decodeAfter = Tests::AllocationProbe::Count();
        REQUIRE(decoded.HasValue());
        CHECK(decodeBefore == decodeAfter);
        CHECK(std::ranges::equal(source->Bytes(), receiver->Bytes()));
        CHECK(receiver->View().shapes.roots[0].shape == 19);
        CHECK(receiver->View().shapes.nodes.data() != source->View().shapes.nodes.data());
    }

    TEST_CASE("Manifest owner rejects quota foreign views and rewrite without changing old immutable bytes",
              "[unit][physics][state][construction-owner]") {
        auto small = Limits();
        small.maximumRetainedBytes = sizeof(ConstructionManifestOwner) - 1;
        CHECK(ConstructionManifestOwner::Prepare(small).HasError());
        auto owner = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        auto foreign = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        const auto foreignView = ShapeOnly(*foreign);
        CHECK(owner->Seal(foreignView, Profile).HasError());
        CHECK_FALSE(owner->IsSealed());
        auto manifest = ShapeOnly(*owner);
        REQUIRE(owner->Seal(manifest, Profile).HasValue());
        std::array<std::byte, 1024> original{};
        std::copy(owner->Bytes().begin(), owner->Bytes().end(), original.begin());
        const auto size = owner->Bytes().size();
        CHECK(owner->Seal(manifest, Profile).HasError());
        CHECK(owner->Decode(std::span{original}.first(size - 1), Profile).HasError());
        CHECK(std::equal(owner->Bytes().begin(), owner->Bytes().end(), original.begin()));
        CHECK(owner->View().shapes.roots[0].shape == 19);
    }

    TEST_CASE("Failed private manifest decode issues no sealed value and remains retryable", "[unit][physics][state][construction-owner]") {
        auto source = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        REQUIRE(source->Seal(ShapeOnly(*source), Profile).HasValue());
        auto receiver = TakeOwner(ConstructionManifestOwner::Prepare(Limits()));
        CHECK(receiver->Decode(source->Bytes().first(source->Bytes().size() - 1), Profile).HasError());
        CHECK_FALSE(receiver->IsSealed());
        CHECK(receiver->Bytes().empty());
        REQUIRE(receiver->Decode(source->Bytes(), Profile).HasValue());
        CHECK(std::ranges::equal(receiver->Bytes(), source->Bytes()));
    }
}  // namespace Horo::Physics::StateDetail
