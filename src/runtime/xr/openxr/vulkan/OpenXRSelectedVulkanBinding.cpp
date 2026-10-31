#include "host/OpenXRSelectedVulkanBinding.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        Error NativeFailure(const char *operation, XrResult result) {
            return MakeError(XRErrors::OperationUnavailable,
                             std::format("{} failed; native result={}", operation, static_cast<std::int32_t>(result)));
        }

        template <typename Function> Result<Function> Resolve(const NativeDiscoveryBorrow &owner, const char *name) {
            PFN_xrVoidFunction function{};
            const auto result = owner.getProc(owner.instance, name, &function);
            if (XR_FAILED(result))
                return Result<Function>::Failure(NativeFailure(name, result));
            if (!function)
                return Result<Function>::Failure(MakeError(XRErrors::LoaderIncompatible, name));
            return Result<Function>::Success(std::bit_cast<Function>(function));
        }

        /** @brief Parses the complete bounded native space-separated publication; no truncation/empty token admission. */
        template <typename Function>
        Result<std::vector<std::string>> Extensions(Function query, const NativeDiscoveryBorrow &owner, const char *name) {
            using Outcome = Result<std::vector<std::string>>;
            std::array<char, 4096> storage{};
            std::uint32_t count{};
            auto result = query(owner.instance, owner.system, 0, &count, nullptr);
            if (XR_FAILED(result))
                return Outcome::Failure(NativeFailure(name, result));
            if (!count || count > storage.size())
                return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
            const auto expected = count;
            result = query(owner.instance, owner.system, static_cast<std::uint32_t>(storage.size()), &count, storage.data());
            if (XR_FAILED(result))
                return Outcome::Failure(NativeFailure(name, result));
            if (count != expected || storage[count - 1] != '\0' ||
                std::find(storage.begin(), storage.begin() + count - 1, '\0') != storage.begin() + count - 1)
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Changed or malformed Vulkan extension publication"));
            std::vector<std::string> names;
            const std::string_view text{storage.data(), count - 1};
            for (std::size_t begin = 0; begin < text.size();) {
                const auto end = text.find(' ', begin);
                const auto length = (end == std::string_view::npos ? text.size() : end) - begin;
                if (!length || names.size() == 32)
                    return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
                names.emplace_back(text.substr(begin, length));
                begin += length;
                if (begin < text.size())
                    ++begin;
            }
            return Outcome::Success(std::move(names));
        }

        Render::VulkanApiVersion Version(XrVersion version) {
            return {static_cast<std::uint32_t>(XR_VERSION_MAJOR(version)), static_cast<std::uint32_t>(XR_VERSION_MINOR(version)),
                    static_cast<std::uint32_t>(XR_VERSION_PATCH(version))};
        }

        template <typename Range> bool ContainsAll(Range enabled, std::span<const std::string> required) {
            return std::ranges::all_of(required, [&](const auto &name) {
                return std::ranges::find(enabled, name) != enabled.end();
            });
        }
    }  // namespace

    /** @copydoc OpenXRSelectedVulkanBinding::CheckLane */
    Result<void> OpenXRSelectedVulkanBinding::CheckLane() const {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "XR graphics operation on foreign owner lane"));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::BorrowBinding */
    Result<const XrGraphicsBindingVulkanKHR *> OpenXRSelectedVulkanBinding::BorrowBinding(const XRSessionId &candidate) const {
        if (auto current = Validate(candidate); current.HasError())
            return Result<const XrGraphicsBindingVulkanKHR *>::Failure(current.ErrorValue());
        if (!bound_)
            return Result<const XrGraphicsBindingVulkanKHR *>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<const XrGraphicsBindingVulkanKHR *>::Success(&binding_);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::Preflight */
    Result<void> OpenXRSelectedVulkanBinding::Preflight(const NativeDiscoveryBorrow &owner) {
        if (const auto lane = CheckLane(); lane.HasError())
            return lane;
        if (requirements_ || !owner.instance || !owner.system || !owner.getProc ||
            std::ranges::none_of(owner.enabledExtensions, [](const char *name) {
            return std::string_view{name} == XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
        }))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        auto query = Resolve<PFN_xrGetVulkanGraphicsRequirementsKHR>(owner, "xrGetVulkanGraphicsRequirementsKHR");
        if (query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        XrGraphicsRequirementsVulkanKHR api{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        const auto result = query.Value()(owner.instance, owner.system, &api);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("xrGetVulkanGraphicsRequirementsKHR", result));
        auto instanceQuery = Resolve<PFN_xrGetVulkanInstanceExtensionsKHR>(owner, "xrGetVulkanInstanceExtensionsKHR");
        if (instanceQuery.HasError())
            return Result<void>::Failure(instanceQuery.ErrorValue());
        auto deviceQuery = Resolve<PFN_xrGetVulkanDeviceExtensionsKHR>(owner, "xrGetVulkanDeviceExtensionsKHR");
        if (deviceQuery.HasError())
            return Result<void>::Failure(deviceQuery.ErrorValue());
        auto instanceNames = Extensions(instanceQuery.Value(), owner, "xrGetVulkanInstanceExtensionsKHR");
        if (instanceNames.HasError())
            return Result<void>::Failure(instanceNames.ErrorValue());
        auto deviceNames = Extensions(deviceQuery.Value(), owner, "xrGetVulkanDeviceExtensionsKHR");
        if (deviceNames.HasError())
            return Result<void>::Failure(deviceNames.ErrorValue());
        auto select = Resolve<PFN_xrGetVulkanGraphicsDeviceKHR>(owner, "xrGetVulkanGraphicsDeviceKHR");
        if (select.HasError())
            return Result<void>::Failure(select.ErrorValue());
        auto requirements = Render::VulkanHostGraphicsRequirements::Create(instanceNames.Value(), deviceNames.Value(),
                                                                           Render::VulkanHostApiRange{Version(api.minApiVersionSupported),
                                                                                                      Version(api.maxApiVersionSupported)});
        if (requirements.HasError())
            return Result<void>::Failure(requirements.ErrorValue());
        requirements_.emplace(std::move(requirements).Value());
        instance_ = owner.instance;
        system_ = owner.system;
        dispatch_ = owner.getProc;
        selectDevice_ = select.Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::Attach */
    Result<void> OpenXRSelectedVulkanBinding::Attach(Render::IVulkanLoaderPort &platform,
                                                     Render::Detail::VulkanEditorGraphicsBridge &graphics) {
        if (const auto lane = CheckLane(); lane.HasError())
            return lane;
        if (!requirements_ || platform_ || &platform == this)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        platform_ = &platform;
        graphics_ = &graphics;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::RequiredExtension */
    const char *OpenXRSelectedVulkanBinding::RequiredExtension() const noexcept {
        return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    }

    /** @copydoc OpenXRSelectedVulkanBinding::ValidatePreparation */
    Result<void> OpenXRSelectedVulkanBinding::ValidatePreparation() const {
        if (const auto lane = CheckLane(); lane.HasError())
            return lane;
        if (!requirements_ || !graphics_ || !graphics_->Port())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        auto current = graphics_->Port()->QueryDevice();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (bound_ && (current.Value().instance != binding_.instance || current.Value().physical != binding_.physicalDevice ||
                       current.Value().device != binding_.device || current.Value().graphicsFamily != binding_.queueFamilyIndex))
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::Validate */
    Result<void> OpenXRSelectedVulkanBinding::Validate(const XRSessionId &candidate) const {
        if (!candidate.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        return ValidatePreparation();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::Prepare */
    Result<const XrBaseInStructure *> OpenXRSelectedVulkanBinding::Prepare(XrInstance instance, XrSystemId system,
                                                                           NativeOpenXRDispatch dispatch) {
        using Outcome = Result<const XrBaseInStructure *>;
        if (const auto lane = CheckLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (bound_ || instance != instance_ || system != system_ || dispatch.getProc != dispatch_ || !graphics_ || !graphics_->Port())
            return Outcome::Failure(MakeError(XRErrors::CapabilityStale));
        auto current = graphics_->Port()->QueryDevice();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto &device = current.Value();
        auto selected = AcceptPhysicalDevice(device.instance, device.physical);
        if (selected.HasError())
            return Outcome::Failure(selected.ErrorValue());
        if (!selected.Value() || !device.device || !device.graphicsQueue || !requirements_->Allows(device.enabled.api) ||
            !ContainsAll(device.enabled.instanceExtensions, requirements_->Instance()) ||
            !ContainsAll(device.enabled.deviceExtensions, requirements_->Device()))
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        binding_ =
            {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, device.instance, device.physical, device.device, device.graphicsFamily, 0};
        bound_ = true;
        return Outcome::Success(std::bit_cast<const XrBaseInStructure *>(&binding_));
    }

    /** @copydoc OpenXRSelectedVulkanBinding::Release */
    void OpenXRSelectedVulkanBinding::Release() noexcept {
        if (std::this_thread::get_id() != lane_)
            return;
        binding_ = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        bound_ = false;
    }

    /** @copydoc OpenXRSelectedVulkanBinding::AcquireLoader */
    Result<void> OpenXRSelectedVulkanBinding::AcquireLoader() {
        if (const auto lane = CheckLane(); lane.HasError())
            return lane;
        if (!platform_ || !requirements_ || acquired_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        auto result = platform_->AcquireLoader();
        if (result.HasValue())
            acquired_ = true;
        return result;
    }

    /** @copydoc OpenXRSelectedVulkanBinding::GetInstanceProcAddr */
    PFN_vkGetInstanceProcAddr OpenXRSelectedVulkanBinding::GetInstanceProcAddr() noexcept {
        return std::this_thread::get_id() == lane_ && acquired_ ? platform_->GetInstanceProcAddr() : nullptr;
    }

    /** @copydoc OpenXRSelectedVulkanBinding::RequiredPresentationExtensions */
    Result<std::vector<std::string>> OpenXRSelectedVulkanBinding::RequiredPresentationExtensions() {
        if (const auto lane = CheckLane(); lane.HasError())
            return Result<std::vector<std::string>>::Failure(lane.ErrorValue());
        if (!acquired_)
            return Result<std::vector<std::string>>::Failure(MakeError(XRErrors::OperationUnavailable));
        return platform_->RequiredPresentationExtensions();
    }

    /** @copydoc OpenXRSelectedVulkanBinding::RequiredGraphicsRequirements */
    Result<Render::VulkanHostGraphicsRequirements> OpenXRSelectedVulkanBinding::RequiredGraphicsRequirements() {
        using Outcome = Result<Render::VulkanHostGraphicsRequirements>;
        if (const auto lane = CheckLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (!requirements_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        return Outcome::Success(*requirements_);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::AcceptPhysicalDevice */
    Result<bool> OpenXRSelectedVulkanBinding::AcceptPhysicalDevice(VkInstance instance, VkPhysicalDevice physical) {
        if (const auto lane = CheckLane(); lane.HasError())
            return Result<bool>::Failure(lane.ErrorValue());
        if (!requirements_ || !instance || !physical)
            return Result<bool>::Failure(MakeError(XRErrors::OperationUnavailable));
        VkPhysicalDevice selected{};
        const auto result = selectDevice_(instance_, system_, instance, &selected);
        if (XR_FAILED(result))
            return Result<bool>::Failure(NativeFailure("xrGetVulkanGraphicsDeviceKHR", result));
        if (!selected)
            return Result<bool>::Failure(MakeError(XRErrors::OperationInvalid));
        return Result<bool>::Success(selected == physical);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::CreatePresentationSurface */
    Result<VkSurfaceKHR> OpenXRSelectedVulkanBinding::CreatePresentationSurface(VkInstance instance) {
        if (const auto lane = CheckLane(); lane.HasError())
            return Result<VkSurfaceKHR>::Failure(lane.ErrorValue());
        if (!acquired_)
            return Result<VkSurfaceKHR>::Failure(MakeError(XRErrors::OperationUnavailable));
        return platform_->CreatePresentationSurface(instance);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::DestroyPresentationSurface */
    void OpenXRSelectedVulkanBinding::DestroyPresentationSurface(VkInstance instance, VkSurfaceKHR surface) noexcept {
        if (std::this_thread::get_id() == lane_ && acquired_)
            platform_->DestroyPresentationSurface(instance, surface);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::SupportsPresentation */
    Result<bool> OpenXRSelectedVulkanBinding::SupportsPresentation(VkPhysicalDevice device, std::uint32_t family) {
        if (const auto lane = CheckLane(); lane.HasError())
            return Result<bool>::Failure(lane.ErrorValue());
        if (!acquired_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationUnavailable));
        return platform_->SupportsPresentation(device, family);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::IsDriverAllowed */
    Result<bool> OpenXRSelectedVulkanBinding::IsDriverAllowed(const VkPhysicalDeviceProperties &properties) {
        if (const auto lane = CheckLane(); lane.HasError())
            return Result<bool>::Failure(lane.ErrorValue());
        if (!acquired_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationUnavailable));
        return platform_->IsDriverAllowed(properties);
    }

    /** @copydoc OpenXRSelectedVulkanBinding::ReleaseLoader */
    void OpenXRSelectedVulkanBinding::ReleaseLoader() noexcept {
        if (std::this_thread::get_id() != lane_ || !acquired_)
            return;
        // SYNC: the sole runtime calls this only after its surfaces/device children retire; XR owner closes its session first.
        platform_->ReleaseLoader();
        acquired_ = false;
    }
}  // namespace Horo::XR::OpenXRInternal
