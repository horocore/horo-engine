#include "OpenGLBackendInternal.h"
#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <functional>
#include <glad/gl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t colorBufferBit = 0x00004000U;

        [[nodiscard]] std::optional<std::size_t> ConservativeRequirement(const std::size_t payload) noexcept {
            constexpr std::size_t alignment = 256;
            constexpr std::size_t mask = alignment - 1;
            if (payload > std::numeric_limits<std::size_t>::max() - mask)
                return std::nullopt;
            return (payload + mask) & ~mask;
        }

        [[nodiscard]] bool MatchesPlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes == 0;
        }

        /** @brief Serializes ownership of the single context retained by one presentation port. */
        struct OpenGLContextLease {
            bool claimed{false};
        };

        struct OpenGLTextureFormat {
            std::int32_t internal;
            std::uint32_t external;
            std::uint32_t type;
        };

        [[nodiscard]] OpenGLTextureFormat TextureFormat(const RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            switch (format) {
                case Rgba8Unorm:
                    return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
                case Depth24Stencil8:
                    return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
                case Depth32Float:
                    return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
                case R8Unorm:
                case Rg8Unorm:
                case Rgba8UnormSrgb:
                case Bgra8Unorm:
                case Bgra8UnormSrgb:
                case R16Float:
                case Rg16Float:
                case Rgba16Float:
                case R32Float:
                case Rg32Float:
                case Rgba32Float:
                case Depth16Unorm:
                case Depth32FloatStencil8:
                    return {};
            }
            return {};
        }

        [[nodiscard]] bool IsValidRenderTargetRequest(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                      const std::uint64_t depthAttachment) noexcept {
            constexpr auto maximumObject = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
            const std::array valid{
                descriptor.IsValid(),
                colorAttachment != 0 || depthAttachment != 0,
                colorAttachment <= maximumObject,
                depthAttachment <= maximumObject,
            };
            return std::ranges::all_of(valid, std::identity{});
        }

        [[nodiscard]] bool VersionAtLeast(const OpenGLContextFacts &facts, const std::uint16_t major, const std::uint16_t minor) noexcept {
            return facts.majorVersion > major || (facts.majorVersion == major && facts.minorVersion >= minor);
        }

        [[nodiscard]] Result<void> ValidateContextFacts(const OpenGLContextFacts &facts, const OpenGLBackendOptions &options) {
            if (facts.apiFamily != OpenGLApiFamily::Desktop)
                return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedApiFamily, "Actual context is not desktop OpenGL."));
            if (!VersionAtLeast(facts, options.majorVersion, options.minorVersion))
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::UnsupportedVersion, "Actual OpenGL context is below the requested version."));
            if (facts.profile != OpenGLContextProfile::Core)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::UnsupportedProfile, "Actual OpenGL context is not Core profile."));
            if (!facts.requiredEntryPointsAvailable)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints, "OpenGL 4.1 Core command dispatch is incomplete."));
            if (facts.maxTexture2DSize == 0 || facts.maxColorAttachments == 0 || facts.maxVertexAttributes == 0)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::InvalidCapabilities, "OpenGL baseline limits must be non-zero."));
            return Result<void>::Success();
        }

        /** @brief OpenGL backend owning one presentation-port context lifecycle. */
        class OpenGLRenderBackend final : public IRenderBackend {  // NOSONAR(cpp:S1448)
        public:
            OpenGLRenderBackend(IOpenGLPresentationPort &presentationPort, const OpenGLBackendOptions options,
                                const Detail::OpenGLCommandFunctions &functions, std::shared_ptr<OpenGLContextLease> contextLease) noexcept
                : presentationPort_(&presentationPort), options_(options), functions_(functions), contextLease_(std::move(contextLease)) {}

            /** @brief Releases a remaining OpenGL context as a lifecycle fallback. */
            ~OpenGLRenderBackend() override {
                Shutdown();
            }

            OpenGLRenderBackend(const OpenGLRenderBackend &) = delete;
            OpenGLRenderBackend &operator=(const OpenGLRenderBackend &) = delete;
            OpenGLRenderBackend(OpenGLRenderBackend &&) = delete;
            OpenGLRenderBackend &operator=(OpenGLRenderBackend &&) = delete;

            /** @copydoc IRenderBackend::Initialize */
            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (initialized_) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::AlreadyInitialized, "Renderer backend is already initialized."));
                }
                if (!functions_.IsValid() || options_.majorVersion == 0 || !config.IsValid()) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL backend configuration is invalid."));
                }
                if (contextLease_->claimed) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::PresentationInUse,
                                                           "OpenGL presentation attachment is already owned by another backend."));
                }
                contextLease_->claimed = true;
                ownsContextLease_ = true;
                ownerThread_ = std::this_thread::get_id();
                if (Result<void> attempt = InitializeAttempt(config, config.enableValidation); attempt.HasError()) {
                    if (!config.enableValidation) {
                        ReleaseContextLease();
                        ownerThread_ = {};
                        return attempt;
                    }

                    Error debugFailure = attempt.ErrorValue();
                    if (Result<void> retry = InitializeAttempt(config, false); retry.HasError()) {
                        Error retryFailure = retry.ErrorValue();
                        ReleaseContextLease();
                        ownerThread_ = {};
                        return Result<void>::Failure(WithCause(std::move(retryFailure), std::move(debugFailure)));
                    }
                }
                initialized_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

