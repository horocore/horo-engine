#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "VulkanBackendTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanBackendInternal.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanBackendTest;

    class HostRuntime final : public FakeRuntimePort {
    public:
        HostRuntime(RuntimeState &state, VulkanHostGraphicsRequirements value) : FakeRuntimePort(state), requirements(std::move(value)) {}

        Result<VulkanHostGraphicsRequirements> RequiredGraphicsRequirements() override {
            if (failure)
                return Result<VulkanHostGraphicsRequirements>::Failure(*failure);
            return Result<VulkanHostGraphicsRequirements>::Success(requirements);
        }

        VulkanHostGraphicsRequirements requirements;
        std::optional<Error> failure;
    };

    TEST_CASE("Vulkan host graphics requirements own exact names and inclusive native API bounds", "[vulkan][host]") {
        std::vector<std::string> instance{"VK_KHR_external_memory_capabilities"};
        std::vector<std::string> device{"VK_KHR_external_memory_fd"};
        auto admitted = VulkanHostGraphicsRequirements::Create(instance, device, VulkanHostApiRange{{1, 2, 0}, {1, 3, 0}});
        REQUIRE(admitted.HasValue());
        instance.front() = "VK_changed";
        device.clear();
        CHECK(admitted.Value().Instance().front() == "VK_KHR_external_memory_capabilities");
        CHECK(admitted.Value().Device().front() == "VK_KHR_external_memory_fd");
        CHECK(admitted.Value().Allows({1, 2, 0}));
        CHECK(admitted.Value().Allows({1, 3, 0}));
        CHECK_FALSE(admitted.Value().Allows({1, 3, 1}));
        CHECK_FALSE(admitted.Value().Allows({1, 1, 999}));
    }

    TEST_CASE("Vulkan host graphics requirements reject malformed duplicate oversized and unencodable facts", "[vulkan][host]") {
        std::vector<std::string> names{"VK_KHR_external_memory_fd", "VK_KHR_external_memory_fd"};
        CHECK(VulkanHostGraphicsRequirements::Create(names, {}).HasError());
        names = {std::string{"VK_name\0suffix", 14}};
        CHECK(VulkanHostGraphicsRequirements::Create({}, names).HasError());
        names.assign(33, "VK_extension");
        CHECK(VulkanHostGraphicsRequirements::Create(names, {}).HasError());
        CHECK(VulkanHostGraphicsRequirements::Create({}, {}, VulkanHostApiRange{{1, 3, 0}, {1, 2, 0}}).HasError());
        CHECK(VulkanHostGraphicsRequirements::Create({}, {}, VulkanHostApiRange{{1, 0, 0}, {128, 0, 0}}).HasError());
    }

    TEST_CASE("Vulkan initialization preserves host preflight failure before creating any native instance", "[vulkan][host]") {
        auto requirements = VulkanHostGraphicsRequirements::Create({}, {});
        REQUIRE(requirements.HasValue());
        auto state = DefaultState();
        HostRuntime port{state, std::move(requirements).Value()};
        port.failure = Error{ErrorCode{"xr.test.preflight_failure"}, ErrorDomainId{"xr.test"}};
        auto backend = CreateBackend(port);
        const auto initialized = backend->Initialize({});
        REQUIRE(initialized.HasError());
        CHECK(initialized.ErrorValue().code.Value() == "xr.test.preflight_failure");
        CHECK(initialized.ErrorValue().domain.Value() == "xr.test");
        CHECK(state.createInstanceCount == 0);
        CHECK(state.createDeviceCount == 0);
        CHECK(state.releaseLoaderCount == 1);
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Vulkan initialization rejects runtime API bounds before native creation without reducing baseline", "[vulkan][host]") {
        auto requirements = VulkanHostGraphicsRequirements::Create({}, {}, VulkanHostApiRange{{1, 0, 0}, {1, 2, 0}});
        REQUIRE(requirements.HasValue());
        auto state = DefaultState();
        HostRuntime port{state, std::move(requirements).Value()};
        auto backend = CreateBackend(port);
        const auto initialized = backend->Initialize({});
        REQUIRE(initialized.HasError());
        CHECK(initialized.ErrorValue().code.Value() == VulkanBackendErrors::LoaderVersionUnsupported.code.Value());
        CHECK(state.createInstanceCount == 0);
        CHECK(state.createDeviceCount == 0);
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Vulkan initialization enables exact supported host extensions before the selected device", "[vulkan][host]") {
        const std::vector<std::string> instance{"VK_KHR_external_memory_capabilities"};
        const std::vector<std::string> device{"VK_KHR_external_memory_fd"};
        auto requirements = VulkanHostGraphicsRequirements::Create(instance, device);
        REQUIRE(requirements.HasValue());
        auto state = DefaultState();
        state.loader.instanceExtensions.push_back(instance.front());
        for (auto &candidate : state.enumeration.adapters)
            candidate.deviceExtensions.push_back(device.front());
        HostRuntime port{state, std::move(requirements).Value()};
        auto backend = CreateBackend(port);
        REQUIRE(backend->Initialize({.requirePresentation = true}).HasValue());
        CHECK(std::ranges::find(state.instanceRequest.enabledExtensions, instance.front()) !=
              state.instanceRequest.enabledExtensions.end());
        CHECK(state.deviceRequest.enabledExtensions ==
              std::vector<std::string>{"VK_KHR_swapchain", VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, device.front()});
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Vulkan host device mismatch is independent of admitted driver policy", "[vulkan][host]") {
        auto state = DefaultState();
        for (auto &candidate : state.enumeration.adapters) {
            REQUIRE(candidate.driverAllowed);
            candidate.hostAllowed = false;
        }
        FakeRuntimePort port{state};
        auto backend = CreateBackend(port);
        const auto initialized = backend->Initialize({});
        REQUIRE(initialized.HasError());
        CHECK(initialized.ErrorValue().code.Value() == VulkanBackendErrors::AdapterRequirementsUnsupported.code.Value());
        CHECK(state.createDeviceCount == 0);
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }
}  // namespace
