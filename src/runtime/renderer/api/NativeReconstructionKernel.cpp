#include "Horo/Runtime/Render/NativeReconstructionKernel.h"

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "RenderErrorDescriptor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Horo::Render::NativeReconstructionErrors {
    namespace {
        const ErrorDomainId Domain{"render.native_reconstruction"};
    }

    const ErrorCodeDescriptor InvalidDispatch =
        Detail::MakeErrorDescriptor(Domain, "render.native_reconstruction.dispatch_invalid", ErrorSeverity::Error,
                                    "Native reconstruction dispatch metadata is invalid.",
                                    "Use the exact pending temporal history attempt and finite blend and depth policy.");
    const ErrorCodeDescriptor Unsupported =
        Detail::MakeErrorDescriptor(Domain, "render.native_reconstruction.unsupported", ErrorSeverity::Error,
                                    "The selected native reconstruction kernel cannot admit these inputs.",
                                    "Resolve a qualified shader variant and compatible graph resources before dispatch.");
    const ErrorCodeDescriptor InvalidArtifact =
        Detail::MakeErrorDescriptor(Domain, "render.native_reconstruction.artifact_invalid", ErrorSeverity::Error,
                                    "The cooked native reconstruction artifact has incompatible reflection or stage identity.",
                                    "Recook the expected compute entry and bound final-target reflection together.");
}  // namespace Horo::Render::NativeReconstructionErrors

namespace Horo::Render {
    static_assert(sizeof(NativeReconstructionDispatch) == 48);
    static_assert(offsetof(NativeReconstructionDispatch, sample) == 16 && offsetof(NativeReconstructionDispatch, depth) == 32);

    namespace {
        /** @brief Supports the exact storage image and sampled-resource variant defined by this shader. */
        bool SupportsDispatch(const ReconstructionFrameRequest &frame, const RenderCapabilitySnapshot &capabilities) noexcept {
            const auto output = frame.output.descriptor;
            constexpr auto samplerLimit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 2);
            const auto fitsTexelAddress = [](FramebufferExtent extent) {
                return extent.width <= samplerLimit && extent.height <= samplerLimit;
            };
            const RenderTextureDescriptor depthOutput{.extent = frame.compatibility.targetExtent,
                                                      .format = RenderTextureFormat::R32Float,
                                                      .usage = RenderTextureUsage::Storage | RenderTextureUsage::Sampled};
            return capabilities.features.Supports(RenderCapability::Compute) && capabilities.queues.compute &&
                   output.format == RenderTextureFormat::Rgba32Float && frame.output.writeUsage == RenderGraphUsageKind::Storage &&
                   HasTextureUsage(output.usage, RenderTextureUsage::Sampled) &&
                   (frame.kind != ReconstructionKind::Temporal || capabilities.Supports(depthOutput)) &&
                   fitsTexelAddress(frame.compatibility.renderExtent) && fitsTexelAddress(frame.compatibility.targetExtent) &&
                   std::holds_alternative<ReconstructionExposureScales>(frame.exposure) && !frame.masks.reactive &&
                   !frame.masks.transparency && !frame.masks.disocclusion && !frame.masks.composition;
        }

        /** @brief Checks the declared tuning and exact frame/history correlation without modifying either owner. */
        bool ValidTemporal(const ReconstructionFrameRequest &frame, const NativeReconstructionTemporalSettings &tuning,
                           const TemporalHistoryFrame &history) noexcept {
            if (frame.compatibility != history.compatibility || frame.timing.frameId != history.frameId ||
                (frame.reset && history.resetCause != frame.reset) || !std::ranges::all_of(history.resources, [&](const auto &resource) {
                return resource.owner == frame.resourceOwner;
            }))
                return false;
            return std::isfinite(tuning.historyWeight) && tuning.historyWeight >= 0.0F && tuning.historyWeight < 1.0F &&
                   std::isfinite(tuning.absoluteDepthTolerance) && tuning.absoluteDepthTolerance >= 0.0F &&
                   std::isfinite(tuning.relativeDepthTolerance) && tuning.relativeDepthTolerance >= 0.0F &&
                   tuning.relativeDepthTolerance <= 1.0F;
        }

