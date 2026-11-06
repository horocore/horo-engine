#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "Horo/Runtime/Render/VfxGpuSimulation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo::Render;

    /** @brief Declares a bounded scalar observation; it grants neither capability nor native resources. */
    VfxGpuReadbackDescriptor Enabled() {
        return {.enabled = true,
                .maximumRecords = 4,
                .maximumBytesPerResult = 4 * VfxGpuObservationWords::Bytes,
                .maximumPendingResults = 2};
    }
}  // namespace

TEST_CASE("Absent GPU observation policy remains valid and disabled without a particle allocation", "[renderer][vfx][readback-policy]") {
    constexpr VfxGpuReadbackDescriptor disabled;
    static_assert(disabled == VfxGpuReadbackDescriptor{});
    CHECK(IsValidVfxGpuReadbackPolicy(disabled, 0));
    CHECK(IsValidVfxGpuReadbackPolicy(disabled, 1, 16));
    CHECK_FALSE(disabled.enabled);
}

TEST_CASE("Shared observation validation admits exact record byte and pending boundaries for every declared purpose",
          "[renderer][vfx][readback-policy]") {
    auto policy = Enabled();
    policy.purpose = GENERATE(VfxGpuReadbackPurpose::AuthoringPreview, VfxGpuReadbackPurpose::DiagnosticsCapture,
                              VfxGpuReadbackPurpose::CosmeticObservation);
    policy.requirement = GENERATE(VfxGpuReadbackRequirement::Optional, VfxGpuReadbackRequirement::RequiredForVisualEffect);
    policy.maximumPendingResults = GENERATE(1U, 64U);
    CHECK(IsValidVfxGpuReadbackPolicy(policy, 4));
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, 3));
}

TEST_CASE("Disabled observation declarations cannot hide unknown schema enum cadence or nonzero storage",
          "[renderer][vfx][readback-policy]") {
    VfxGpuReadbackDescriptor policy;
    SECTION("unknown schema") {
        policy.schemaVersion = 2;
    }
    SECTION("unknown purpose") {
        policy.purpose = static_cast<VfxGpuReadbackPurpose>(255);
    }
    SECTION("unknown requirement") {
        policy.requirement = static_cast<VfxGpuReadbackRequirement>(255);
    }
    SECTION("zero cadence") {
        policy.cadenceSteps = 0;
    }
    SECTION("undeclared records") {
        policy.maximumRecords = 1;
    }
    SECTION("undeclared bytes") {
        policy.maximumBytesPerResult = 1;
    }
    SECTION("undeclared queue") {
        policy.maximumPendingResults = 1;
    }
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, 4));
}

TEST_CASE("Enabled observations reject undersized storage unknown metadata custom streams and unbounded queues",
          "[renderer][vfx][readback-policy]") {
    auto policy = Enabled();
    SECTION("no records") {
        policy.maximumRecords = 0;
    }
    SECTION("undersized result") {
        --policy.maximumBytesPerResult;
    }
    SECTION("no pending capacity") {
        policy.maximumPendingResults = 0;
    }
    SECTION("pending capacity ceiling") {
        policy.maximumPendingResults = 65;
    }
    SECTION("unknown schema") {
        policy.schemaVersion = 0;
    }
    SECTION("unknown purpose") {
        policy.purpose = static_cast<VfxGpuReadbackPurpose>(255);
    }
    SECTION("unknown requirement") {
        policy.requirement = static_cast<VfxGpuReadbackRequirement>(255);
    }
    SECTION("zero cadence") {
        policy.cadenceSteps = 0;
    }
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, 4));
}

TEST_CASE("Readback schema one rejects custom streams without silently changing the requested observation",
          "[renderer][vfx][readback-policy]") {
    const auto policy = Enabled();
    CHECK(IsValidVfxGpuReadbackPolicy(policy, 4, 0));
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, 4, 1));
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, 4, 16));
    CHECK(policy == Enabled());
}

TEST_CASE("Observation byte multiplication cannot wrap into an admitted small result", "[renderer][vfx][readback-policy]") {
    auto policy = Enabled();
    policy.maximumRecords = std::numeric_limits<std::uint32_t>::max();
    policy.maximumBytesPerResult = std::numeric_limits<std::uint32_t>::max();
    CHECK_FALSE(IsValidVfxGpuReadbackPolicy(policy, std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("Observation value identity includes every authored field copied into a cook plan", "[renderer][vfx][readback-policy]") {
    const auto policy = Enabled();
    std::array<VfxGpuReadbackDescriptor, 8> changed;
    changed.fill(policy);
    changed[0].enabled = false;
    ++changed[1].schemaVersion;
    changed[2].purpose = VfxGpuReadbackPurpose::AuthoringPreview;
    changed[3].requirement = VfxGpuReadbackRequirement::RequiredForVisualEffect;
    ++changed[4].cadenceSteps;
    ++changed[5].maximumRecords;
    ++changed[6].maximumBytesPerResult;
    ++changed[7].maximumPendingResults;
    for (const auto &candidate : changed)
        CHECK(candidate != policy);
}
