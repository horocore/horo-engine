#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "RenderExternalImages.h"
#include "VulkanBackendInternal.h"
#include "VulkanRenderBackendErrors.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
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

#if defined(_WIN32) || defined(__linux__)
        [[nodiscard]] bool IsQualifiedPresentationExtension(const std::string_view extension) noexcept {
#if defined(_WIN32)
            return extension == "VK_KHR_win32_surface";
#elif defined(__linux__)
            return extension == "VK_KHR_xlib_surface" || extension == "VK_KHR_xcb_surface" || extension == "VK_KHR_wayland_surface";
#endif
        }
#endif

        struct VulkanRuntimeLease {
            bool claimed{false};
        };

        class VulkanRenderBackend final : public IRenderBackend, public Detail::IRenderExternalTextureBackend {  // NOSONAR(cpp:S1448)
        public:
            VulkanRenderBackend(IVulkanRuntimePort &runtimePort, std::shared_ptr<VulkanRuntimeLease> lease) noexcept
                : runtimePort_(&runtimePort), resourcePort_(dynamic_cast<IVulkanResourcePort *>(&runtimePort)),
                  commandPort_(dynamic_cast<IVulkanCommandPort *>(&runtimePort)), lease_(std::move(lease)) {}

            ~VulkanRenderBackend() override {
                try {
                    const auto begun = BeginShutdown();
                    const auto drained = PollDrain();
                    if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                        std::terminate();

                } catch (...) {
                    // Destruction cannot release native owners without proven terminal retirement.
                    std::terminate();
                }
            }

            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (config.presentationOutput)
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnsupported));
                if (Result<void> prepared = PrepareInitialization(config); prepared.HasError()) {
                    return prepared;
                }
                if (Result<void> instance = InitializeInstance(config); instance.HasError()) {
                    return FailAndRollback(std::move(instance).ErrorValue());
                }
                if (Result<void> device = InitializeDevice(config); device.HasError()) {
                    return FailAndRollback(std::move(device).ErrorValue());
                }
                CommitInitialization(config);
                return Result<void>::Success();
            }