#include "OpenGLRenderBackendResources.inl"

            /** @copydoc IRenderBackend::DestroyBuffer */
            void DestroyBuffer(const std::uint64_t backendInstance) noexcept override {
                if (!IsOwnerThread())
                    return;
                DeleteTrackedObject(functions_.buffers.deleteBuffers, buffers_, backendInstance);
            }

            /** @copydoc IRenderBackend::DestroyMesh */
            void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
                if (!IsOwnerThread())
                    return;
                DeleteTrackedObject(functions_.vertexArrays.deleteVertexArrays, meshes_, backendInstance);
            }

            void DestroyTexture(const std::uint64_t backendInstance) noexcept override {
                if (!IsOwnerThread())
                    return;
                textureFormats_.erase(static_cast<std::uint32_t>(backendInstance));
                DeleteTrackedObject(functions_.textures.deleteTextures, textures_, backendInstance);
            }

            void DestroyTextureView(const std::uint64_t backendInstance) noexcept override {
                if (!IsOwnerThread())
                    return;
                textureViewFormats_.erase(static_cast<std::uint32_t>(backendInstance));
            }

            void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
                if (!IsOwnerThread())
                    return;
                DeleteTrackedObject(functions_.framebuffers.deleteFramebuffers, renderTargets_, backendInstance);
            }

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!IsOwnerThread())
                    return WrongThread<FrameToken>();
                if (!initialized_) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (frameActive_) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::FrameAlreadyActive, "A renderer frame is already active."));
                }
                if (constexpr auto maxViewportExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
                    descriptor.frameNumber == 0 || !descriptor.outputExtent.IsValid() ||
                    descriptor.outputExtent.width > maxViewportExtent || descriptor.outputExtent.height > maxViewportExtent) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidFrameDescriptor, "Frame number and output extent must be valid."));
                }

                if (nextFrameToken_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<FrameToken>::Failure(
                        MakeError(OpenGLBackendErrors::FrameTokenExhausted, "Frame token space is exhausted."));
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError()) {
                    return Result<FrameToken>::Failure(current.ErrorValue());
                }

                functions_.viewport(0, 0, static_cast<std::int32_t>(descriptor.outputExtent.width),
                                    static_cast<std::int32_t>(descriptor.outputExtent.height));
                frameActive_ = true;
                activeFrame_ = FrameToken{nextFrameToken_++};
                return Result<FrameToken>::Success(activeFrame_);
            }

            /** @copydoc IRenderBackend::Execute */
            Result<void> Execute(const RenderExecutionPlan &plan) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (const Result<void> valid = ValidatePlan(plan); valid.HasError()) {
                    return valid;
                }

                for (const RenderPassDescriptor &pass : plan.orderedPasses) {
                    if (!pass.primaryOutput.has_value()) {
                        continue;
                    }
                    const PrimaryOutputAttachment &attachment = *pass.primaryOutput;
                    if (attachment.loadOperation == AttachmentLoadOperation::Clear) {
                        const ClearColor &color = attachment.clearColor;
                        functions_.clearColor(color.red, color.green, color.blue, color.alpha);
                        functions_.clear(colorBufferBit);
                    }
                }
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Present */
            Result<void> Present(const FrameToken frame) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (const Result<void> state = ValidateActiveFrame(frame); state.HasError()) {
                    return state;
                }

                if (const Result<void> presented = presentationPort_->SwapBuffers(); presented.HasError()) {
                    return Result<void>::Failure(presented.ErrorValue());
                }

                AbortActiveFrame();
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::AbortFrame */
            void AbortFrame(const FrameToken frame) noexcept override {
                if (!IsOwnerThread())
                    return;
                if (frameActive_ && frame == activeFrame_) {
                    AbortActiveFrame();
                }
            }

            /** @copydoc IRenderBackend::AbortActiveFrame */
            void AbortActiveFrame() noexcept override {
                if (!IsOwnerThread())
                    return;
                frameActive_ = false;
                activeFrame_ = {};
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(const FramebufferExtent extent) override {
                if (!IsOwnerThread())
                    return WrongThread<void>();
                if (!initialized_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!extent.IsValid()) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::InvalidExtent, "Renderer output extent must be non-zero."));
                }
                if (frameActive_) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::FrameActive, "Renderer output cannot resize while a frame is active."));
                }

                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Shutdown */
            void Shutdown() noexcept override {
                // Native cleanup is illegal from another thread. Preserve the owner-visible
                // state so the host can dispatch shutdown back to the context owner.
                if (ownerThread_ != std::thread::id{} && !IsOwnerThread())
                    return;
                AbortActiveFrame();
                DestroyRemainingResources();
                initialized_ = false;
                DestroyContext();
                ownerThread_ = {};
            }

        private:
            [[nodiscard]] Result<void> InitializeAttempt(const RenderBackendConfig &config, const bool enableDebugContext) {
                // Assume ownership before crossing the platform boundary. A typed failure is
                // contractually non-retaining; an exception may occur after native creation,
                // so the frontend's rollback must still call DestroyContext().
                contextCreated_ = true;
                if (const Result<void> created = presentationPort_->CreateContext(OpenGLContextDescriptor{
                        .majorVersion = options_.majorVersion,
                        .minorVersion = options_.minorVersion,
                        .profile = OpenGLContextProfile::Core,
                        .enableDebugContext = enableDebugContext,
                    });
                    created.HasError()) {
                    contextCreated_ = false;
                    return Result<void>::Failure(created.ErrorValue());
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError())
                    return RollbackAttempt(current.ErrorValue());
                if (const Result<void> loaded = presentationPort_->LoadCommandDispatch(); loaded.HasError())
                    return RollbackAttempt(loaded.ErrorValue());
                const Result<OpenGLContextFacts> facts = presentationPort_->QueryContextFacts();
                if (facts.HasError())
                    return RollbackAttempt(facts.ErrorValue());
                if (const Result<void> admitted = ValidateContextFacts(facts.Value(), options_); admitted.HasError())
                    return RollbackAttempt(admitted.ErrorValue());
                if (const Result<void> presentMode = presentationPort_->SetPresentMode(config.presentMode); presentMode.HasError())
                    return RollbackAttempt(presentMode.ErrorValue());

                contextFacts_ = facts.Value();
                const bool resourcesAvailable = functions_.HasResourceFunctions();
                capabilities_.supportsOffscreenTargets = resourcesAvailable && contextFacts_.maxColorAttachments > 0;
                capabilities_.supportsBufferResources = resourcesAvailable;
                capabilities_.supportsMeshResources = resourcesAvailable && contextFacts_.maxVertexAttributes > 0;
                capabilities_.supportsTextureResources = resourcesAvailable && contextFacts_.maxTexture2DSize > 0;
                capabilities_.supportsRenderTargetResources = resourcesAvailable && contextFacts_.maxColorAttachments > 0;
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> RollbackAttempt(Error error) {
                DestroyNativeContext();
                return Result<void>::Failure(std::move(error));
            }

            template <typename T> [[nodiscard]] Result<T> WrongThread() const {
                return Result<T>::Failure(MakeError(OpenGLBackendErrors::WrongThread,
                                                    "OpenGL operation must execute on the thread that initialized the context."));
            }

            [[nodiscard]] bool IsOwnerThread() const noexcept {
                return ownerThread_ == std::thread::id{} || ownerThread_ == std::this_thread::get_id();
            }

            [[nodiscard]] Result<std::uint64_t> ResourceUnavailable(std::string message) const {
                return Result<std::uint64_t>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation, std::move(message)));
            }

            template <typename Destroy> static void DeleteObject(const Destroy destroy, const std::uint64_t backendInstance) noexcept {
                if (destroy != nullptr && backendInstance != 0 && backendInstance <= std::numeric_limits<std::uint32_t>::max()) {
                    const auto object = static_cast<std::uint32_t>(backendInstance);
                    destroy(1, &object);
                }
            }

            template <typename Destroy>
            static void DeleteTrackedObject(const Destroy destroy, std::unordered_set<std::uint32_t> &objects,
                                            const std::uint64_t backendInstance) noexcept {
                if (backendInstance <= std::numeric_limits<std::uint32_t>::max() &&
                    objects.erase(static_cast<std::uint32_t>(backendInstance)) > 0)
                    DeleteObject(destroy, backendInstance);
            }

            void DestroyRemainingResources() noexcept {
                const auto destroyAll = [](const auto destroy, std::unordered_set<std::uint32_t> &objects) {
                    for (const std::uint32_t object : objects)
                        destroy(1, &object);
                    objects.clear();
                };
                if (functions_.HasResourceFunctions()) {
                    destroyAll(functions_.framebuffers.deleteFramebuffers, renderTargets_);
                    destroyAll(functions_.vertexArrays.deleteVertexArrays, meshes_);
                    destroyAll(functions_.textures.deleteTextures, textures_);
                    destroyAll(functions_.buffers.deleteBuffers, buffers_);
                }
                textureViewFormats_.clear();
                textureFormats_.clear();
            }

            [[nodiscard]] Result<void> ValidateActiveFrame(const FrameToken frame) const {
                if (!initialized_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeError(OpenGLBackendErrors::FrameTokenMismatch, "Frame token does not match the active frame."));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidatePlan(const RenderExecutionPlan &plan) const {
                if (const Result<void> state = ValidateActiveFrame(plan.frame); state.HasError()) {
                    return state;
                }

                for (std::size_t index = 0; index < plan.orderedPasses.size(); ++index) {
                    const RenderPassDescriptor &pass = plan.orderedPasses[index];
                    if (!pass.id.IsValid()) {
                        return Result<void>::Failure(
                            MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Execution plan contains an invalid render pass ID."));
                    }
                    if (pass.kind != RenderPassKind::Graphics) {
                        return Result<void>::Failure(
                            MakeError(OpenGLBackendErrors::UnsupportedPassKind, "Initial OpenGL backend supports graphics passes only."));
                    }
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (pass.id == plan.orderedPasses[previous].id) {
                            return Result<void>::Failure(
                                MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Execution plan contains duplicate render pass IDs."));
                        }
                    }
                    if (!pass.primaryOutput.has_value()) {
                        continue;
                    }

                    if (!pass.primaryOutput->IsValid()) {
                        return Result<void>::Failure(
                            MakeError(OpenGLBackendErrors::InvalidExecutionPlan, "Primary output attachment operations are invalid."));
                    }
                }
                return Result<void>::Success();
            }

            void DestroyContext() noexcept {
                DestroyNativeContext();
                ReleaseContextLease();
            }

            void DestroyNativeContext() noexcept {
                if (contextCreated_)
                    presentationPort_->DestroyContext();
                contextCreated_ = false;
                contextFacts_ = {};
                capabilities_.supportsOffscreenTargets = false;
                capabilities_.supportsBufferResources = false;
                capabilities_.supportsMeshResources = false;
                capabilities_.supportsTextureResources = false;
                capabilities_.supportsRenderTargetResources = false;
            }

            void ReleaseContextLease() noexcept {
                if (ownsContextLease_) {
                    contextLease_->claimed = false;
                    ownsContextLease_ = false;
                }
            }

            IOpenGLPresentationPort *presentationPort_{nullptr};
            OpenGLBackendOptions options_{};
            Detail::OpenGLCommandFunctions functions_{};
            std::shared_ptr<OpenGLContextLease> contextLease_;
            std::unordered_map<std::uint32_t, RenderTextureFormat> textureFormats_;
            std::unordered_map<std::uint32_t, RenderTextureFormat> textureViewFormats_;
            std::unordered_set<std::uint32_t> buffers_;
            std::unordered_set<std::uint32_t> meshes_;
            std::unordered_set<std::uint32_t> textures_;
            std::unordered_set<std::uint32_t> renderTargets_;
            RenderBackendCapabilities capabilities_{
                .backend = RenderBackendId{"opengl"},
                .presentsToWindow = true,
                .supportsOffscreenTargets = false,
                .supportsTimestampQueries = false,
                .supportsCompute = false,
                .supportsBindlessResources = false,
                .supportsRayTracing = false,
            };
            OpenGLContextFacts contextFacts_{};
            std::thread::id ownerThread_{};
            FrameToken activeFrame_{};
            std::uint64_t nextFrameToken_{1};
            bool initialized_{false};
            bool contextCreated_{false};
            bool ownsContextLease_{false};
            bool frameActive_{false};
        };

        /** @brief Provides inert OpenGL backend instances bound to one borrowed presentation port. */
        class OpenGLBackendProvider final : public IRenderBackendProvider {
        public:
            OpenGLBackendProvider(IOpenGLPresentationPort &presentationPort, const OpenGLBackendOptions options,
                                  const Detail::OpenGLCommandFunctions &functions)
                : presentationPort_(&presentationPort), options_(options), functions_(functions) {}

            /** @copydoc IRenderBackendProvider::Create */
            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                return Result<std::unique_ptr<IRenderBackend>>::Success(
                    std::make_unique<OpenGLRenderBackend>(*presentationPort_, options_, functions_, contextLease_));
            }

        private:
            IOpenGLPresentationPort *presentationPort_{nullptr};
            OpenGLBackendOptions options_{};
            Detail::OpenGLCommandFunctions functions_{};
            std::shared_ptr<OpenGLContextLease> contextLease_{std::make_shared<OpenGLContextLease>()};
        };
    }  // namespace

    namespace Detail {
        Result<void> RegisterOpenGLRenderBackendWithFunctions(RenderBackendRegistry &registry, IOpenGLPresentationPort &presentationPort,
                                                              const OpenGLBackendOptions options, const OpenGLCommandFunctions &functions) {
            if (!functions.IsValid() || options.majorVersion < 4 || (options.majorVersion == 4 && options.minorVersion < 1)) {
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::InvalidRegistration, "OpenGL backend registration options are invalid."));
            }
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"opengl"},
                .displayName = "OpenGL",
                .provider = std::make_unique<OpenGLBackendProvider>(presentationPort, options, functions),
            });
        }
    }  // namespace Detail

}  // namespace Horo::Render
