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
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t colorBufferBit = 0x00004000U;

        [[nodiscard]] Error MakeOpenGLError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

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

        /** @brief OpenGL backend owning one presentation-port context lifecycle. */
        class OpenGLRenderBackend final : public IRenderBackend {
        public:
            OpenGLRenderBackend(IOpenGLPresentationPort &presentationPort, const OpenGLBackendOptions options,
                                const Detail::OpenGLCommandFunctions &functions, std::shared_ptr<OpenGLContextLease> contextLease) noexcept
                : presentationPort_(&presentationPort), options_(options), functions_(functions), contextLease_(std::move(contextLease)) {
                const bool resourcesAvailable = functions_.HasResourceFunctions();
                capabilities_.supportsOffscreenTargets = resourcesAvailable;
                capabilities_.supportsBufferResources = resourcesAvailable;
                capabilities_.supportsMeshResources = resourcesAvailable;
                capabilities_.supportsTextureResources = resourcesAvailable;
                capabilities_.supportsRenderTargetResources = resourcesAvailable;
            }

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
                        MakeOpenGLError(OpenGLBackendErrors::AlreadyInitialized, "Renderer backend is already initialized."));
                }
                if (!functions_.IsValid() || options_.majorVersion == 0 || !config.IsValid()) {
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL backend configuration is invalid."));
                }
                if (contextLease_->claimed) {
                    return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::PresentationInUse,
                                                                 "OpenGL presentation attachment is already owned by another backend."));
                }
                contextLease_->claimed = true;
                ownsContextLease_ = true;
                // Assume ownership before crossing the platform boundary. A typed failure is
                // contractually non-retaining; an exception may occur after native creation,
                // so the frontend's rollback must still call DestroyContext().
                contextCreated_ = true;
                if (const Result<void> created = presentationPort_->CreateContext(OpenGLContextDescriptor{
                        .majorVersion = options_.majorVersion,
                        .minorVersion = options_.minorVersion,
                        .profile = OpenGLContextProfile::Core,
                        .enableDebugContext = config.enableValidation,
                    });
                    created.HasError()) {
                    contextCreated_ = false;
                    ReleaseContextLease();
                    return Result<void>::Failure(created.ErrorValue());
                }

                if (const Result<void> current = presentationPort_->MakeCurrent(); current.HasError()) {
                    DestroyContext();
                    return Result<void>::Failure(current.ErrorValue());
                }
                if (const Result<void> loaded = presentationPort_->LoadCommandDispatch(); loaded.HasError()) {
                    DestroyContext();
                    return Result<void>::Failure(loaded.ErrorValue());
                }
                if (const Result<void> presentMode = presentationPort_->SetPresentMode(config.presentMode); presentMode.HasError()) {
                    DestroyContext();
                    return Result<void>::Failure(presentMode.ErrorValue());
                }

                initialized_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

            /** @copydoc IRenderBackend::QueryBufferMemoryCost */
            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                const auto required = ConservativeRequirement(descriptor.byteSize);
                if (!initialized_ || !functions_.HasResourceFunctions() || !descriptor.IsValid() || !required.has_value())
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                        "OpenGL buffer memory requirements are unavailable for this descriptor."));
                return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                              .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                              .provenance = RenderMemoryCostProvenance::Estimated,
                                                              .compatibility = RenderMemoryCompatibilityId{1},
                                                              .payloadBytes = descriptor.byteSize,
                                                              .requiredBytes = *required,
                                                              .alignment = 256});
            }

            /** @copydoc IRenderBackend::QueryTextureMemoryCost */
            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                const auto payload = RenderTextureBaseLevelByteSize(descriptor);
                const auto required = payload.has_value() ? ConservativeRequirement(*payload) : std::nullopt;
                if (!initialized_ || !functions_.HasResourceFunctions() || !payload.has_value() || !required.has_value() ||
                    TextureFormat(descriptor.format).internal == 0)
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                        "OpenGL texture memory requirements are unavailable for this descriptor."));
                return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                              .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                              .provenance = RenderMemoryCostProvenance::Estimated,
                                                              .compatibility = RenderMemoryCompatibilityId{2},
                                                              .payloadBytes = *payload,
                                                              .requiredBytes = *required,
                                                              .alignment = 256});
            }

            /** @copydoc IRenderBackend::CreateBuffer */
            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                               const RenderMemoryPlacement &placement) override {
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL buffer creation is unavailable in the current backend state.");
                const auto cost = QueryBufferMemoryCost(descriptor);
                if (!descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) || cost.HasError() ||
                    !MatchesPlacement(cost.Value(), placement) ||
                    descriptor.byteSize > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL buffer creation request is invalid."));
                std::uint32_t buffer = 0;
                functions_.buffers.generateBuffers(1, &buffer);
                if (buffer == 0)
                    return ResourceUnavailable("OpenGL failed to allocate a buffer object.");
                constexpr std::uint32_t target = GL_ARRAY_BUFFER;
                functions_.buffers.bindBuffer(target, buffer);
                functions_.buffers.bufferData(target, descriptor.byteSize, initialData, GL_STATIC_DRAW);
                functions_.buffers.bindBuffer(target, 0);
                buffers_.insert(buffer);
                return Result<std::uint64_t>::Success(buffer);
            }

            /** @copydoc IRenderBackend::CreateMesh */
            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                             const std::uint64_t indexBuffer) override {
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL mesh creation is unavailable in the current backend state.");
                if (!descriptor.IsValid() || vertexBuffer == 0 || indexBuffer == 0 ||
                    vertexBuffer > std::numeric_limits<std::uint32_t>::max() || indexBuffer > std::numeric_limits<std::uint32_t>::max())
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL mesh creation request is invalid."));
                std::uint32_t vertexArray = 0;
                functions_.vertexArrays.generateVertexArrays(1, &vertexArray);
                if (vertexArray == 0)
                    return ResourceUnavailable("OpenGL failed to allocate a mesh vertex array.");
                functions_.vertexArrays.bindVertexArray(0, vertexArray);
                functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(vertexBuffer));
                functions_.buffers.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<std::uint32_t>(indexBuffer));
                functions_.vertexArrays.vertexAttributePointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                               offsetof(MeshVertex, position));
                functions_.vertexArrays.enableVertexAttribute(0);
                functions_.vertexArrays.vertexAttributePointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                               offsetof(MeshVertex, normal));
                functions_.vertexArrays.enableVertexAttribute(1);
                functions_.vertexArrays.vertexAttributePointer(2, 2, GL_FLOAT, GL_FALSE, static_cast<std::int32_t>(descriptor.vertexStride),
                                                               offsetof(MeshVertex, uv));
                functions_.vertexArrays.enableVertexAttribute(2);
                functions_.vertexArrays.bindVertexArray(0, 0);
                functions_.buffers.bindBuffer(GL_ARRAY_BUFFER, 0);
                meshes_.insert(vertexArray);
                return Result<std::uint64_t>::Success(vertexArray);
            }

            /** @copydoc IRenderBackend::CreateTexture */
            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement) override {
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL texture creation is unavailable in the current backend state.");
                const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
                const auto cost = QueryTextureMemoryCost(descriptor);
                if (constexpr auto maximumExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
                    !descriptor.IsValid() || !bytes.has_value() || (!initialData.empty() && initialData.size() != *bytes) ||
                    cost.HasError() || !MatchesPlacement(cost.Value(), placement) || descriptor.extent.width > maximumExtent ||
                    descriptor.extent.height > maximumExtent)
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture creation request is invalid."));
                std::uint32_t texture = 0;
                functions_.textures.generateTextures(1, &texture);
                if (texture == 0)
                    return ResourceUnavailable("OpenGL failed to allocate a texture object.");
                functions_.textures.bindTexture(GL_TEXTURE_2D, texture);
                functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                functions_.textures.textureParameter(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                const auto format = TextureFormat(descriptor.format);
                functions_.textures.textureImage({
                    .target = GL_TEXTURE_2D,
                    .internalFormat = format.internal,
                    .width = static_cast<std::int32_t>(descriptor.extent.width),
                    .height = static_cast<std::int32_t>(descriptor.extent.height),
                    .format = format.external,
                    .type = format.type,
                    .initialData = initialData,
                });
                functions_.textures.bindTexture(GL_TEXTURE_2D, 0);
                textureFormats_.insert_or_assign(texture, descriptor.format);
                textures_.insert(texture);
                return Result<std::uint64_t>::Success(texture);
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL texture-view creation is unavailable in the current backend state.");
                if (!descriptor.IsValid() || texture == 0 || texture > std::numeric_limits<std::uint32_t>::max())
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture-view creation request is invalid."));
                if (const auto source = textureFormats_.find(static_cast<std::uint32_t>(texture));
                    source == textureFormats_.end() || source->second != descriptor.format)
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture-view format does not match its texture."));
                textureViewFormats_.insert_or_assign(static_cast<std::uint32_t>(texture), descriptor.format);
                return Result<std::uint64_t>::Success(texture);
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) override {
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL render-target creation is unavailable in the current backend state.");
                if (!IsValidRenderTargetRequest(descriptor, colorAttachment, depthAttachment))
                    return Result<std::uint64_t>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL render-target creation request is invalid."));
                std::uint32_t framebuffer = 0;
                functions_.framebuffers.generateFramebuffers(1, &framebuffer);
                if (framebuffer == 0)
                    return ResourceUnavailable("OpenGL failed to allocate a framebuffer object.");
                functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                if (colorAttachment != 0)
                    functions_.framebuffers.framebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                               static_cast<std::uint32_t>(colorAttachment), 0);
                else {
                    functions_.framebuffers.drawBuffer(GL_NONE);
                    functions_.framebuffers.readBuffer(GL_NONE);
                }
                if (const Result<void> depth = AttachDepthView(depthAttachment); depth.HasError()) {
                    functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, 0);
                    functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
                    return Result<std::uint64_t>::Failure(depth.ErrorValue());
                }
                const bool complete = functions_.framebuffers.checkFramebuffer(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                functions_.framebuffers.bindFramebuffer(GL_FRAMEBUFFER, 0);
                if (!complete) {
                    functions_.framebuffers.deleteFramebuffers(1, &framebuffer);
                    return ResourceUnavailable("OpenGL framebuffer attachments are incomplete.");
                }
                renderTargets_.insert(framebuffer);
                return Result<std::uint64_t>::Success(framebuffer);
            }

            [[nodiscard]] Result<void> AttachDepthView(const std::uint64_t depthAttachment) const {
                if (depthAttachment == 0)
                    return Result<void>::Success();
                const auto format = textureViewFormats_.find(static_cast<std::uint32_t>(depthAttachment));
                if (format == textureViewFormats_.end())
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidConfig, "OpenGL render-target depth view metadata is unavailable."));
                const std::uint32_t attachment =
                    format->second == RenderTextureFormat::Depth24Stencil8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
                functions_.framebuffers.framebufferTexture(GL_FRAMEBUFFER, attachment, static_cast<std::uint32_t>(depthAttachment), 0);
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::DestroyBuffer */
            void DestroyBuffer(const std::uint64_t backendInstance) noexcept override {
                DeleteTrackedObject(functions_.buffers.deleteBuffers, buffers_, backendInstance);
            }

            /** @copydoc IRenderBackend::DestroyMesh */
            void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
                DeleteTrackedObject(functions_.vertexArrays.deleteVertexArrays, meshes_, backendInstance);
            }

            void DestroyTexture(const std::uint64_t backendInstance) noexcept override {
                textureFormats_.erase(static_cast<std::uint32_t>(backendInstance));
                DeleteTrackedObject(functions_.textures.deleteTextures, textures_, backendInstance);
            }

            void DestroyTextureView(const std::uint64_t backendInstance) noexcept override {
                textureViewFormats_.erase(static_cast<std::uint32_t>(backendInstance));
            }

            void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
                DeleteTrackedObject(functions_.framebuffers.deleteFramebuffers, renderTargets_, backendInstance);
            }

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!initialized_) {
                    return Result<FrameToken>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (frameActive_) {
                    return Result<FrameToken>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::FrameAlreadyActive, "A renderer frame is already active."));
                }
                if (constexpr auto maxViewportExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
                    descriptor.frameNumber == 0 || !descriptor.outputExtent.IsValid() ||
                    descriptor.outputExtent.width > maxViewportExtent || descriptor.outputExtent.height > maxViewportExtent) {
                    return Result<FrameToken>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidFrameDescriptor, "Frame number and output extent must be valid."));
                }

                if (nextFrameToken_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<FrameToken>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::FrameTokenExhausted, "Frame token space is exhausted."));
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
                if (frameActive_ && frame == activeFrame_) {
                    AbortActiveFrame();
                }
            }

            /** @copydoc IRenderBackend::AbortActiveFrame */
            void AbortActiveFrame() noexcept override {
                frameActive_ = false;
                activeFrame_ = {};
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(const FramebufferExtent extent) override {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!extent.IsValid()) {
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::InvalidExtent, "Renderer output extent must be non-zero."));
                }
                if (frameActive_) {
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::FrameActive, "Renderer output cannot resize while a frame is active."));
                }

                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Shutdown */
            void Shutdown() noexcept override {
                AbortActiveFrame();
                DestroyRemainingResources();
                initialized_ = false;
                DestroyContext();
            }

        private:
            [[nodiscard]] Result<std::uint64_t> ResourceUnavailable(std::string message) const {
                return Result<std::uint64_t>::Failure(
                    MakeOpenGLError(OpenGLBackendErrors::UnsupportedResourceOperation, std::move(message)));
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
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeOpenGLError(OpenGLBackendErrors::FrameTokenMismatch, "Frame token does not match the active frame."));
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
                        return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::InvalidExecutionPlan,
                                                                     "Execution plan contains an invalid render pass ID."));
                    }
                    if (pass.kind != RenderPassKind::Graphics) {
                        return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::UnsupportedPassKind,
                                                                     "Initial OpenGL backend supports graphics passes only."));
                    }
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (pass.id == plan.orderedPasses[previous].id) {
                            return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::InvalidExecutionPlan,
                                                                         "Execution plan contains duplicate render pass IDs."));
                        }
                    }
                    if (!pass.primaryOutput.has_value()) {
                        continue;
                    }

                    if (!pass.primaryOutput->IsValid()) {
                        return Result<void>::Failure(MakeOpenGLError(OpenGLBackendErrors::InvalidExecutionPlan,
                                                                     "Primary output attachment operations are invalid."));
                    }
                }
                return Result<void>::Success();
            }

            void DestroyContext() noexcept {
                if (contextCreated_) {
                    presentationPort_->DestroyContext();
                    contextCreated_ = false;
                }
                ReleaseContextLease();
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
            if (!functions.IsValid() || options.majorVersion == 0) {
                return Result<void>::Failure(
                    MakeOpenGLError(OpenGLBackendErrors::InvalidRegistration, "OpenGL backend registration options are invalid."));
            }
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"opengl"},
                .displayName = "OpenGL",
                .provider = std::make_unique<OpenGLBackendProvider>(presentationPort, options, functions),
            });
        }
    }  // namespace Detail

}  // namespace Horo::Render