#include "VulkanBackendShaderDelegation.inl"

            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->QueryBufferMemoryCost(descriptor)
                                                                   : Unsupported<RenderMemoryCostPlan>();
            }

            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                return resourceBackend_ != nullptr && initialized_ ? resourceBackend_->QueryTextureMemoryCost(descriptor)
                                                                   : Unsupported<RenderMemoryCostPlan>();
            }

            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                               const RenderMemoryPlacement &placement,
                                               RenderResourceNativeLifetime lifetime = {}) override {
                return resourceBackend_ != nullptr && initialized_
                           ? resourceBackend_->CreateBuffer(descriptor, bytes, placement, std::move(lifetime))
                           : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index,
                                             RenderResourceNativeLifetime lifetime = {}) override {
                return resourceBackend_ != nullptr && initialized_
                           ? resourceBackend_->CreateMesh(descriptor, vertex, index, std::move(lifetime))
                           : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                                const RenderMemoryPlacement &placement,
                                                RenderResourceNativeLifetime lifetime = {}) override {
                return resourceBackend_ != nullptr && initialized_
                           ? resourceBackend_->CreateTexture(descriptor, bytes, placement, std::move(lifetime))
                           : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture,
                                                    RenderResourceNativeLifetime lifetime = {}) override {
                return resourceBackend_ != nullptr && initialized_
                           ? resourceBackend_->CreateTextureView(descriptor, texture, std::move(lifetime))
                           : Unsupported<std::uint64_t>();
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color, std::uint64_t depth,
                                                     RenderResourceNativeLifetime lifetime = {}) override {
                return resourceBackend_ != nullptr && initialized_
                           ? resourceBackend_->CreateRenderTarget(descriptor, color, depth, std::move(lifetime))
                           : Unsupported<std::uint64_t>();
            }

            /** @copydoc Detail::IRenderExternalTextureBackend::ImportExternalTexture */
            Result<std::uint64_t> ImportExternalTexture(const Detail::IRenderExternalTextureSource &source,
                                                        RenderResourceNativeLifetime lifetime) override {
                auto *external = dynamic_cast<Detail::IRenderExternalTextureBackend *>(resourceBackend_);
                return initialized_ && external != nullptr ? external->ImportExternalTexture(source, std::move(lifetime))
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

            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return Result<FrameToken>::Failure(access.ErrorValue());
                auto frame = commandPort_->BeginFrame(descriptor);
                if (frame.HasValue())
                    activeFrame_ = frame.Value();
                return frame;
            }

            Result<void> Execute(const RenderExecutionPlan &) override {
                return UnsupportedVoid();
            }

            Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return access;
                if (request.frame != activeFrame_ || !activeFrame_.IsValid())
                    return Result<void>::Failure(
                        MakeVulkanError(VulkanBackendErrors::InvalidCommandState, "Graph frame identity is stale."));
                return commandPort_->ExecuteGraph(request);
            }

            Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return access;
                if (request.frame != activeFrame_ || !activeFrame_.IsValid())
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
                return commandPort_->ExecuteMaterialPass(request);
            }

            Result<void> Present(const FrameToken frame) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return access;
                if (frame != activeFrame_ || !activeFrame_.IsValid())
                    return Result<void>::Failure(
                        MakeVulkanError(VulkanBackendErrors::InvalidCommandState, "Submission frame identity is stale."));
                auto submitted = commandPort_->SubmitFrame(frame);
                activeFrame_ = {};
                return submitted;
            }

            /** @copydoc IRenderBackend::PrepareGuiComposition */
            Result<void> PrepareGuiComposition(const GuiCompositionRequest &request) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return access;
                if (request.frame != activeFrame_ || !activeFrame_.IsValid())
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
                return commandPort_->PrepareGuiComposition(request);
            }

            void AbortFrame(const FrameToken frame) noexcept override {
                if (ownerThread_ == std::this_thread::get_id() && commandPort_ != nullptr && frame == activeFrame_) {
                    commandPort_->AbortFrame(frame);
                    activeFrame_ = {};
                }
            }

            void AbortActiveFrame() noexcept override {
                AbortFrame(activeFrame_);
            }

            Result<void> Resize(const FramebufferExtent extent) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return access;
                if (activeFrame_.IsValid())
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
                return requirePresentation_ ? commandPort_->ResizePresentation(extent, presentMode_) : UnsupportedVoid();
            }

            Result<bool> PollPresentationAvailability(const FramebufferExtent extent) override {
                if (const auto access = CommandAccess(); access.HasError())
                    return Result<bool>::Failure(access.ErrorValue());
                if (activeFrame_.IsValid())
                    return Result<bool>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
                return requirePresentation_ ? commandPort_->PollPresentationAvailability(extent) : Result<bool>::Success(true);
            }

            Result<bool> ObserveSubmission(const RenderCpuWait &wait) override {
                if (ownerThread_ == std::thread::id{})
                    return Result<bool>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                if (ownerThread_ != std::this_thread::get_id())
                    return Result<bool>::Failure(MakeError(VulkanBackendErrors::WrongThread));
                if (!deviceCreated_)
                    return Result<bool>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                if (!wait.IsValid() || (activeFrame_.IsValid() && wait.purpose != RenderCpuWaitPurpose::Poll))
                    return Result<bool>::Failure(MakeError(VulkanBackendErrors::TimelineInvalid));
                return commandPort_ != nullptr ? commandPort_->ObserveSubmission(wait)
                                               : Result<bool>::Failure(MakeError(VulkanBackendErrors::UnsupportedOperation));
            }

            Result<void> BeginShutdown() override {
                if (ownerThread_ != std::thread::id{} && ownerThread_ != std::this_thread::get_id())
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::WrongThread, {}));
                closing_ = true;
                initialized_ = false;
                capabilities_.presentsToWindow = false;
                AbortActiveFrame();
                return commandPort_ != nullptr && deviceCreated_ ? commandPort_->BeginShutdownCommands() : Result<void>::Success();
            }

            Result<RenderBackendDrainProgress> PollDrain() override {
                using DrainResult = Result<RenderBackendDrainProgress>;
                if (ownerThread_ != std::thread::id{} && ownerThread_ != std::this_thread::get_id())
                    return DrainResult::Failure(MakeVulkanError(VulkanBackendErrors::WrongThread, {}));
                if (!closing_)
                    return DrainResult::Failure(
                        MakeVulkanError(VulkanBackendErrors::InvalidCommandState, "BeginShutdown must close Vulkan admission first."));
                if (commandPort_ != nullptr && deviceCreated_) {
                    const auto progress = commandPort_->PollCommandDrain();
                    if (progress.HasError() || progress.Value() == RenderBackendDrainProgress::Pending)
                        return progress;
                }
                Rollback();
                ownerThread_ = {};
                return DrainResult::Success(RenderBackendDrainProgress::Drained);
            }

        private:
            void CommitInitialization(const RenderBackendConfig &config) noexcept {
                using enum RenderCapability;
                capabilities_.presentsToWindow = config.requirePresentation && commandPort_ && commandPort_->SupportsWindowPresentation();
                resourceBackend_ = resourcePort_ != nullptr ? resourcePort_->ResourceBackend() : nullptr;
                initialized_ = true;
                capabilities_.supportsExplicitGraphSubmissions = commandPort_ != nullptr;
                capabilities_.supportsExplicitMaterialSubmissions = commandPort_ && commandPort_->SupportsMaterialCommands();
                capabilities_.supportsSubmissionObservation = commandPort_ != nullptr;
                capabilities_.support = resourcePort_ != nullptr ? resourcePort_->ResourceCapabilities() : RenderCapabilitySnapshot{};
                capabilities_.supportsBufferResources = capabilities_.support.features.Supports(BufferResources);
                capabilities_.supportsMeshResources = capabilities_.support.features.Supports(MeshResources);
                capabilities_.supportsTextureResources = capabilities_.support.features.Supports(TextureResources);
                capabilities_.supportsRenderTargetResources = capabilities_.support.features.Supports(RenderTargetResources);
                capabilities_.supportsOffscreenTargets = capabilities_.support.features.Supports(OffscreenTargets);
                requirePresentation_ = config.requirePresentation;
                presentMode_ = config.presentMode;
            }

            [[nodiscard]] Result<void> CommandAccess() const {
                if (ownerThread_ == std::thread::id{})
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::NotInitialized, {}));
                if (ownerThread_ != std::this_thread::get_id())
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::WrongThread, {}));
                if (!initialized_)
                    return Result<void>::Failure(MakeVulkanError(VulkanBackendErrors::NotInitialized, {}));
                if (requirePresentation_ && !capabilities_.presentsToWindow)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::PresentationUnsupported));
                return commandPort_ != nullptr ? Result<void>::Success() : UnsupportedVoid();
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
                ownerThread_ = std::this_thread::get_id();
                closing_ = false;
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
                auto host = runtimePort_->RequiredGraphicsRequirements();
                if (host.HasError())
                    return Result<void>::Failure(host.ErrorValue());
                hostRequirements_.emplace(std::move(host).Value());
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
                const auto [graphicsQueue, presentationQueue] = Detail::SelectQueues(*candidate, config.requirePresentation);
                const auto transferQueue = Detail::SelectTransferQueue(*candidate, config, graphicsQueue);
                if (transferQueue.HasError())
                    return Result<void>::Failure(transferQueue.ErrorValue());
                std::vector<std::string> deviceExtensions;
                if (config.requirePresentation) {
                    deviceExtensions.emplace_back(SwapchainExtension);
                    deviceExtensions.emplace_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                }
                if (!hostRequirements_)
                    return Result<void>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                auto required = Detail::ResolveHostDeviceExtensions(std::move(deviceExtensions), *candidate, *hostRequirements_);
                if (required.HasError())
                    return Result<void>::Failure(required.ErrorValue());
                deviceCreated_ = true;
                return runtimePort_->CreateDevice(VulkanDeviceRequest{
                    .adapter = candidate->id,
                    .graphicsQueueFamily = graphicsQueue,
                    .presentationQueueFamily = presentationQueue,
                    .transferQueueFamily = transferQueue.Value(),
                    .enabledFeatures = {.dynamicRendering = true, .synchronization2 = true, .timelineSemaphore = true},
                    .enabledExtensions = std::move(required).Value(),
                    .queueAssignments = config.queueAssignments,
                    .maxFramesInFlight = config.maxFramesInFlight,
                    .enableVertexLayerRouting = candidate->vertexLayerRouting,
                    .enableSwapchainMaintenance = config.requirePresentation,
                });
            }

            [[nodiscard]] Result<std::vector<std::string>> ResolveInstanceExtensions(const RenderBackendConfig &config,
                                                                                     const VulkanLoaderCapabilities &loader) {
                std::vector<std::string> required;
                if (config.requirePresentation) {
                    required.emplace_back(SurfaceExtension);
                    required.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                    required.emplace_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
                    auto platform = runtimePort_->RequiredPresentationExtensions();
                    if (platform.HasError()) {
                        return Result<std::vector<std::string>>::Failure(std::move(platform).ErrorValue());
                    }
#if defined(_WIN32) || defined(__linux__)
                    if (!std::ranges::any_of(platform.Value(), IsQualifiedPresentationExtension) ||
                        !std::ranges::all_of(platform.Value(), [](const std::string &extension) {
                        return IsQualifiedPresentationExtension(extension) || extension == SurfaceExtension ||
                               extension == VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME ||
                               extension == VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
                    })) {
                        return Result<std::vector<std::string>>::Failure(
                            MakeVulkanError(VulkanBackendErrors::PresentationExtensionUnsupported,
                                            "The platform did not provide one qualified Vulkan WSI extension."));
                    }
                    required.insert(required.end(), platform.Value().begin(), platform.Value().end());
#else
                    return Result<std::vector<std::string>>::Failure(
                        MakeVulkanError(VulkanBackendErrors::PresentationExtensionUnsupported,
                                        "The platform did not provide one qualified Vulkan WSI extension."));
#endif
                }
                if (config.enableValidation) {
                    required.emplace_back(DebugUtilsExtension);
                    if (!Detail::VulkanContains(loader.layers, ValidationLayer)) {
                        return Result<std::vector<std::string>>::Failure(
                            MakeVulkanError(VulkanBackendErrors::ValidationUnavailable, "VK_LAYER_KHRONOS_validation is unavailable."));
                    }
                }
                if (!hostRequirements_)
                    return Result<std::vector<std::string>>::Failure(MakeError(VulkanBackendErrors::NotInitialized));
                return Detail::ResolveHostInstanceExtensions(std::move(required), loader, *hostRequirements_);
            }

            [[nodiscard]] Result<const VulkanAdapterCandidate *> SelectCandidate(const RenderBackendConfig &config,
                                                                                 const VulkanAdapterEnumeration &enumeration) const {
                if (Result<void> validation = Detail::ValidateEnumeration(enumeration); validation.HasError()) {
                    return Result<const VulkanAdapterCandidate *>::Failure(std::move(validation).ErrorValue());
                }
                if (config.adapter.has_value()) {
                    return Detail::SelectExplicitCandidate(config, enumeration);
                }
                const VulkanAdapterCandidate *best = Detail::SelectAutomaticCandidate(config, enumeration);
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
                hostRequirements_.reset();
                if (ownsLease_) {
                    lease_->claimed = false;
                    ownsLease_ = false;
                }
            }

            IVulkanRuntimePort *runtimePort_{nullptr};
            std::optional<VulkanHostGraphicsRequirements> hostRequirements_;
            IVulkanResourcePort *resourcePort_{nullptr};
            IVulkanCommandPort *commandPort_{nullptr};
            IRenderResourceBackend *resourceBackend_{nullptr};
            std::shared_ptr<VulkanRuntimeLease> lease_;
            RenderBackendCapabilities capabilities_{.backend = RenderBackendId{"vulkan"}};
            bool initialized_{false};
            bool loaderAcquired_{false};
            bool instanceCreated_{false};
            bool deviceCreated_{false};
            bool ownsLease_{false};
            bool closing_{false};
            bool requirePresentation_{false};
            PresentMode presentMode_{PresentMode::Fifo};
            std::thread::id ownerThread_;
            FrameToken activeFrame_;
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
