#include "VulkanHostGraphicsRequirements.h"

#include "VulkanBackendInternal.h"
#include "VulkanBackendModule.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>

namespace Horo::Render {
    namespace {
        /** @brief Validates exact bounded native identifiers before any allocation or loader callback. */
        bool ValidNames(const std::span<const std::string> names) {
            if (names.size() > 32)
                return false;
            for (std::size_t index = 0; index < names.size(); ++index) {
                const auto &name = names[index];
                if (!name.starts_with("VK_") || name.size() <= 3 || name.size() >= VK_MAX_EXTENSION_NAME_SIZE ||
                    !std::ranges::all_of(name,
                                         [](const unsigned char character) {
                    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_';
                }) ||
                    std::ranges::find(names.first(index), name) != names.first(index).end())
                    return false;
            }
            return true;
        }

        /** @brief Checks the real standard-variant VkVersion packing bounds before native encoding. */
        bool ValidVersion(const VulkanApiVersion version) {
            return version.major > 0 && version.major <= 127 && version.minor <= 1023 && version.patch <= 4095;
        }
    }  // namespace

    /** @copydoc VulkanHostGraphicsRequirements::Create */
    Result<VulkanHostGraphicsRequirements> VulkanHostGraphicsRequirements::Create(const std::span<const std::string> instance,
                                                                                  const std::span<const std::string> device,
                                                                                  const std::optional<VulkanHostApiRange> api) {
        if (!ValidNames(instance) || !ValidNames(device) ||
            (api && (!ValidVersion(api->minimum) || !ValidVersion(api->maximum) || api->minimum > api->maximum)))
            return Result<VulkanHostGraphicsRequirements>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        return Result<VulkanHostGraphicsRequirements>::Success(
            VulkanHostGraphicsRequirements{{instance.begin(), instance.end()}, {device.begin(), device.end()}, api});
    }

    /** @copydoc VulkanHostGraphicsRequirements::VulkanHostGraphicsRequirements */
    VulkanHostGraphicsRequirements::VulkanHostGraphicsRequirements(std::vector<std::string> instance, std::vector<std::string> device,
                                                                   const std::optional<VulkanHostApiRange> api)
        : instance_(std::move(instance)), device_(std::move(device)), api_(api) {}

    /** @copydoc VulkanHostGraphicsRequirements::Instance */
    std::span<const std::string> VulkanHostGraphicsRequirements::Instance() const noexcept {
        return instance_;
    }

    /** @copydoc VulkanHostGraphicsRequirements::Device */
    std::span<const std::string> VulkanHostGraphicsRequirements::Device() const noexcept {
        return device_;
    }

    /** @copydoc VulkanHostGraphicsRequirements::Allows */
    bool VulkanHostGraphicsRequirements::Allows(const VulkanApiVersion api) const noexcept {
        return ValidVersion(api) && (!api_ || (api >= api_->minimum && api <= api_->maximum));
    }

    /** @copydoc IVulkanLoaderPort::RequiredGraphicsRequirements */
    Result<VulkanHostGraphicsRequirements> IVulkanLoaderPort::RequiredGraphicsRequirements() {
        return VulkanHostGraphicsRequirements::Create({}, {});
    }

    /** @copydoc IVulkanLoaderPort::AcceptPhysicalDevice */
    Result<bool> IVulkanLoaderPort::AcceptPhysicalDevice(VkInstance, VkPhysicalDevice) {
        return Result<bool>::Success(true);
    }

    /** @copydoc IVulkanRuntimePort::RequiredGraphicsRequirements */
    Result<VulkanHostGraphicsRequirements> IVulkanRuntimePort::RequiredGraphicsRequirements() {
        return VulkanHostGraphicsRequirements::Create({}, {});
    }

    namespace Detail {
        /** @copydoc ResolveHostInstanceExtensions */
        Result<std::vector<std::string>> ResolveHostInstanceExtensions(std::vector<std::string> required,
                                                                       const VulkanLoaderCapabilities &loader,
                                                                       const VulkanHostGraphicsRequirements &host) {
            if (!host.Allows(RequiredVulkanApiVersion))
                return Result<std::vector<std::string>>::Failure(MakeError(VulkanBackendErrors::LoaderVersionUnsupported));
            required.insert(required.end(), host.Instance().begin(), host.Instance().end());
            std::ranges::sort(required);
            required.erase(std::ranges::unique(required).begin(), required.end());
            for (const auto &name : required)
                if (!VulkanContains(loader.instanceExtensions, name))
                    return Result<std::vector<std::string>>::Failure(
                        MakeError(VulkanBackendErrors::InstanceExtensionMissing, "Required Vulkan instance extension is missing: " + name));
            return Result<std::vector<std::string>>::Success(std::move(required));
        }

        /** @copydoc ResolveHostDeviceExtensions */
        Result<std::vector<std::string>> ResolveHostDeviceExtensions(std::vector<std::string> required,
                                                                     const VulkanAdapterCandidate &candidate,
                                                                     const VulkanHostGraphicsRequirements &host) {
            for (const auto &name : host.Device())
                if (std::ranges::find(required, name) == required.end())
                    required.push_back(name);
            for (const auto &name : required)
                if (!VulkanContains(candidate.deviceExtensions, name))
                    return Result<std::vector<std::string>>::Failure(
                        MakeError(VulkanBackendErrors::DeviceExtensionMissing, "Required Vulkan device extension is missing: " + name));
            return Result<std::vector<std::string>>::Success(std::move(required));
        }
    }  // namespace Detail
}  // namespace Horo::Render