        /** @brief One logical binding and authored source name in fixed register order. */
        struct KernelBinding final {
            ShaderResourceKind kind;
            std::string_view name;
            NativeReconstructionEntry minimum{NativeReconstructionEntry::Spatial};
        };
        using enum ShaderResourceKind;
        using enum NativeReconstructionEntry;
        constexpr std::array KernelBindings{
            KernelBinding{SampledTexture, "CurrentColor"},
            KernelBinding{StorageTexture, "ReconstructedColor"},
            KernelBinding{SampledTexture, "CurrentDepth", TemporalSeed},
            KernelBinding{SampledTexture, "Motion", TemporalHistory},
            KernelBinding{SampledTexture, "PreviousColor", TemporalHistory},
            KernelBinding{SampledTexture, "PreviousDepth", TemporalHistory},
            KernelBinding{StorageTexture, "ReconstructedDepth", TemporalSeed},
        };

        /** @brief Chooses a seed when valid prior color and depth are intentionally unused. */
        NativeReconstructionEntry SelectEntry(bool temporal, bool readsHistory) noexcept {
            if (!temporal)
                return Spatial;
            return readsHistory ? TemporalHistory : TemporalSeed;
        }

        /** @brief Verifies active final ABI and the exact Vulkan descriptor slots. */
        bool ExactLayout(const NormalizedShaderReflection &reflected) noexcept {
            if (!reflected.parameters.empty() || !reflected.stageInterface.empty() ||
                reflected.targetBindings.size() != reflected.bindings.size())
                return false;
            return std::ranges::all_of(reflected.bindings, &ShaderReflectedBinding::active) &&
                   std::ranges::all_of(reflected.targetBindings, [](const auto &binding) {
                return binding.active && binding.nativeSpace == 0 && binding.nativeBinding == binding.id.value - 1 &&
                       binding.generatedHelperIndex == 0;
            });
        }
    }  // namespace

    /** @copydoc NativeReconstructionEntryName */
    std::string_view NativeReconstructionEntryName(NativeReconstructionEntry entry) noexcept {
        switch (entry) {
            case Spatial:
                return "ReconstructSpatial";
            case TemporalSeed:
                return "ReconstructTemporalSeed";
            case TemporalHistory:
                return "ReconstructTemporal";
        }
        return {};
    }

    /** @copydoc NativeReconstructionUsesImage */
    bool NativeReconstructionUsesImage(NativeReconstructionEntry entry, NativeReconstructionImage image) noexcept {
        const auto slot = static_cast<std::size_t>(image);
        return entry <= TemporalHistory && slot < static_cast<std::size_t>(NativeReconstructionImage::Count) &&
               entry >= KernelBindings[slot].minimum;
    }

    /** @copydoc PrepareNativeReconstructionDispatch */
    Result<NativeReconstructionDispatchPlan> PrepareNativeReconstructionDispatch(const ReconstructionFrameRequest &request,
                                                                                 const RenderCapabilitySnapshot &capabilities,
                                                                                 const NativeReconstructionTemporalSettings &tuning,
                                                                                 const std::optional<NativeReconstructionHistory> history) {
        using Outcome = Result<NativeReconstructionDispatchPlan>;
        if (const auto valid = ValidateReconstructionFrame(request, capabilities); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (!SupportsDispatch(request, capabilities))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        const bool temporal = request.kind == ReconstructionKind::Temporal;
        if (temporal != history.has_value() || (history && !ValidTemporal(request, tuning, history->frame)))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidDispatch));
        if (history) {
            if (const auto pending = history->owner.CheckPending(history->frame); pending.HasError())
                return Outcome::Failure(pending.ErrorValue());
        }
        const auto &exposure = std::get<ReconstructionExposureScales>(request.exposure);
        const bool readsHistory = history && history->frame.canReadPrevious && tuning.historyWeight > 0.0F;
        const auto render = request.compatibility.renderExtent;
        const auto target = request.compatibility.targetExtent;
        return Outcome::Success({.entry = SelectEntry(temporal, readsHistory),
                                 .constants = {.extents = {render.width, render.height, target.width, target.height},
                                               .sample = {request.currentJitterUv.x, request.currentJitterUv.y,
                                                          readsHistory ? exposure.current / exposure.previous : 1.0F,
                                                          readsHistory ? tuning.historyWeight : 0.0F},
                                               .depth = {readsHistory ? tuning.absoluteDepthTolerance : 0.0F,
                                                         readsHistory ? tuning.relativeDepthTolerance : 0.0F, 0.0F, 0.0F}}});
    }

    /** @copydoc DescribeNativeReconstructionShader */
    Result<NativeReconstructionShaderContract> DescribeNativeReconstructionShader(NativeReconstructionEntry entry,
                                                                                  const ShaderTargetRequirement &target) {
        using Outcome = Result<NativeReconstructionShaderContract>;
        const auto name = NativeReconstructionEntryName(entry);
        if (name.empty() || target.backend != ShaderTargetBackend::Vulkan || target.payloadFormat != ShaderPayloadFormat::SpirV16 ||
            !target.supportsCompute || !target.supportsStorageResources ||
            target.maximumInlineConstantBytes < sizeof(NativeReconstructionDispatch))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        NativeReconstructionShaderContract result;
        auto &manifest = result.manifest;
        manifest.schemaVersion = 1;
        manifest.sourceIdentity = "horo/runtime/native_reconstruction.hlsl";
        manifest.sourceRevision = 3;
        manifest.entryPoints = {{ShaderStage::Compute, std::string{name}}};
        manifest.inlineConstants = {{0, sizeof(NativeReconstructionDispatch), ShaderStageVisibility::Compute}};
        manifest.targets = {target};
        for (std::uint32_t slot = 0; slot < KernelBindings.size(); ++slot) {
            const auto &binding = KernelBindings[slot];
            if (entry < binding.minimum)
                continue;
            const ShaderBindingId id{slot + 1};
            const auto access = binding.kind == StorageTexture ? ShaderResourceAccess::ReadWrite : ShaderResourceAccess::ReadOnly;
            manifest.bindings.push_back({id, binding.kind, access, 1, ShaderStageVisibility::Compute});
            result.reflectionNames.push_back({id, std::string{binding.name}, {}});
        }
        if (const auto valid = ValidateShaderManifest(manifest); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success(std::move(result));
    }

    /** @copydoc AdmitNativeReconstructionShader */
    Result<std::span<const std::uint8_t>> AdmitNativeReconstructionShader(NativeReconstructionEntry entry,
                                                                          const ShaderTargetRequirement &target,
                                                                          const CompiledShaderArtifact &artifact) {
        using Outcome = Result<std::span<const std::uint8_t>>;
        const auto contract = DescribeNativeReconstructionShader(entry, target);
        if (contract.HasError())
            return Outcome::Failure(contract.ErrorValue());
        if (!artifact.reflection || artifact.backend != target.backend || artifact.payloadFormat != target.payloadFormat)
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidArtifact));
        if (const auto valid = ValidateReflectedShaderArtifact(artifact, *artifact.reflection); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        constexpr auto maximumPayload = 16U * 1024U * 1024U;
        const auto decoded = DecodeShaderArtifactStages(artifact.payload, target.backend, target.payloadFormat, maximumPayload);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        const auto &package = decoded.Value();
        if (package.stageCount != 1 || package.stages[0].stage != ShaderStage::Compute ||
            package.stages[0].entryPoint != NativeReconstructionEntryName(entry))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidArtifact));
        const auto &reflection = artifact.reflection->reflection;
        const ShaderReflectionCandidate candidate{reflection.backend,    reflection.interfaceSchemaVersion, reflection.bindings,
                                                  reflection.parameters, reflection.stageInterface,         reflection.targetBindings,
                                                  reflection.sourceMap};
        const auto normalized = NormalizeShaderReflection(contract.Value().manifest, target, candidate);
        if (normalized.HasError())
            return Outcome::Failure(normalized.ErrorValue());
        if (normalized.Value().interfaceCompatibility != reflection.interfaceCompatibility || !ExactLayout(normalized.Value()))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidArtifact));
        return Outcome::Success(package.stages[0].bytes);
    }

    /** @copydoc IRenderBackend::RealizeNativeReconstructionKernel */
    Result<std::shared_ptr<IResidentNativeReconstructionKernel>> IRenderBackend::RealizeNativeReconstructionKernel(
        const CookedNativeReconstructionKernel &) {
        return Result<std::shared_ptr<IResidentNativeReconstructionKernel>>::Failure(MakeError(NativeReconstructionErrors::Unsupported));
    }
}  // namespace Horo::Render
