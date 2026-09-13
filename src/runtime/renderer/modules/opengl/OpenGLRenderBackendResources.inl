/** @brief Resource realization methods kept with OpenGLRenderBackend's private definition. */
            /** @copydoc IRenderBackend::QueryBufferMemoryCost */
            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                const auto required = ConservativeRequirement(descriptor.byteSize);
                if (!initialized_ || !functions_.HasResourceFunctions() || !descriptor.IsValid() || !required.has_value())
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
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
                        MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
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
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL buffer creation is unavailable in the current backend state.");
                if (const auto cost = QueryBufferMemoryCost(descriptor);
                    !descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) || cost.HasError() ||
                    !MatchesPlacement(cost.Value(), placement) ||
                    descriptor.byteSize > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL buffer creation request is invalid."));
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
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL mesh creation is unavailable in the current backend state.");
                if (!descriptor.IsValid() || vertexBuffer == 0 || indexBuffer == 0 ||
                    vertexBuffer > std::numeric_limits<std::uint32_t>::max() || indexBuffer > std::numeric_limits<std::uint32_t>::max())
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL mesh creation request is invalid."));
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
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL texture creation is unavailable in the current backend state.");
                const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
                const auto cost = QueryTextureMemoryCost(descriptor);
                if (constexpr auto maximumExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
                    !descriptor.IsValid() || !bytes.has_value() || (!initialData.empty() && initialData.size() != *bytes) ||
                    cost.HasError() || !MatchesPlacement(cost.Value(), placement) || descriptor.extent.width > maximumExtent ||
                    descriptor.extent.height > maximumExtent || descriptor.extent.width > contextFacts_.maxTexture2DSize ||
                    descriptor.extent.height > contextFacts_.maxTexture2DSize)
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture creation request is invalid."));
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
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL texture-view creation is unavailable in the current backend state.");
                if (!descriptor.IsValid() || texture == 0 || texture > std::numeric_limits<std::uint32_t>::max())
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture-view creation request is invalid."));
                if (const auto source = textureFormats_.find(static_cast<std::uint32_t>(texture));
                    source == textureFormats_.end() || source->second != descriptor.format)
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL texture-view format does not match its texture."));
                textureViewFormats_.insert_or_assign(static_cast<std::uint32_t>(texture), descriptor.format);
                return Result<std::uint64_t>::Success(texture);
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) override {
                if (!IsOwnerThread())
                    return WrongThread<std::uint64_t>();
                if (!initialized_ || !functions_.HasResourceFunctions())
                    return ResourceUnavailable("OpenGL render-target creation is unavailable in the current backend state.");
                if (!IsValidRenderTargetRequest(descriptor, colorAttachment, depthAttachment))
                    return Result<std::uint64_t>::Failure(
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL render-target creation request is invalid."));
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
                        MakeError(OpenGLBackendErrors::InvalidConfig, "OpenGL render-target depth view metadata is unavailable."));
                const std::uint32_t attachment =
                    format->second == RenderTextureFormat::Depth24Stencil8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
                functions_.framebuffers.framebufferTexture(GL_FRAMEBUFFER, attachment, static_cast<std::uint32_t>(depthAttachment), 0);
                return Result<void>::Success();
            }
