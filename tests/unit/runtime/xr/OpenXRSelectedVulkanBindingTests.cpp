#include "OpenXRSelectedVulkanTestSupport.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace {
    using namespace Horo;
    using namespace Horo::XR::OpenXRInternal;

    using Horo::Tests::OpenXR::GraphicsScript;

    TEST_CASE("XR Vulkan preflight retains exact native API interval and owned extensions", "[xr][vulkan][host]") {
        GraphicsScript script;
        OpenXRSelectedVulkanBinding binding;
        CHECK(binding.RequiredGraphicsRequirements().HasError());
        REQUIRE(binding.Preflight(script.Borrow()).HasValue());
        script.instanceExtensions = "VK_changed_after_preflight";
        const auto requirements = binding.RequiredGraphicsRequirements();
        REQUIRE(requirements.HasValue());
        CHECK(requirements.Value().Allows({1, 3, 0}));
        CHECK_FALSE(requirements.Value().Allows({1, 1, 0}));
        CHECK_FALSE(requirements.Value().Allows({1, 3, 1}));
        REQUIRE(requirements.Value().Instance().size() == 1);
        CHECK(requirements.Value().Instance()[0] == "VK_KHR_get_physical_device_properties2");
        CHECK(binding.Preflight(script.Borrow()).HasError());
    }

    TEST_CASE("XR Vulkan physical admission queries the actual same backend instance and preserves errors", "[xr][vulkan][host]") {
        GraphicsScript script;
        OpenXRSelectedVulkanBinding binding;
        REQUIRE(binding.Preflight(script.Borrow()).HasValue());
        const auto instance = std::bit_cast<VkInstance>(std::uintptr_t{99});
        const auto accepted = binding.AcceptPhysicalDevice(instance, script.selected);
        REQUIRE(accepted.HasValue());
        CHECK(accepted.Value());
        CHECK(script.queriedInstance == instance);
        const auto rejected = binding.AcceptPhysicalDevice(instance, std::bit_cast<VkPhysicalDevice>(std::uintptr_t{8}));
        REQUIRE(rejected.HasValue());
        CHECK_FALSE(rejected.Value());
        script.failure = XR_ERROR_RUNTIME_FAILURE;
        const auto failed = binding.AcceptPhysicalDevice(instance, script.selected);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().message.find("xrGetVulkanGraphicsDeviceKHR") != std::string::npos);
    }

    TEST_CASE("XR Vulkan failed native preflight never admits empty replacement requirements", "[xr][vulkan][host]") {
        GraphicsScript script;
        OpenXRSelectedVulkanBinding binding;
        SECTION("native query") {
            script.failure = XR_ERROR_RUNTIME_FAILURE;
        }
        SECTION("duplicate extension") {
            script.deviceExtensions = "VK_KHR_external_memory VK_KHR_external_memory";
        }
        SECTION("oversized extension list") {
            script.instanceExtensions.assign(4096, 'x');
        }
        REQUIRE(binding.Preflight(script.Borrow()).HasError());
        CHECK(binding.RequiredGraphicsRequirements().HasError());
        script.failure = XR_SUCCESS;
        script.instanceExtensions = "VK_KHR_get_physical_device_properties2";
        script.deviceExtensions = "VK_KHR_external_memory";
        REQUIRE(binding.Preflight(script.Borrow()).HasValue());
    }
}  // namespace
