#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "MetalResourceFormats.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxShaderAbi.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Finite conservative driver-object estimate, never advertised as measured allocation. */
        RenderMemoryCostPlan Estimate(std::uint64_t kind, std::size_t payload, std::size_t overhead) {
            return {.memoryClass = RenderMemoryClass::PersistentDevice,
                    .allocationClass = RenderMemoryAllocationClass::Dedicated,
                    .provenance = RenderMemoryCostProvenance::Estimated,
                    .compatibility = RenderMemoryCompatibilityId{kind},
                    .payloadBytes = payload,
                    .requiredBytes = (payload * 16 + overhead + 255) & ~std::size_t{255},
                    .alignment = 256};
        }

        /** @brief Rejects any placement differing from the exact previously queried estimate. */
        bool Matches(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes == 0;
        }

        /** @brief Supplies explicit native comparison constants without relying on enum ordering. */
        MTLCompareFunction Compare(RenderCompareFunction value) {
            constexpr std::array values{MTLCompareFunctionNever,    MTLCompareFunctionLess,         MTLCompareFunctionLessEqual,
                                        MTLCompareFunctionEqual,    MTLCompareFunctionGreaterEqual, MTLCompareFunctionGreater,
                                        MTLCompareFunctionNotEqual, MTLCompareFunctionAlways};
            return values[static_cast<std::size_t>(value)];
        }

        /** @brief Builds native attachment and fixed MeshVertex state after descriptor admission. */
        void ConfigurePipelineState(MTLRenderPipelineDescriptor *native, const RenderGraphicsPipelineState &state) {
            native.rasterSampleCount = state.sampleCount;
            native.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(MetalPixelFormatValue(state.colorFormat));
            if (state.depthFormat) {
                native.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(MetalPixelFormatValue(*state.depthFormat));
                if (*state.depthFormat == RenderTextureFormat::Depth24Stencil8 ||
                    *state.depthFormat == RenderTextureFormat::Depth32FloatStencil8)
                    native.stencilAttachmentPixelFormat = native.depthAttachmentPixelFormat;
            }
            if (state.blend != RenderBlendMode::Disabled) {
                auto *color = native.colorAttachments[0];
                color.blendingEnabled = YES;
                color.sourceRGBBlendFactor = state.blend == RenderBlendMode::Alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorOne;
                color.destinationRGBBlendFactor =
                    state.blend == RenderBlendMode::Additive ? MTLBlendFactorOne : MTLBlendFactorOneMinusSourceAlpha;
                color.sourceAlphaBlendFactor = MTLBlendFactorOne;
                color.destinationAlphaBlendFactor = color.destinationRGBBlendFactor;
            }
            MTLVertexDescriptor *vertices = [[MTLVertexDescriptor alloc] init];
            constexpr std::array offsets{offsetof(MeshVertex, position), offsetof(MeshVertex, normal), offsetof(MeshVertex, uv)};
            for (NSUInteger index = 0; index < offsets.size(); ++index) {
                vertices.attributes[index].format = index == 2 ? MTLVertexFormatFloat2 : MTLVertexFormatFloat3;
                vertices.attributes[index].offset = offsets[index];
                vertices.attributes[index].bufferIndex = MetalMeshVertexBufferIndex;
            }
            vertices.layouts[MetalMeshVertexBufferIndex].stride = sizeof(MeshVertex);
            vertices.layouts[MetalMeshVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            native.vertexDescriptor = vertices;
        }

        /** @brief Matches cooked vertex inputs to actual native attributes and the fixed MeshVertex layout. */
        Result<void> ValidateVertexInputs(id<MTLFunction> function, const NormalizedShaderReflection &reflection) {
            std::size_t expected = 0;
            for (const auto &input : reflection.stageInterface) {
                if (input.stage != ShaderStage::Vertex || input.direction != ShaderInterfaceDirection::Input)
                    continue;
                ++expected;
                const auto components = input.location == 2 ? 2U : 3U;
                if (input.location > 2 || input.type != ShaderValueType::Float32 || input.arrayCount != 1 || input.rows != 1 ||
                    input.columns != components)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                                  "Metal vertex inputs must use the established MeshVertex locations and shapes."));
                bool found = false;
                for (MTLVertexAttribute *attribute in function.vertexAttributes)
                    if (attribute.active && !attribute.patchData && !attribute.patchControlPointData &&
                        attribute.attributeIndex == input.location &&
                        attribute.attributeType == (components == 2 ? MTLDataTypeFloat2 : MTLDataTypeFloat3))
                        found = true;
                if (!found)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                                           "Cooked vertex input differs from the actual Metal function ABI."));
            }
            std::size_t active = 0;
            for (MTLVertexAttribute *attribute in function.vertexAttributes)
                if (attribute.active)
                    ++active;
            if (active != expected)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::ResourceIdentityInvalid, "Native Metal vertex input is missing from cooked reflection."));
            return Result<void>::Success();
        }

        /** @brief Returns the scalar/vector type for the bounded supported uniform ABI. */
        MTLDataType ParameterType(const ShaderReflectedParameter &parameter) {
            MTLDataType base = MTLDataTypeNone;
            switch (parameter.type) {
                case ShaderValueType::Float32:
                    base = MTLDataTypeFloat;
                    break;
                case ShaderValueType::Int32:
                    base = MTLDataTypeInt;
                    break;
                case ShaderValueType::Uint32:
                    base = MTLDataTypeUInt;
                    break;
                case ShaderValueType::Bool32:
                    return MTLDataTypeNone;
            }
            return static_cast<MTLDataType>(base + std::max(parameter.rows, parameter.columns) - 1);
        }

        /** @brief Matches one native structure member without trusting names from the source compiler. */
        bool HasParameter(MTLStructType *structure, const ShaderReflectedParameter &parameter) {
            const auto type = ParameterType(parameter);
            for (MTLStructMember *member in structure.members)
                if (member.offset == parameter.byteOffset && type != MTLDataTypeNone && member.dataType == type)
                    return true;
            return false;
        }

        /** @brief Verifies native uniform packing and publishes only a complete block size. */
        Result<void> ValidateUniform(id<MTLBufferBinding> buffer, ShaderBindingId binding, const NormalizedShaderReflection &reflection,
                                     std::size_t &size) {
            if (buffer.bufferDataSize == 0 || buffer.bufferDataSize > 65536 || buffer.bufferStructType == nil)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            const bool vfxInstances = IsMetalVfxInstances(binding, reflection);
            if (vfxInstances && buffer.bufferDataSize != 10240)
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                                       "Metal VFX instance block extent differs from its closed schema."));
            std::size_t count = 0;
            for (const auto &parameter : reflection.parameters) {
                if (!parameter.active || parameter.binding != binding)
                    continue;
                ++count;
                if (vfxInstances) {
                    if (!HasMetalVfxArray(buffer.bufferStructType, parameter))
                        return Result<void>::Failure(
                            MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                      "Metal VFX array type, count, offset or stride differs from cooked packing."));
                    continue;
                }
                if (parameter.arrayCount != 1 || parameter.type == ShaderValueType::Bool32 ||
                    (parameter.rows != 1 && parameter.columns != 1))
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                                                           "Metal parameter arrays and matrices require explicit native ABI support."));
                if (!HasParameter(buffer.bufferStructType, parameter))
                    return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                                           "Metal uniform member offset or value shape differs from cooked packing."));
            }
            if (buffer.bufferStructType.members.count != count || (size != 0 && size != buffer.bufferDataSize))
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            size = buffer.bufferDataSize;
            return Result<void>::Success();
        }

        /** @brief Resolves one actual active stage binding by its native namespace and index. */
        id<MTLBinding> FindNativeBinding(NSArray<id<MTLBinding>> *arguments, std::uint32_t index, MTLBindingType type) {
            for (id<MTLBinding> argument in arguments)
                if (argument.used && argument.index == index && argument.type == type)
                    return argument;
            return nil;
        }

        /** @brief Validates a native resource against the supported direct-binding material envelope. */
        Result<void> ValidateArgument(NSArray<id<MTLBinding>> *arguments, const ShaderReflectedBinding &binding,
                                      const ShaderTargetBindingMapEntry &mapping, const NormalizedShaderReflection &reflection,
                                      std::array<std::size_t, 31> &sizes, const ShaderStageVisibility stage) {
            MTLBindingType type;
            switch (binding.kind) {
                case ShaderResourceKind::UniformBuffer:
                    type = MTLBindingTypeBuffer;
                    break;
                case ShaderResourceKind::SampledTexture:
                    type = MTLBindingTypeTexture;
                    break;
                case ShaderResourceKind::Sampler:
                    type = MTLBindingTypeSampler;
                    break;
                default:
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedResourceOperation));
            }
            const id<MTLBinding> actual = FindNativeBinding(arguments, mapping.nativeBinding, type);
            if (actual == nil || actual.access != MTLBindingAccessReadOnly) {
                std::string observed;
                for (id<MTLBinding> argument in arguments)
                    observed += std::format(" [index={}, type={}, access={}, used={}]", argument.index, static_cast<long>(argument.type),
                                            static_cast<long>(argument.access), argument.used);
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                                                       std::format("Metal stage {} logical binding {} requires active native type {} "
                                                                   "at index {} with read-only access; actual bindings:{}.",
                                                                   static_cast<unsigned>(stage), binding.id.value, static_cast<long>(type),
                                                                   mapping.nativeBinding, observed)));
            }
            if (type == MTLBindingTypeBuffer)
                return ValidateUniform((id<MTLBufferBinding>)actual, binding.id, reflection, sizes[mapping.nativeBinding]);
            if (type == MTLBindingTypeTexture) {
                const id<MTLTextureBinding> texture = (id<MTLTextureBinding>)actual;
                if (texture.textureType != MTLTextureType2D || texture.arrayLength != 1 || texture.depthTexture ||
                    texture.textureDataType != MTLDataTypeFloat)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                                                           "Metal material preparation supports noncomparison float 2D textures."));
            }
            return Result<void>::Success();
        }

        /** @brief Admits only the driver's implicit MeshVertex fetch after vertex attribute validation has succeeded. */
        Result<void> ValidateMeshVertexFetch(id<MTLBufferBinding> buffer, const NormalizedShaderReflection &reflection) {
            if (const bool hasInputs = std::ranges::any_of(reflection.stageInterface,
                                                           [](const ShaderReflectedInterfaceVariable &input) {
                return input.stage == ShaderStage::Vertex && input.direction == ShaderInterfaceDirection::Input;
            });
                buffer.argument || buffer.access != MTLBindingAccessReadOnly || buffer.bufferDataType != MTLDataTypeStruct ||
                buffer.bufferDataSize != sizeof(MeshVertex) || buffer.bufferAlignment != alignof(MeshVertex) || !hasInputs)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                              std::format("Metal vertex fetch at slot {} has incompatible argument={}, access={}, type={}, size={}, "
                                          "alignment={}.",
                                          MetalMeshVertexBufferIndex, buffer.argument, static_cast<long>(buffer.access),
                                          static_cast<long>(buffer.bufferDataType), buffer.bufferDataSize, buffer.bufferAlignment)));
            return Result<void>::Success();
        }

        /** @brief Counts material arguments while validating the one reserved implicit geometry fetch separately. */
        Result<std::size_t> CountMaterialArguments(NSArray<id<MTLBinding>> *arguments, ShaderStageVisibility stage,
                                                   const NormalizedShaderReflection &reflection) {
            std::size_t active = 0;
            bool vertexFetchSeen = false;
            for (id<MTLBinding> argument in arguments) {
                if (!argument.used)
                    continue;
                if (stage != ShaderStageVisibility::Vertex || argument.type != MTLBindingTypeBuffer ||
                    argument.index != MetalMeshVertexBufferIndex) {
                    ++active;
                    continue;
                }
                if (vertexFetchSeen)
                    return Result<std::size_t>::Failure(
                        MakeError(MetalBackendErrors::ResourceIdentityInvalid, "Metal exposes duplicate reserved vertex-fetch bindings."));
                if (const auto valid = ValidateMeshVertexFetch((id<MTLBufferBinding>)argument, reflection); valid.HasError())
                    return Result<std::size_t>::Failure(valid.ErrorValue());
                vertexFetchSeen = true;
            }
            return Result<std::size_t>::Success(active);
        }

        /** @brief Checks each stage's complete native active argument set against the cooked logical map. */
        Result<void> ValidateArguments(NSArray<id<MTLBinding>> *arguments, ShaderStageVisibility stage,
                                       const NormalizedShaderReflection &reflection, std::array<std::size_t, 31> &sizes) {
            std::size_t expected = 0;
            for (const auto &binding : reflection.bindings) {
                if (!binding.active || (static_cast<unsigned>(binding.stages) & static_cast<unsigned>(stage)) == 0)
                    continue;
                ++expected;
                const auto mapping = std::ranges::find_if(reflection.targetBindings, [&](const ShaderTargetBindingMapEntry &entry) {
                    return entry.id == binding.id && entry.active && entry.generatedHelperIndex == 0;
                });
                if (mapping == reflection.targetBindings.end() || mapping->nativeSpace != 0 || mapping->nativeBinding >= 31 ||
                    binding.arrayCount != 1 ||
                    (binding.kind == ShaderResourceKind::UniformBuffer && mapping->nativeBinding == MetalMeshVertexBufferIndex))
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                                  "Metal requires direct scalar arguments below slot 31; buffer slot 30 is reserved for MeshVertex."));
                if (const auto validated = ValidateArgument(arguments, binding, *mapping, reflection, sizes, stage); validated.HasError())
                    return validated;
            }
            const auto active = CountMaterialArguments(arguments, stage, reflection);
            if (active.HasError())
                return Result<void>::Failure(active.ErrorValue());
            if (active.Value() != expected)
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                              std::format("Metal stage {} has {} active material arguments; cooked reflection requires {}.",
                                          static_cast<unsigned>(stage), active.Value(), expected)));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::QueryPipelineDeviceIdentity */
    Result<RenderPipelineDeviceIdentity> MetalResourceRuntime::QueryPipelineDeviceIdentity() const {
        if (impl_->device == nil)
            return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(MetalBackendErrors::NotInitialized));
        const std::string version = NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String;
        const std::string name = impl_->device.name.UTF8String;
        const std::string adapter = std::format("metal:{:016x}", static_cast<std::uint64_t>(impl_->device.registryID));
        Sha256Builder hash;
        for (const auto &fact : {adapter, name, version}) {
            static_cast<void>(hash.Update(std::as_bytes(std::span{fact.data(), fact.size()})));
            constexpr std::array<std::byte, 1> separator{};
            static_cast<void>(hash.Update(separator));
        }
        return Result<RenderPipelineDeviceIdentity>::Success({.backend = RenderBackendId{"metal"},
                                                              .backendModuleVersion = 1,
                                                              .adapter = RenderAdapterId{adapter},
                                                              .deviceCompatibilityDigest = hash.Finalize(),
                                                              .driverIdentity = name,
                                                              .driverVersion = version});
    }

    /** @copydoc MetalResourceRuntime::QueryShaderModuleMemoryCost */
    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &descriptor,
                                                                                   std::size_t cookedBytes) const {
        if (const auto valid = ValidateRenderShaderModuleDescriptor(descriptor); valid.HasError())
            return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
        if (impl_->device == nil)
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::NotInitialized));
        if (descriptor.target.backend != ShaderTargetBackend::Metal ||
            descriptor.target.payloadFormat != ShaderPayloadFormat::MetalLibrary24 || descriptor.target.descriptorVersion != 1 ||
            descriptor.target.interfaceSchemaVersion != 1 ||
            (descriptor.stage != ShaderStage::Vertex && descriptor.stage != ShaderStage::Fragment) || cookedBytes < 4 ||
            cookedBytes > 64U * 1024U * 1024U || impl_->shaderModules.size() >= Impl::MaxResidentInstances)
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                          "Metal requires finite offline metallib vertex/fragment stages and version-one metadata."));
        return Result<RenderMemoryCostPlan>::Success(Estimate(30, cookedBytes, 65536));
    }

    /** @copydoc MetalResourceRuntime::CreateShaderModule */
    Result<std::uint64_t> MetalResourceRuntime::CreateShaderModule(const RenderShaderModuleDescriptor &descriptor,
                                                                   std::span<const std::uint8_t> code,
                                                                   const RenderMemoryPlacement &placement) {
        const auto cost = QueryShaderModuleMemoryCost(descriptor, code.size());
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!Matches(cost.Value(), placement) || std::memcmp(code.data(), "MTLB", 4) != 0)
            return Result<std::uint64_t>::Failure(MakeError(MetalBackendErrors::InvalidConfig));
        dispatch_data_t data = dispatch_data_create(code.data(), code.size(), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = data ? [impl_->device newLibraryWithData:data error:&error] : nil;
        NSString *entry = [[NSString alloc] initWithBytes:descriptor.entryPoint.data()
                                                   length:descriptor.entryPoint.size()
                                                 encoding:NSUTF8StringEncoding];
        id<MTLFunction> function = library ? [library newFunctionWithName:entry] : nil;
        const auto expected = descriptor.stage == ShaderStage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
        if (function == nil || function.functionType != expected)
            return Result<std::uint64_t>::Failure(
                MakeError(MetalBackendErrors::ResourceCreationFailed,
                          "Metal rejected the offline library or its exact stage entry; recook for this device/toolchain."));
        auto candidate = std::make_unique<MetalShaderModule>();
        candidate->function = function;
        candidate->descriptor = descriptor;
        candidate->cookedBytes = code.size();
        const auto tracked = impl_->TrackInstance(candidate.release(), impl_->shaderModules, "Metal shader metadata allocation failed.");
        return tracked;
    }

    /** @copydoc MetalResourceRuntime::QueryGraphicsPipelineMemoryCost */
    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                                       std::uint64_t vertex, std::uint64_t fragment) const {
        if (const auto valid = ValidateRenderGraphicsPipelineDescriptor(descriptor); valid.HasError())
            return Result<RenderMemoryCostPlan>::Failure(valid.ErrorValue());
        auto *v = Decode<MetalShaderModule>(vertex);
        auto *f = Decode<MetalShaderModule>(fragment);
        if (!impl_->shaderModules.contains(v) || !impl_->shaderModules.contains(f))
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto matches = [&](const MetalShaderModule &module, ShaderStage stage) {
            return module.descriptor.stage == stage && SameMetalShaderTarget(module.descriptor.target, descriptor.target) &&
                   module.descriptor.artifactKey == descriptor.compatibility.shaderArtifactKey &&
                   module.descriptor.shaderInterface == descriptor.reflection.interfaceCompatibility &&
                   std::ranges::any_of(descriptor.manifest.entryPoints, [&](const auto &entry) {
                return entry.stage == stage && entry.name == module.descriptor.entryPoint;
            });
        };
        if (!matches(*v, ShaderStage::Vertex) || !matches(*f, ShaderStage::Fragment))
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto actual = QueryPipelineDeviceIdentity();
        if (actual.HasError())
            return Result<RenderMemoryCostPlan>::Failure(actual.ErrorValue());
        const auto &a = actual.Value();
        const auto &e = descriptor.compatibility;
        if (a.backend != e.backend || a.adapter != e.adapter || a.backendModuleVersion != e.backendModuleVersion ||
            a.deviceCompatibilityDigest != e.deviceCompatibilityDigest || a.driverIdentity != e.driverIdentity ||
            a.driverVersion != e.driverVersion)
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(MetalBackendErrors::ResourceIdentityInvalid,
                          "Pipeline cache compatibility does not identify the selected Metal device."));
        if (std::ranges::any_of(descriptor.reflection.bindings, [](const ShaderReflectedBinding &binding) {
            return binding.active && (static_cast<unsigned>(binding.stages) & static_cast<unsigned>(ShaderStageVisibility::Compute)) != 0;
        }))
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                          "Metal graphics material preparation does not admit compute-stage resource visibility."));
        if (std::ranges::any_of(descriptor.reflection.targetBindings, [](const ShaderTargetBindingMapEntry &entry) {
            return entry.generatedHelperIndex != 0;
        }))
            return Result<RenderMemoryCostPlan>::Failure(
                MakeError(MetalBackendErrors::UnsupportedResourceOperation,
                          "Generated Metal helper bindings require a separately implemented native material ABI."));
        if (descriptor.target.backend != ShaderTargetBackend::Metal || descriptor.state.viewRouting != RenderViewRouting::SingleView ||
            descriptor.state.sampleCount != 1 || !descriptor.manifest.inlineConstants.empty() ||
            !descriptor.manifest.specializationInputs.empty() || impl_->graphicsPipelines.size() >= Impl::MaxResidentInstances)
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::UnsupportedResourceOperation));
        return Result<RenderMemoryCostPlan>::Success(Estimate(31, v->cookedBytes + f->cookedBytes, 131072));
    }

    /** @copydoc MetalResourceRuntime::CreateGraphicsPipeline */
    Result<std::uint64_t> MetalResourceRuntime::CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                       std::uint64_t vertex, std::uint64_t fragment,
                                                                       const RenderMemoryPlacement &placement) {
        const auto cost = QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!Matches(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(MakeError(MetalBackendErrors::InvalidConfig));
        const auto layout = PrepareMaterialBindingLayout(descriptor.reflection);
        if (layout.HasError())
            return Result<std::uint64_t>::Failure(layout.ErrorValue());
        const auto key = ValidateRenderGraphicsPipelineDescriptor(descriptor);
        if (key.HasError())
            return Result<std::uint64_t>::Failure(key.ErrorValue());
        MTLRenderPipelineDescriptor *native = [[MTLRenderPipelineDescriptor alloc] init];
        native.vertexFunction = Decode<MetalShaderModule>(vertex)->function;
        native.fragmentFunction = Decode<MetalShaderModule>(fragment)->function;
        if (const auto vertexInputs = ValidateVertexInputs(native.vertexFunction, descriptor.reflection); vertexInputs.HasError())
            return Result<std::uint64_t>::Failure(vertexInputs.ErrorValue());
        ConfigurePipelineState(native, descriptor.state);
        auto candidate = std::make_unique<MetalGraphicsPipeline>();
        NSError *error = nil;
        MTLRenderPipelineReflection *reflection = nil;
        candidate->pipeline =
            [impl_->device newRenderPipelineStateWithDescriptor:native
                                                        options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                     reflection:&reflection
                                                          error:&error];
        if (candidate->pipeline == nil || reflection == nil)
            return Result<std::uint64_t>::Failure(
                MakeError(MetalBackendErrors::ResourceCreationFailed,
                          "Metal rejected the cooked graphics pipeline; verify stage interfaces and attachment format support."));
        for (const auto stage : {ShaderStageVisibility::Vertex, ShaderStageVisibility::Fragment}) {
            const auto validated =
                ValidateArguments(stage == ShaderStageVisibility::Vertex ? reflection.vertexBindings : reflection.fragmentBindings, stage,
                                  descriptor.reflection, candidate->bufferSizes);
            if (validated.HasError())
                return Result<std::uint64_t>::Failure(validated.ErrorValue());
        }
        MTLDepthStencilDescriptor *depth = [[MTLDepthStencilDescriptor alloc] init];
        depth.depthCompareFunction = descriptor.state.depthTest ? Compare(descriptor.state.depthCompare) : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = descriptor.state.depthWrite;
        candidate->depth = [impl_->device newDepthStencilStateWithDescriptor:depth];
        if (candidate->depth == nil)
            return Result<std::uint64_t>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
        candidate->layout = layout.Value();
        candidate->state = descriptor.state;
        candidate->key = key.Value();
        return impl_->TrackInstance(candidate.release(), impl_->graphicsPipelines, "Metal pipeline metadata allocation failed.");
    }

    /** @copydoc MetalResourceRuntime::QuerySamplerMemoryCost */
    Result<RenderMemoryCostPlan> MetalResourceRuntime::QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const {
        if (impl_->device == nil)
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::NotInitialized));
        if (!descriptor.IsValid() || descriptor.maximumAnisotropy > 16 ||
            std::floor(descriptor.maximumAnisotropy) != descriptor.maximumAnisotropy ||
            impl_->samplers.size() >= Impl::MaxResidentInstances)
            return Result<RenderMemoryCostPlan>::Failure(MakeError(MetalBackendErrors::UnsupportedResourceOperation));
        return Result<RenderMemoryCostPlan>::Success(Estimate(32, 256, 0));
    }

    /** @copydoc MetalResourceRuntime::CreateSampler */
    Result<std::uint64_t> MetalResourceRuntime::CreateSampler(const RenderSamplerDescriptor &descriptor,
                                                              const RenderMemoryPlacement &placement) {
        const auto cost = QuerySamplerMemoryCost(descriptor);
        if (cost.HasError())
            return Result<std::uint64_t>::Failure(cost.ErrorValue());
        if (!Matches(cost.Value(), placement))
            return Result<std::uint64_t>::Failure(MakeError(MetalBackendErrors::InvalidConfig));
        MTLSamplerDescriptor *native = [[MTLSamplerDescriptor alloc] init];
        native.minFilter =
            descriptor.minFilter == RenderSamplerFilter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        native.magFilter =
            descriptor.magFilter == RenderSamplerFilter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        native.mipFilter =
            descriptor.mipmapMode == RenderSamplerMipmapMode::Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        constexpr std::array addresses{MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge,
                                       MTLSamplerAddressModeClampToBorderColor};
        native.sAddressMode = addresses[static_cast<std::size_t>(descriptor.addressU)];
        native.tAddressMode = addresses[static_cast<std::size_t>(descriptor.addressV)];
        native.rAddressMode = addresses[static_cast<std::size_t>(descriptor.addressW)];
        native.lodMinClamp = descriptor.minimumLod;
        native.lodMaxClamp = descriptor.maximumLod;
        native.maxAnisotropy = static_cast<NSUInteger>(descriptor.maximumAnisotropy);
        native.compareFunction = descriptor.compareEnabled ? Compare(descriptor.compare) : MTLCompareFunctionNever;
        auto candidate = std::make_unique<MetalSamplerInstance>();
        candidate->sampler = [impl_->device newSamplerStateWithDescriptor:native];
        candidate->descriptor = descriptor;
        if (candidate->sampler == nil)
            return Result<std::uint64_t>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
        return impl_->TrackInstance(candidate.release(), impl_->samplers, "Metal sampler metadata allocation failed.");
    }

    /** @copydoc MetalResourceRuntime::DestroyShaderModule */
    void MetalResourceRuntime::DestroyShaderModule(std::uint64_t identity) noexcept {
        auto *native = Decode<MetalShaderModule>(identity);
        if (impl_->shaderModules.erase(native))
            delete native;
    }

    /** @copydoc MetalResourceRuntime::DestroyGraphicsPipeline */
    void MetalResourceRuntime::DestroyGraphicsPipeline(std::uint64_t identity) noexcept {
        auto *native = Decode<MetalGraphicsPipeline>(identity);
        if (impl_->graphicsPipelines.erase(native))
            delete native;
    }

    /** @copydoc MetalResourceRuntime::DestroySampler */
    void MetalResourceRuntime::DestroySampler(std::uint64_t identity) noexcept {
        auto *native = Decode<MetalSamplerInstance>(identity);
        if (impl_->samplers.erase(native))
            delete native;
    }
}  // namespace Horo::Render::Detail
