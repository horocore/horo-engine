#include "VulkanAndroidBackendModule.h"
#include "VulkanAndroidRuntime.h"

#include <memory>

namespace Horo::Render {
    namespace {
        /** @brief Returns explicit unsupported without creating any native resource or fallback. */
        template <class T> Result<T> Unsupported() {
            return Result<T>::Failure(MakeError(AndroidVulkan::Errors::Unsupported));
        }

        /** @brief Implements only the admitted primary-output clear/presentation profile. */
        class Backend final : public IRenderBackend {
        public:
            explicit Backend(Platform::Android::AndroidWindowHost &windows) : runtime_(windows) {}

            /** @copydoc IRenderBackend::Initialize */
            Result<void> Initialize(const RenderBackendConfig &config) override {
                return runtime_.Initialize(config);
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return runtime_.capabilities;
            }

            /** @copydoc IRenderBackend::PollPresentationAvailability */
            Result<bool> PollPresentationAvailability(FramebufferExtent extent) override {
                return runtime_.Availability(extent);
            }

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &frame) override {
                return runtime_.Begin(frame);
            }

            /** @copydoc IRenderBackend::Execute */
            Result<void> Execute(const RenderExecutionPlan &plan) override {
                return runtime_.Execute(plan);
            }

            /** @copydoc IRenderBackend::Present */
            Result<void> Present(FrameToken frame) override {
                return runtime_.Present(frame);
            }

            /** @copydoc IRenderBackend::AbortFrame */
            void AbortFrame(FrameToken frame) noexcept override {
                runtime_.Abort(frame);
            }

            /** @copydoc IRenderBackend::AbortActiveFrame */
            void AbortActiveFrame() noexcept override {
                runtime_.AbortActive();
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(FramebufferExtent extent) override {
                const auto ready = runtime_.Availability(extent);
                if (ready.HasError())
                    return Result<void>::Failure(ready.ErrorValue());
                if (!ready.Value())
                    return Result<void>::Failure(MakeError(AndroidVulkan::Errors::Pending));
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::BeginShutdown */
            Result<void> BeginShutdown() override {
                return runtime_.Close();
            }

            /** @copydoc IRenderBackend::PollDrain */
            Result<RenderBackendDrainProgress> PollDrain() override {
                return runtime_.Drain();
            }

            /** @copydoc IRenderResourceBackend::QueryBufferMemoryCost */
            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &) const override {
                return Unsupported<RenderMemoryCostPlan>();
            }

            /** @copydoc IRenderResourceBackend::QueryTextureMemoryCost */
            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &) const override {
                return Unsupported<RenderMemoryCostPlan>();
            }

            /** @copydoc IRenderResourceBackend::CreateBuffer */
            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                               RenderResourceNativeLifetime) override {
                return Unsupported<std::uint64_t>();
            }

            /** @copydoc IRenderResourceBackend::CreateMesh */
            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t,
                                             RenderResourceNativeLifetime) override {
                return Unsupported<std::uint64_t>();
            }

            /** @copydoc IRenderResourceBackend::CreateTexture */
            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                                RenderResourceNativeLifetime) override {
                return Unsupported<std::uint64_t>();
            }

            /** @copydoc IRenderResourceBackend::CreateTextureView */
            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t,
                                                    RenderResourceNativeLifetime) override {
                return Unsupported<std::uint64_t>();
            }

            /** @copydoc IRenderResourceBackend::CreateRenderTarget */
            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t,
                                                     RenderResourceNativeLifetime) override {
                return Unsupported<std::uint64_t>();
            }

            /** @copydoc IRenderResourceBackend::DestroyBuffer */
            void DestroyBuffer(std::uint64_t) noexcept override {}

            /** @copydoc IRenderResourceBackend::DestroyMesh */
            void DestroyMesh(std::uint64_t) noexcept override {}

            /** @copydoc IRenderResourceBackend::DestroyTexture */
            void DestroyTexture(std::uint64_t) noexcept override {}

            /** @copydoc IRenderResourceBackend::DestroyTextureView */
            void DestroyTextureView(std::uint64_t) noexcept override {}

            /** @copydoc IRenderResourceBackend::DestroyRenderTarget */
            void DestroyRenderTarget(std::uint64_t) noexcept override {}

        private:
            AndroidVulkan::Runtime runtime_;
        };

        /** @brief Captures only the borrowed explicit window service, never a native window or device. */
        class Provider final : public IRenderBackendProvider {
        public:
            explicit Provider(Platform::Android::AndroidWindowHost &windows) : windows_(windows) {}

            /** @copydoc IRenderBackendProvider::Create */
            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<Backend>(windows_));
            }

        private:
            Platform::Android::AndroidWindowHost &windows_;
        };
    }  // namespace

    /** @copydoc GetVulkanAndroidBackendModuleInfo */
    RenderBackendModuleInfo GetVulkanAndroidBackendModuleInfo() {
        return {RenderBackendId{"vulkan-android"},
                "Android Vulkan 1.1 development presentation",
                {RenderPresentationKind::Vulkan, true, true},
                true};
    }

    /** @copydoc IsVulkanAndroidBackpressure */
    bool IsVulkanAndroidBackpressure(const Error &error) noexcept {
        return error.domain.Value() == AndroidVulkan::Errors::Pending.domain.Value() &&
               error.code.Value() == AndroidVulkan::Errors::Pending.code.Value();
    }

    /** @copydoc RegisterVulkanAndroidRenderBackend */
    Result<void> RegisterVulkanAndroidRenderBackend(RenderBackendRegistry &registry, Platform::Android::AndroidWindowHost &windows) {
        auto module = GetVulkanAndroidBackendModuleInfo();
        return registry.Register({std::move(module.id), std::move(module.displayName), std::make_unique<Provider>(windows)});
    }
}  // namespace Horo::Render
