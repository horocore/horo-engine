#include "Horo/Vfx/VfxSpawnRequest.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] VfxSpawnRequest ValidRequest() {
            std::array<std::uint8_t, 16> bytes{};
            bytes[0] = 0x42;
            VfxSpawnRequest request;
            request.effectAsset = Assets::AssetId::FromBytes(bytes);
            request.scene = VfxIdentityScope::Create(41).Value();
            request.owner = VfxIdentityScope::Create(42).Value();
            request.ownerGeneration = 3;
            request.tick = 7;
            request.occurrence = {8, 9};
            request.bindingGeneration = 10;
            request.destination = 11;
            request.layerOrdinal = 2;
            request.transform.orientation = {0.0F, 0.0F, 0.0F, 1.0F};
            return request;
        }

        [[nodiscard]] VfxSpawnRequest ParameterizedRequest() {
            auto request = ValidRequest();
            request.parameters.schema = 17;
            request.parameters.count = 2;
            request.parameters.values[0] = {2, 0.5F};
            request.parameters.values[1] = {9, -0.25F};
            return request;
        }

        using Mutation = void (*)(VfxSpawnRequest &);
    }  // namespace

    TEST_CASE("VFX spawn requests accept bounded authored values without inventing absent normals", "[unit][vfx][spawn-request]") {
        CHECK(ValidateVfxSpawnRequest({}) == VfxSpawnStatus::InvalidRequest);
        auto request = ValidRequest();
        REQUIRE_FALSE(request.impactNormal.has_value());
        CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::Accepted);

        request.parameters.schema = 17;
        request.parameters.count = VfxSpawnHardLimits::Parameters;
        for (std::uint16_t channel = 0; channel < VfxSpawnHardLimits::Parameters; ++channel)
            request.parameters.values[channel] = {channel, static_cast<float>(channel)};
        request.impactNormal = Math::Vec3{0.0F, -1.0F, 0.0F};
        request.transform.orientation = {0.0F, 0.0F, 0.0F, -1.0F};
        request.requirement = VfxRequirementClass::GameplayRequired;
        CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::Accepted);
    }

    TEST_CASE("VFX spawn requests reject malformed identity and routing dimensions", "[unit][vfx][spawn-request]") {
        const std::array<Mutation, 12> mutations{
            [](VfxSpawnRequest &r) {
            r.effectAsset = {};
        },
            [](VfxSpawnRequest &r) {
            r.scene = {};
        },
            [](VfxSpawnRequest &r) {
            r.owner = {};
        },
            [](VfxSpawnRequest &r) {
            r.ownerGeneration = 0;
        },
            [](VfxSpawnRequest &r) {
            r.tick = 0;
        },
            [](VfxSpawnRequest &r) {
            r.occurrence.producer = 0;
        },
            [](VfxSpawnRequest &r) {
            r.occurrence.sequence = 0;
        },
            [](VfxSpawnRequest &r) {
            r.bindingGeneration = 0;
        },
            [](VfxSpawnRequest &r) {
            r.destination = 0;
        },
            [](VfxSpawnRequest &r) {
            r.layerOrdinal = VfxSpawnHardLimits::Layers;
        },
            [](VfxSpawnRequest &r) {
            r.requirement = VfxRequirementClass::Count;
        },
            [](VfxSpawnRequest &r) {
            r.requirement = static_cast<VfxRequirementClass>(255);
        },
        };
        for (const auto mutate : mutations) {
            auto request = ValidRequest();
            mutate(request);
            CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::InvalidRequest);
        }
    }

    TEST_CASE("VFX spawn requests reject nonunit and nonfinite rotations", "[unit][vfx][spawn-request]") {
        const std::array<Math::Quaternion, 4> invalid{
            Math::Quaternion{0.0F, 0.0F, 0.0F, 0.0F},
            Math::Quaternion{0.0F, 0.0F, 0.0F, 2.0F},
            Math::Quaternion{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F},
            Math::Quaternion{0.0F, std::numeric_limits<float>::infinity(), 0.0F, 1.0F},
        };
        for (const auto orientation : invalid) {
            auto request = ValidRequest();
            request.transform.orientation = orientation;
            CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::InvalidTransform);
        }
    }

    TEST_CASE("VFX spawn requests reject collapsed nonfinite and overflowing effective scale", "[unit][vfx][spawn-request]") {
        const std::array<Mutation, 10> mutations{
            [](VfxSpawnRequest &r) {
            r.scale = 0.0F;
        },
            [](VfxSpawnRequest &r) {
            r.scale = -1.0F;
        },
            [](VfxSpawnRequest &r) {
            r.scale = std::numeric_limits<float>::quiet_NaN();
        },
            [](VfxSpawnRequest &r) {
            r.scale = std::numeric_limits<float>::infinity();
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.x = 0.0F;
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.y = -1.0F;
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.z = std::numeric_limits<float>::infinity();
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.x = std::numeric_limits<float>::quiet_NaN();
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.x = 2.0F;
            r.scale = std::numeric_limits<float>::max();
        },
            [](VfxSpawnRequest &r) {
            r.transform.scale.x = std::numeric_limits<float>::min();
            r.scale = std::numeric_limits<float>::min();
        },
        };
        for (const auto mutate : mutations) {
            auto request = ValidRequest();
            mutate(request);
            CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::InvalidTransform);
        }
    }

    TEST_CASE("VFX spawn requests preserve the distinction between absent and malformed impact normals", "[unit][vfx][spawn-request]") {
        const std::array<Math::Vec3, 4> invalid{
            Math::Vec3{0.0F, 0.0F, 0.0F},
            Math::Vec3{2.0F, 0.0F, 0.0F},
            Math::Vec3{0.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F},
            Math::Vec3{0.0F, 0.0F, std::numeric_limits<float>::infinity()},
        };
        for (const auto normal : invalid) {
            auto request = ValidRequest();
            request.impactNormal = normal;
            CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::InvalidImpactNormal);
        }
    }

    TEST_CASE("VFX spawn parameters reject malformed shapes schemas and channel order", "[unit][vfx][spawn-request]") {
        const std::array<Mutation, 7> mutations{
            [](VfxSpawnRequest &r) {
            r.parameters.count = VfxSpawnHardLimits::Parameters + 1;
        },
            [](VfxSpawnRequest &r) {
            r.parameters.count = 0;
        },
            [](VfxSpawnRequest &r) {
            r.parameters.schema = 0;
        },
            [](VfxSpawnRequest &r) {
            r.parameters.values[0].value = std::numeric_limits<float>::quiet_NaN();
        },
            [](VfxSpawnRequest &r) {
            r.parameters.values[1].value = std::numeric_limits<float>::infinity();
        },
            [](VfxSpawnRequest &r) {
            r.parameters.values[1].channel = 2;
        },
            [](VfxSpawnRequest &r) {
            r.parameters.values[1].channel = 1;
        },
        };
        CHECK(ValidateVfxSpawnRequest(ParameterizedRequest()) == VfxSpawnStatus::Accepted);
        for (const auto mutate : mutations) {
            auto request = ParameterizedRequest();
            mutate(request);
            CHECK(ValidateVfxSpawnRequest(request) == VfxSpawnStatus::InvalidParameters);
        }
    }

    TEST_CASE("VFX parameter views never expose a malformed active prefix", "[unit][vfx][spawn-request]") {
        VfxParameterBlock block;
        CHECK(block.Values().empty());
        block.count = VfxSpawnHardLimits::Parameters;
        CHECK(block.Values().size() == VfxSpawnHardLimits::Parameters);
        CHECK(block.Values().data() == block.values.data());
        block.count = VfxSpawnHardLimits::Parameters + 1;
        CHECK(block.Values().empty());
    }

    TEST_CASE("VFX spawn outcomes retain copied correlation for explicit failure and completion", "[unit][vfx][spawn-request]") {
        auto request = ValidRequest();
        const EffectSystemId instance{request.scene, 4, 5};
        const auto outcome = MakeVfxSpawnOutcome(request, VfxSpawnStatus::Spawned, instance);
        CHECK(outcome.status == VfxSpawnStatus::Spawned);
        CHECK(outcome.instance == instance);
        CHECK(outcome.occurrence == request.occurrence);
        CHECK(outcome.bindingGeneration == request.bindingGeneration);
        CHECK(outcome.destination == request.destination);
        CHECK(outcome.layerOrdinal == request.layerOrdinal);
        CHECK(outcome.effectAsset == request.effectAsset);

        const auto rejected = MakeVfxSpawnOutcome(request, VfxSpawnStatus::ResourceBudgetExceeded);
        CHECK(rejected.status == VfxSpawnStatus::ResourceBudgetExceeded);
        CHECK_FALSE(rejected.instance.IsValid());
        request.occurrence.sequence = 100;
        request.effectAsset = {};
        CHECK(outcome.occurrence.sequence == 9);
        CHECK(outcome.effectAsset.IsValid());
        CHECK(rejected.occurrence == outcome.occurrence);
        CHECK(rejected.effectAsset == outcome.effectAsset);
    }

    TEST_CASE("VFX request validation and outcome construction allocate no C++ storage", "[unit][vfx][spawn-request][allocation]") {
        const auto request = ParameterizedRequest();
        auto invalid = request;
        invalid.parameters.count = VfxSpawnHardLimits::Parameters + 1;
        VfxSpawnStatus validStatus{};
        VfxSpawnStatus invalidStatus{};
        VfxSpawnOutcome outcome;
        Tests::AllocationProbe::Measurement allocations;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            validStatus = ValidateVfxSpawnRequest(request);
            invalidStatus = ValidateVfxSpawnRequest(invalid);
            outcome = MakeVfxSpawnOutcome(request, VfxSpawnStatus::Accepted);
            allocations = measurement.Snapshot();
        }
        CHECK(validStatus == VfxSpawnStatus::Accepted);
        CHECK(invalidStatus == VfxSpawnStatus::InvalidParameters);
        CHECK(outcome.occurrence == request.occurrence);
        CHECK(allocations.requests == 0);
    }
}  // namespace Horo::Vfx
