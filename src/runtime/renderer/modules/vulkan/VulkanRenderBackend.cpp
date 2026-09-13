#include "VulkanBackendInternal.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::string_view SurfaceExtension{"VK_KHR_surface"};
        constexpr std::string_view SwapchainExtension{"VK_KHR_swapchain"};
        constexpr std::string_view DebugUtilsExtension{"VK_EXT_debug_utils"};
        constexpr std::string_view ValidationLayer{"VK_LAYER_KHRONOS_validation"};

        [[nodiscard]] Error MakeVulkanError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsQualifiedPresentationExtension(const std::string_view extension) noexcept {
#if defined(_WIN32)
            return extension == "VK_KHR_win32_surface";
#elif defined(__linux__)
            return extension == "VK_KHR_xlib_surface" || extension == "VK_KHR_xcb_surface" || extension == "VK_KHR_wayland_surface";
#else
            static_cast<void>(extension);
            return false;
#endif
        }

        [[nodiscard]] bool HasGraphicsQueue(const VulkanAdapterCandidate &candidate) noexcept {
            return std::ranges::any_of(candidate.queueFamilies, &VulkanQueueFamily::supportsGraphics);
        }

        [[nodiscard]] bool HasPresentationQueue(const VulkanAdapterCandidate &candidate) noexcept {
            return std::ranges::any_of(candidate.queueFamilies, &VulkanQueueFamily::supportsPresentation);
        }

        [[nodiscard]] bool IsCandidateDataValid(const VulkanAdapterCandidate &candidate) {
            if (!candidate.id.IsValid() || candidate.displayName.empty() || candidate.displayName.size() > 256 ||
                candidate.queueFamilies.size() > 256) {
                return false;
            }
            std::set<std::uint32_t, std::less<>> queueIndices;
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                if (!queueIndices.insert(queue.index).second) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateEnumeration(const VulkanAdapterEnumeration &enumeration) {
            if (enumeration.revision == 0 || enumeration.adapters.size() > 64) {
                return Result<void>::Failure(
                    MakeVulkanError(VulkanBackendErrors::AdapterDataInvalid, "Physical-device enumeration was oversized or unversioned."));
            }
            if (enumeration.adapters.empty()) {
                return Result<void>::Failure(
                    MakeVulkanError(VulkanBackendErrors::AdapterRequirementsUnsupported, "No Vulkan physical device was enumerated."));
            }
            std::set<std::string, std::less<>> identities;
            for (const VulkanAdapterCandidate &candidate : enumeration.adapters) {
                if (!IsCandidateDataValid(candidate) || !identities.insert(candidate.id.Value()).second) {
                    return Result<void>::Failure(
                        MakeVulkanError(VulkanBackendErrors::AdapterDataInvalid, "Physical-device records are malformed or duplicated."));
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsEligible(const VulkanAdapterCandidate &candidate, const bool requirePresentation) {
            return candidate.standardApiVariant && !candidate.portabilitySubset && candidate.driverAllowed &&
                   Detail::VulkanSupportsVersion(candidate.apiVersion) && candidate.requiredFeatures.AreAvailable() &&
                   HasGraphicsQueue(candidate) &&
                   (!requirePresentation ||
                    (Detail::VulkanContains(candidate.deviceExtensions, SwapchainExtension) && HasPresentationQueue(candidate)));
        }

        [[nodiscard]] const ErrorCodeDescriptor &RejectionDescriptor(const VulkanAdapterCandidate &candidate,
                                                                     const bool requirePresentation) {
            if (!candidate.standardApiVariant || candidate.portabilitySubset) {
                return VulkanBackendErrors::ApiVariantUnsupported;
            }
            if (!Detail::VulkanSupportsVersion(candidate.apiVersion)) {
                return VulkanBackendErrors::DeviceVersionUnsupported;
            }
            if (!candidate.requiredFeatures.AreAvailable()) {
                return VulkanBackendErrors::RequiredFeaturesUnavailable;
            }
            if (!candidate.driverAllowed) {
                return VulkanBackendErrors::DriverUnsupported;
            }
            if (!HasGraphicsQueue(candidate)) {
                return VulkanBackendErrors::GraphicsQueueUnavailable;
            }
            if (requirePresentation && !Detail::VulkanContains(candidate.deviceExtensions, SwapchainExtension)) {
                return VulkanBackendErrors::DeviceExtensionMissing;
            }
            return requirePresentation ? VulkanBackendErrors::PresentationUnsupported : VulkanBackendErrors::AdapterRequirementsUnsupported;
        }

        [[nodiscard]] Result<const VulkanAdapterCandidate *> SelectExplicitCandidate(const RenderBackendConfig &config,
                                                                                     const VulkanAdapterEnumeration &enumeration) {
            if (config.adapterDiscoveryRevision != enumeration.revision) {
                return Result<const VulkanAdapterCandidate *>::Failure(
                    MakeVulkanError(VulkanBackendErrors::AdapterRevisionStale, "Explicit adapter discovery revision is stale."));
            }
            const auto match = std::ranges::find_if(enumeration.adapters, [&](const VulkanAdapterCandidate &candidate) {
                return candidate.id == *config.adapter;
            });
            if (match == enumeration.adapters.end()) {
                return Result<const VulkanAdapterCandidate *>::Failure(
                    MakeVulkanError(VulkanBackendErrors::AdapterNotFound, "Requested Vulkan adapter was not enumerated."));
            }
            if (!IsEligible(*match, config.requirePresentation)) {
                return Result<const VulkanAdapterCandidate *>::Failure(
                    MakeVulkanError(RejectionDescriptor(*match, config.requirePresentation),
                                    "Requested Vulkan adapter does not satisfy the Vulkan 1.3 device contract."));
            }
            return Result<const VulkanAdapterCandidate *>::Success(std::to_address(match));
        }

        [[nodiscard]] const VulkanAdapterCandidate *SelectAutomaticCandidate(const RenderBackendConfig &config,
                                                                             const VulkanAdapterEnumeration &enumeration) {
            const VulkanAdapterCandidate *best = nullptr;
            for (const VulkanAdapterCandidate &candidate : enumeration.adapters) {
                if (!IsEligible(candidate, config.requirePresentation)) {
                    continue;
                }
                if (best == nullptr || candidate.id.Value() < best->id.Value()) {
                    best = &candidate;
                }
            }
            return best;
        }

        [[nodiscard]] std::optional<std::uint32_t> FindCommonQueue(const VulkanAdapterCandidate &candidate) {
            std::optional<std::uint32_t> commonIndex;
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                if (queue.supportsGraphics && queue.supportsPresentation && (!commonIndex.has_value() || queue.index < *commonIndex)) {
                    commonIndex = queue.index;
                }
            }
            return commonIndex;
        }

        [[nodiscard]] std::uint32_t FindMinimumQueue(const VulkanAdapterCandidate &candidate, const bool presentation) {
            std::uint32_t selected = std::numeric_limits<std::uint32_t>::max();
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                const bool supported = presentation ? queue.supportsPresentation : queue.supportsGraphics;
                selected = supported ? std::min(selected, queue.index) : selected;
            }
            return selected;
        }

        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> SelectQueues(const VulkanAdapterCandidate &candidate,
                                                                           const bool requirePresentation) {
            if (const std::optional<std::uint32_t> commonIndex = requirePresentation ? FindCommonQueue(candidate) : std::nullopt;
                commonIndex.has_value()) {
                return {*commonIndex, *commonIndex};
            }
            const std::uint32_t graphics = FindMinimumQueue(candidate, false);
            const std::uint32_t presentation = requirePresentation ? FindMinimumQueue(candidate, true) : graphics;
            return {graphics, requirePresentation ? presentation : graphics};
        }

        struct VulkanRuntimeLease {
            bool claimed{false};
        };

        class VulkanRenderBackend final : public IRenderBackend {  // NOSONAR(cpp:S1448)
        public:
            VulkanRenderBackend(IVulkanRuntimePort &runtimePort, std::shared_ptr<VulkanRuntimeLease> lease) noexcept
                : runtimePort_(&runtimePort), resourcePort_(dynamic_cast<IVulkanResourcePort *>(&runtimePort)), lease_(std::move(lease)) {}

            ~VulkanRenderBackend() override {
                Shutdown();
            }

            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (Result<void> prepared = PrepareInitialization(config); prepared.HasError()) {
                    return prepared;
                }
                if (Result<void> instance = InitializeInstance(config); instance.HasError()) {
                    return FailAndRollback(std::move(instance).ErrorValue());
                }
                if (Result<void> device = InitializeDevice(config); device.HasError()) {
                    return FailAndRollback(std::move(device).ErrorValue());
                }
                CommitInitialization(config.requirePresentation);
                return Result<void>::Success();
            }

            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->QueryBufferMemoryCost(descriptor)
                                                                   : Unsupported<RenderMemoryCostPlan>();
            }

            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->QueryTextureMemoryCost(descriptor)
                                                                   : Unsupported<RenderMemoryCostPlan>();
            }

            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                               const RenderMemoryPlacement &placement) override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->CreateBuffer(descriptor, bytes, placement)
                                                                   : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index) override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->CreateMesh(descriptor, vertex, index)
                                                                   : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                                const RenderMemoryPlacement &placement) override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->CreateTexture(descriptor, bytes, placement)
                                                                   : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture) override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->CreateTextureView(descriptor, texture)
                                                                   : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color,
                                                     std::uint64_t depth) override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->CreateRenderTarget(descriptor, color, depth)
                                                                   : Unsupported<std::uint64_t>();
            }

            void DestroyBuffer(std::uint64_t identity) noexcept override {
                if (resourceBackend_ != nullptr)
                    resourceBackend_->DestroyBuffer(identity);
            }

            void DestroyMesh(std::uint64_t identity) noexcept override {
                if (resourceBackend_ != nullptr)
                    resourceBackend_->DestroyMesh(identity);
            }

            void DestroyTexture(std::uint64_t identity) noexcept override {
                if (resourceBackend_ != nullptr)
                    resourceBackend_->DestroyTexture(identity);
            }

            void DestroyTextureView(std::uint64_t identity) noexcept override {
                if (resourceBackend_ != nullptr)
                    resourceBackend_->DestroyTextureView(identity);
            }

            void DestroyRenderTarget(std::uint64_t identity) noexcept override {
                if (resourceBackend_ != nullptr)
                    resourceBackend_->DestroyRenderTarget(identity);
            }

            Result<FrameToken> BeginFrame(const FrameDescriptor &) override {
                return Unsupported<FrameToken>();
            }

            Result<void> Execute(const RenderExecutionPlan &) override {
                return UnsupportedVoid();
            }

            Result<void> Present(FrameToken) override {
                return UnsupportedVoid();
            }

            void AbortFrame(FrameToken) noexcept override {
                // Frame submission is introduced by RND-006.4; no active frame exists yet.
            }

            void AbortActiveFrame() noexcept override {
                // Frame submission is introduced by RND-006.4; no active frame exists yet.
            }

            Result<void> Resize(FramebufferExtent) override {
                return UnsupportedVoid();
            }

            void Shutdown() noexcept override {
                initialized_ = false;
                capabilities_.presentsToWindow = false;
                Rollback();
            }

        private:
            void CommitInitialization(const bool presentsToWindow) noexcept {
                capabilities_.presentsToWindow = presentsToWindow;
                resourceBackend_ = resourcePort_ != nullptr ? resourcePort_->ResourceBackend() : nullptr;
                initialized_ = true;
            }

            [[nodiscard]] Result<void> PrepareInitialization(const RenderBackendConfig &config) {
                if (initialized_ || loaderAcquired_) {
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::AlreadyInitialized, {}));
                }
                if (!config.IsValid()) {
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::InvalidConfig, {}));
                }
                if (lease_->claimed) {
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::PresentationInUse, {}));
                }
                lease_->claimed = true;
                ownsLease_ = true;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> InitializeInstance(const RenderBackendConfig &config) {
                // Mark ownership before crossing each native boundary. A throwing
                // port may have retained partial state; release calls are idempotent.
                loaderAcquired_ = true;
                if (Result<void> acquired = runtimePort_->AcquireLoader(); acquired.HasError()) {
                    return acquired;
                }
                auto loader = runtimePort_->QueryLoaderCapabilities();
                if (loader.HasError()) {
                    return Result<void>::Failure(std::move(loader).ErrorValue());
                }
                if (!Detail::VulkanLoaderMeetsBaseline(loader.Value())) {
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::LoaderVersionUnsupported,
                                                                 "Vulkan 1.3 and vkEnumerateInstanceVersion are required."));
                }
                if (!loader.Value().standardApiVariant) {
                    return Result<void>::Failure(
                        MakeVulkanError(VulkanBackendErrors::ApiVariantUnsupported, "Only the standard Vulkan API variant is supported."));
                }
                auto extensions = ResolveInstanceExtensions(config, loader.Value());
                if (extensions.HasError()) {
                    return Result<void>::Failure(std::move(extensions).ErrorValue());
                }
                instanceCreated_ = true;
                return runtimePort_->CreateInstance(VulkanInstanceRequest{
                    .apiVersion = Detail::RequiredVulkanApiVersion,
                    .enabledExtensions = std::move(extensions).Value(),
                    .enableValidation = config.enableValidation,
                });
            }

            [[nodiscard]] Result<void> InitializeDevice(const RenderBackendConfig &config) {
                auto enumeration = runtimePort_->EnumeratePhysicalDevices(64);
                if (enumeration.HasError()) {
                    return Result<void>::Failure(std::move(enumeration).ErrorValue());
                }
                auto selected = SelectCandidate(config, enumeration.Value());
                if (selected.HasError()) {
                    return Result<void>::Failure(std::move(selected).ErrorValue());
                }
                const VulkanAdapterCandidate *candidate = selected.Value();
                const auto [graphicsQueue, presentationQueue] = SelectQueues(*candidate, config.requirePresentation);
                std::vector<std::string> deviceExtensions;
                if (config.requirePresentation) {
                    deviceExtensions.emplace_back(SwapchainExtension);
                }
                deviceCreated_ = true;
                return runtimePort_->CreateDevice(VulkanDeviceRequest{
                    .adapter = candidate->id,
                    .graphicsQueueFamily = graphicsQueue,
                    .presentationQueueFamily = presentationQueue,
                    .enabledFeatures = {.dynamicRendering = true, .synchronization2 = true, .timelineSemaphore = true},
                    .enabledExtensions = std::move(deviceExtensions),
                });
            }

            [[nodiscard]] Result<std::vector<std::string>> ResolveInstanceExtensions(const RenderBackendConfig &config,
                                                                                     const VulkanLoaderCapabilities &loader) {
                std::vector<std::string> required;
                if (config.requirePresentation) {
                    required.emplace_back(SurfaceExtension);
                    auto platform = runtimePort_->RequiredPresentationExtensions();
                    if (platform.HasError()) {
                        return Result<std::vector<std::string>>::Failure(std::move(platform).ErrorValue());
                    }
                    if (platform.Value().empty() || !std::ranges::all_of(platform.Value(), [](const std::string &extension) {
                        return IsQualifiedPresentationExtension(extension);
                    })) {
                        return Result<std::vector<std::string>>::Failure(
                            MakeVulkanError(VulkanBackendErrors::PresentationExtensionUnsupported,
                                            "The platform did not provide one qualified Vulkan WSI extension."));
                    }
                    required.insert(required.end(), platform.Value().begin(), platform.Value().end());
                }
                if (config.enableValidation) {
                    required.emplace_back(DebugUtilsExtension);
                    if (!Detail::VulkanContains(loader.layers, ValidationLayer)) {
                        return Result<std::vector<std::string>>::Failure(
                            MakeVulkanError(VulkanBackendErrors::ValidationUnavailable, "VK_LAYER_KHRONOS_validation is unavailable."));
                    }
                }
                std::ranges::sort(required);
                required.erase(std::ranges::unique(required).begin(), required.end());
                for (const std::string &extension : required) {
                    if (!Detail::VulkanContains(loader.instanceExtensions, extension)) {
                        return Result<std::vector<std::string>>::Failure(
                            MakeVulkanError(VulkanBackendErrors::InstanceExtensionMissing,
                                            "Required Vulkan instance extension is missing: " + extension));
                    }
                }
                return Result<std::vector<std::string>>::Success(std::move(required));
            }

            [[nodiscard]] Result<const VulkanAdapterCandidate *> SelectCandidate(const RenderBackendConfig &config,
                                                                                 const VulkanAdapterEnumeration &enumeration) const {
                if (Result<void> validation = ValidateEnumeration(enumeration); validation.HasError()) {
                    return Result<const VulkanAdapterCandidate *>::Failure(std::move(validation).ErrorValue());
                }
                if (config.adapter.has_value()) {
                    return SelectExplicitCandidate(config, enumeration);
                }
                const VulkanAdapterCandidate *best = SelectAutomaticCandidate(config, enumeration);
                if (best == nullptr) {
                    return Result<const VulkanAdapterCandidate *>::Failure(
                        MakeVulkanError(VulkanBackendErrors::AdapterRequirementsUnsupported,
                                        "No enumerated adapter provides Vulkan 1.3, the required features, and compatible queues."));
                }
                return Result<const VulkanAdapterCandidate *>::Success(best);
            }

            template <typename T> [[nodiscard]] Result<T> Unsupported() const {
                const ErrorCodeDescriptor &descriptor =
                    initialized_ ? VulkanBackendErrors::UnsupportedOperation : VulkanBackendErrors::NotInitialized;
                return Result<T>::Failure(MakeVulkanError(descriptor, {}));
            }

            [[nodiscard]] Result<void> UnsupportedVoid() const {
                const ErrorCodeDescriptor &descriptor =
                    initialized_ ? VulkanBackendErrors::UnsupportedOperation : VulkanBackendErrors::NotInitialized;
                return Result<void>::Failure(MakeVulkanError(descriptor, {}));
            }

            [[nodiscard]] Result<void> FailAndRollback(Error error) {
                Rollback();
                return Result<void>::Failure(std::move(error));
            }

            void Rollback() noexcept {
                resourceBackend_ = nullptr;
                if (deviceCreated_) {
                    runtimePort_->DestroyDevice();
                    deviceCreated_ = false;
                }
                if (instanceCreated_) {
                    runtimePort_->DestroyInstance();
                    instanceCreated_ = false;
                }
                if (loaderAcquired_) {
                    runtimePort_->ReleaseLoader();
                    loaderAcquired_ = false;
                }
                if (ownsLease_) {
                    lease_->claimed = false;
                    ownsLease_ = false;
                }
            }

            IVulkanRuntimePort *runtimePort_{nullptr};
            IVulkanResourcePort *resourcePort_{nullptr};
            IRenderResourceBackend *resourceBackend_{nullptr};
            std::shared_ptr<VulkanRuntimeLease> lease_;
            RenderBackendCapabilities capabilities_{.backend = RenderBackendId{"vulkan"}};
            bool initialized_{false};
            bool loaderAcquired_{false};
            bool instanceCreated_{false};
            bool deviceCreated_{false};
            bool ownsLease_{false};
        };

        class VulkanBackendProvider final : public IRenderBackendProvider {
        public:
            explicit VulkanBackendProvider(IVulkanRuntimePort &runtimePort) : runtimePort_(&runtimePort) {}

            explicit VulkanBackendProvider(std::shared_ptr<IVulkanRuntimePort> runtimePort)
                : runtimeOwner_(std::move(runtimePort)), runtimePort_(runtimeOwner_.get()) {}

            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<VulkanRenderBackend>(*runtimePort_, lease_));
            }

        private:
            std::shared_ptr<IVulkanRuntimePort> runtimeOwner_;
            IVulkanRuntimePort *runtimePort_{nullptr};
            std::shared_ptr<VulkanRuntimeLease> lease_{std::make_shared<VulkanRuntimeLease>()};
        };
    }  // namespace

    namespace Detail {
        Result<void> RegisterVulkanRenderBackendWithRuntimePort(RenderBackendRegistry &registry, IVulkanRuntimePort &runtimePort) {
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"vulkan"},
                .displayName = "Vulkan",
                .provider = std::make_unique<VulkanBackendProvider>(runtimePort),
            });
        }

        Result<void> RegisterVulkanRenderBackendWithOwnedRuntimePort(RenderBackendRegistry &registry,
                                                                     std::shared_ptr<IVulkanRuntimePort> runtimePort) {
            if (!runtimePort) {
                return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::InvalidConfig, "Vulkan runtime is null."));
            }
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"vulkan"},
                .displayName = "Vulkan",
                .provider = std::make_unique<VulkanBackendProvider>(std::move(runtimePort)),
            });
        }
    }  // namespace Detail
}  // namespace Horo::Render
