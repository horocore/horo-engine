#include "MetalClusteredMaterialGraphFixture.h"

#include "Horo/Runtime/Render/MaterialBindingErrors.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Test {
    namespace {
        using namespace Detail;

        /** @brief Preserves original native failures in the actual qualification assertion. */
        template <typename T> T RequireValue(Result<T> result) {
            INFO(
                (result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Native operation accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Converts only an identity actually issued by this fixture's canonical resource registry. */
        template <typename Handle> Handle IssuedHandle(const RenderResourceIdentity identity) {
            return {identity.owner, identity.slot, identity.generation};
        }

        /** @brief Projects an issued typed handle back into the same registry's lookup identity. */
        template <typename Handle> RenderResourceIdentity Identity(const Handle handle) {
            return {handle.owner, handle.slot, handle.generation};
        }

        /** @brief Owns the actual native table and the exact registry pins it resolved. */
        class QualifiedMaterial final : public IResidentMaterialBinding {
        public:
            RenderResourceRegistry *registry{};
            std::unique_ptr<IResidentMaterialBinding> native;
            std::vector<std::pair<RenderResourceClass, RenderResourceIdentity>> pins;

            ~QualifiedMaterial() override {
                native.reset();
                for (const auto &[kind, id] : pins)
                    static_cast<void>(registry->ReleaseSubmissionPin(kind, id));
            }

            const IResidentMaterialBinding &NativeBinding() const noexcept override {
                return native->NativeBinding();
            }

            Result<void> Pin(const RenderResourceClass kind, const RenderResourceIdentity id) {
                if (auto pinned = registry->AddSubmissionPin(kind, id); pinned.HasError())
                    return pinned;
                pins.emplace_back(kind, id);
                return Result<void>::Success();
            }
        };

        struct QualifiedInput final {
            RenderResourceClass kind;
            RenderResourceIdentity identity;
            MaterialResidentResource native;
        };

        /** @brief Resolves actual issued logical inputs; implicit buffers and unregistered handles are rejected. */
        Result<QualifiedInput> ResolveInput(RenderResourceRegistry &registry, const MaterialBindingValue &value) {
            QualifiedInput result;
            if (const auto *buffer = std::get_if<MaterialBufferBinding>(&value)) {
                result = {RenderResourceClass::Buffer,
                          Identity(buffer->buffer),
                          {MaterialResidentResourceKind::Buffer, 0, buffer->offset, buffer->byteCount}};
            } else if (const auto *view = std::get_if<RenderTextureViewHandle>(&value)) {
                result = {RenderResourceClass::TextureView, Identity(*view), {MaterialResidentResourceKind::TextureView}};
            } else if (const auto *sampler = std::get_if<RenderSamplerHandle>(&value)) {
                result = {RenderResourceClass::Sampler, Identity(*sampler), {MaterialResidentResourceKind::Sampler}};
            } else {
                return Result<QualifiedInput>::Failure(MakeError(MaterialBindingErrors::Unsupported));
            }
            const auto resolved = registry.BackendInstance(result.kind, result.identity);
            if (resolved.HasError())
                return Result<QualifiedInput>::Failure(resolved.ErrorValue());
            result.native.instance = resolved.Value();
            return Result<QualifiedInput>::Success(result);
        }
    }  // namespace

    Result<void> ClusteredQualificationPort::CreateSurface() {
        layer_ = [CAMetalLayer layer];
        return layer_ ? Result<void>::Success() : Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
    }

    void *ClusteredQualificationPort::Layer() const noexcept {
        return (__bridge void *)layer_;
    }

    void ClusteredQualificationPort::DestroySurface() noexcept {
        layer_ = nil;
    }

    Result<std::unique_ptr<IResidentMaterialBinding>> ClusteredQualificationMaterials::Realize(
        const MaterialBindingDescriptor &descriptor) {
        using Outcome = Result<std::unique_ptr<IResidentMaterialBinding>>;
        const auto pipeline = registry->BackendInstance(Detail::RenderResourceClass::Pipeline, Identity(descriptor.pipeline));
        if (pipeline.HasError())
            return Outcome::Failure(pipeline.ErrorValue());
        auto owned = std::make_unique<QualifiedMaterial>();
        owned->registry = registry;
        if (auto pin = owned->Pin(Detail::RenderResourceClass::Pipeline, Identity(descriptor.pipeline)); pin.HasError())
            return Outcome::Failure(pin.ErrorValue());
        std::vector<MaterialResidentResource> inputs;
        for (const auto &logical : descriptor.resources) {
            const auto resolved = ResolveInput(*registry, logical.value);
            if (resolved.HasError())
                return Outcome::Failure(resolved.ErrorValue());
            if (auto pin = owned->Pin(resolved.Value().kind, resolved.Value().identity); pin.HasError())
                return Outcome::Failure(pin.ErrorValue());
            inputs.push_back(resolved.Value().native);
        }
        auto native = runtime->RealizeResolved(descriptor, pipeline.Value(), inputs);
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        owned->native = std::move(native).Value();
        return Outcome::Success(std::move(owned));
    }

    void MetalClusteredMaterialGraphFixture::CreateOwners() {
        REQUIRE(RegisterMetalRenderBackend(backends_, port_, bridge_).HasValue());
        REQUIRE(backends_.Seal().HasValue());
        frontend_ = RequireValue(
            RenderFrontend::Create(backends_, RenderBackendId{"metal"}, {.requirePresentation = false, .maxFramesInFlight = 2}));
        productionGraphCapability_ = frontend_->Capabilities().supportsMaterialRasterGraph;
        productionBatchCapability_ = frontend_->Capabilities().supportsMaterialPassBatches;
        INFO("Unmodified production graph capability: " << productionGraphCapability_);
        REQUIRE(bridge_.Device());
        REQUIRE(bridge_.CommandQueue());
        const auto incarnation = frontend_->Capabilities().support.deviceIncarnation;
        REQUIRE(incarnation != 0);
        runtime_.Initialize(bridge_.Device(), bridge_.CommandQueue(), incarnation);
        const auto owner = RequireValue(Detail::AcquireRenderResourceOwnerId());
        memory_ = RequireValue(RenderMemoryBudget::Create(owner, {.hardCapBytes = 32U * 1024U * 1024U,
                                                                  .defaultBlockBytes = 1U * 1024U * 1024U,
                                                                  .maximumBlockBytes = 8U * 1024U * 1024U}));
        registry_ =
            std::make_unique<Detail::RenderResourceRegistry>(owner, Detail::RenderResourceRegistryLimits{},
                                                             [this](const auto kind, const auto native, const auto, const auto mode) {
            Destroy(kind, native, mode);
        }, memory_.get());
        adapter_.registry = registry_.get();
        adapter_.runtime = &runtime_;
        materials_ = RequireValue(MaterialBindingTable::Create(owner, adapter_, {.maximumGenerations = 8}));
    }

    MetalClusteredMaterialGraphFixture::Registered MetalClusteredMaterialGraphFixture::Reserve(
        const Detail::RenderResourceClass kind, const std::span<const Detail::RenderResourceIdentity> dependencies,
        const std::optional<RenderMemoryCostPlan> &cost, const bool nativeLifetime) {
        Registered record{.kind = kind,
                          .reservation = RequireValue(registry_->Reserve(kind, dependencies)),
                          .memory = std::nullopt,
                          .placement = {},
                          .lifetime = {}};
        if (cost) {
            record.memory = RequireValue(memory_->Reserve({382, 1}, record.reservation.operation, *cost));
            record.placement = RequireValue(memory_->Placement(*record.memory));
            if (nativeLifetime)
                record.lifetime = RequireValue(registry_->PrepareNativeLifetime(kind, record.reservation.identity, *record.memory));
        }
        return record;
    }

    Detail::RenderResourceIdentity MetalClusteredMaterialGraphFixture::Publish(const Registered &record, const std::uint64_t native) {
        REQUIRE(registry_->AttachNativeCreation(record.kind, record.reservation.identity, native).HasValue());
        std::optional<RenderMemoryAllocationId> allocation;
        if (record.memory)
            allocation = RequireValue(memory_->Commit(*record.memory)).id;
        REQUIRE(registry_->Publish(record.kind, record.reservation.identity, native, allocation).HasValue());
        return record.reservation.identity;
    }

    std::uint64_t MetalClusteredMaterialGraphFixture::Native(const Detail::RenderResourceClass kind,
                                                             const Detail::RenderResourceIdentity identity) const {
        return RequireValue(registry_->BackendInstance(kind, identity));
    }

    void MetalClusteredMaterialGraphFixture::Destroy(const Detail::RenderResourceClass kind, const std::uint64_t native,
                                                     const Detail::BackendResourceReleaseMode mode) noexcept {
        if (mode != Detail::BackendResourceReleaseMode::DestroyNative)
            return;
        switch (kind) {
            case Detail::RenderResourceClass::Buffer:
                runtime_.DestroyBuffer(native);
                break;
            case Detail::RenderResourceClass::Texture:
                runtime_.DestroyTexture(native);
                break;
            case Detail::RenderResourceClass::TextureView:
                runtime_.DestroyTextureView(native);
                break;
            case Detail::RenderResourceClass::Sampler:
                runtime_.DestroySampler(native);
                break;
            case Detail::RenderResourceClass::ShaderModule:
                runtime_.DestroyShaderModule(native);
                break;
            case Detail::RenderResourceClass::Pipeline:
                runtime_.DestroyGraphicsPipeline(native);
                break;
            case Detail::RenderResourceClass::Mesh:
                runtime_.DestroyMesh(native);
                break;
            case Detail::RenderResourceClass::RenderTarget:
                runtime_.DestroyRenderTarget(native);
                break;
        }
    }

    RenderBufferHandle MetalClusteredMaterialGraphFixture::Buffer(const RenderBufferDescriptor &descriptor,
                                                                  const std::span<const std::byte> bytes) {
        const auto cost = RequireValue(runtime_.QueryBufferMemoryCost(descriptor));
        auto record = Reserve(Detail::RenderResourceClass::Buffer, {}, cost, true);
        // The native creation owns the single lease on success and releases it on rollback.
        const auto native = RequireValue(runtime_.CreateBuffer(descriptor, bytes, record.placement, std::move(record.lifetime)));
        return IssuedHandle<RenderBufferHandle>(Publish(record, native));
    }

    RenderTextureHandle MetalClusteredMaterialGraphFixture::Texture(const RenderTextureDescriptor &descriptor) {
        const auto cost = RequireValue(runtime_.QueryTextureMemoryCost(descriptor));
        auto record = Reserve(Detail::RenderResourceClass::Texture, {}, cost, true);
        const auto native = RequireValue(runtime_.CreateTexture(descriptor, {}, record.placement, std::move(record.lifetime)));
        return IssuedHandle<RenderTextureHandle>(Publish(record, native));
    }

    RenderTextureViewHandle MetalClusteredMaterialGraphFixture::View(const RenderTextureHandle texture, const RenderTextureFormat format,
                                                                     const RenderTextureAspect aspect) {
        const std::array dependencies{Identity(texture)};
        const auto record = Reserve(Detail::RenderResourceClass::TextureView, dependencies);
        const auto native = RequireValue(runtime_.CreateTextureView({.texture = texture, .format = format, .aspect = aspect},
                                                                    Native(Detail::RenderResourceClass::Texture, Identity(texture))));
        return IssuedHandle<RenderTextureViewHandle>(Publish(record, native));
    }

    RenderShaderModuleHandle MetalClusteredMaterialGraphFixture::Stage(const ShaderHotReloadProgramView &view, const ShaderStage stage,
                                                                       const char *entry) {
        const auto &artifact = view.compiled->artifacts.front();
        const auto &metadata = *artifact.reflection;
        const RenderShaderModuleDescriptor descriptor{view.manifest->targets.front(), stage, entry, metadata.cookedArtifactKey,
                                                      metadata.reflection.interfaceCompatibility};
        INFO("Preparing genuine packaged Metal stage " << static_cast<int>(stage) << " entry " << entry);
        const auto decoded = RequireValue(
            DecodeShaderArtifactStages(artifact.payload, descriptor.target.backend, descriptor.target.payloadFormat, 64U * 1024U * 1024U));
        const auto stages = std::span{decoded.stages}.first(decoded.stageCount);
        const auto selected = std::ranges::find_if(stages, [&](const ShaderArtifactStageView &candidate) {
            return candidate.stage == stage && candidate.entryPoint == entry;
        });
        REQUIRE(selected != stages.end());
        const auto record = Reserve(Detail::RenderResourceClass::ShaderModule, {},
                                    RequireValue(runtime_.QueryShaderModuleMemoryCost(descriptor, selected->bytes.size())));
        const auto native = RequireValue(runtime_.CreateShaderModule(descriptor, selected->bytes, record.placement));
        return IssuedHandle<RenderShaderModuleHandle>(Publish(record, native));
    }

    void MetalClusteredMaterialGraphFixture::PrepareProgram(const ShaderHotReloadProgramView &view, Program &program,
                                                            const bool depthOnly) {
        program.vertex = Stage(view, ShaderStage::Vertex, "VSMain");
        program.fragment = Stage(view, ShaderStage::Fragment, "PSMain");
        const auto vertex = Native(Detail::RenderResourceClass::ShaderModule, Identity(program.vertex));
        const auto fragment = Native(Detail::RenderResourceClass::ShaderModule, Identity(program.fragment));
        const auto &metadata = *view.compiled->artifacts.front().reflection;
        const auto device = RequireValue(runtime_.QueryPipelineDeviceIdentity());
        const auto &target = view.manifest->targets.front();
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                .depthFormat = RenderTextureFormat::Depth32Float,
                                                .depthOnly = depthOnly};
        const PipelineCacheCompatibility compatibility{1,
                                                       device.backend,
                                                       device.backendModuleVersion,
                                                       device.adapter,
                                                       device.deviceCompatibilityDigest,
                                                       device.driverIdentity,
                                                       device.driverVersion,
                                                       target.backend,
                                                       target.payloadFormat,
                                                       metadata.cookedArtifactKey,
                                                       metadata.reflection.interfaceCompatibility,
                                                       RequireValue(ComputeRenderGraphicsPipelineStateDigest(state))};
        const RenderGraphicsPipelineDescriptor descriptor{program.vertex, program.fragment, *view.manifest, target, metadata.reflection,
                                                          state,          compatibility};
        const std::array dependencies{Identity(program.vertex), Identity(program.fragment)};
        const auto record = Reserve(Detail::RenderResourceClass::Pipeline, dependencies,
                                    RequireValue(runtime_.QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment)));
        const auto native = RequireValue(runtime_.CreateGraphicsPipeline(descriptor, vertex, fragment, record.placement));
        program.pipeline = IssuedHandle<RenderPipelineHandle>(Publish(record, native));
        program.layout = RequireValue(PrepareMaterialBindingLayout(metadata.reflection));
        program.uniformBytes =
            RequireValue(runtime_.QueryMaterialParameterBufferSize(native, {.pipeline = program.pipeline, .layout = program.layout}, {1}));
    }

    void MetalClusteredMaterialGraphFixture::PreparePrograms(const Editor::EditorClusteredLightingAssets &assets) {
        const auto family = assets.ProgramViews();
        PrepareProgram(family[0], programs_[0], false);
        PrepareProgram(family[2], programs_[1], true);
        kernel_ = RequireValue(runtime_.RealizeLightCullingKernel(assets.kernel));
        const auto record = Reserve(Detail::RenderResourceClass::Sampler, {}, RequireValue(runtime_.QuerySamplerMemoryCost({})));
        sampler_ = IssuedHandle<RenderSamplerHandle>(Publish(record, RequireValue(runtime_.CreateSampler({}, record.placement))));
    }
}  // namespace Horo::Render::Test
