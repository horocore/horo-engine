#include "Horo/TerrainRender/TerrainMaterialBinding.h"

#include "Horo/Runtime/Render/StandardPbrMaterialErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::TerrainRender {
    namespace TerrainMaterialBindingErrors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.terrain.render"},
                                          .code = ErrorCode{"terrain.render.material_invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Terrain material layers, classification or admission facts are inconsistent.",
                                          .remediationHint =
                                              "Supply ordered resolved material assets and complete exact generation evidence."};
        const ErrorCodeDescriptor
            VariantUnavailable{.domain = ErrorDomainId{"horo.terrain.render"},
                               .code = ErrorCode{"terrain.render.material_variant_unavailable"},
                               .defaultSeverity = ErrorSeverity::Error,
                               .summary = "The exact cooked terrain shader variant or artifact is unavailable.",
                               .remediationHint =
                                   "Cook the declared layer, hole, pass, classification and target variant before activation."};
        const ErrorCodeDescriptor
            WrongThread{.domain = ErrorDomainId{"horo.terrain.render"},
                        .code = ErrorCode{"terrain.render.material_wrong_thread"},
                        .defaultSeverity = ErrorSeverity::Error,
                        .summary = "Terrain material publication ran outside its host owner lane.",
                        .remediationHint = "Marshal publication, snapshot capture and shutdown to the creating render-safe-point thread."};
    }  // namespace TerrainMaterialBindingErrors

    namespace {
        /** @brief Checks exact captured identities and admission lifecycle before allocating a candidate. */
        [[nodiscard]] Result<void> ValidateContext(const TerrainMaterialBindingRequest &request) {
            using namespace Terrain;
            const auto &context = request.context;
            const auto &configuration = request.configuration.Data();
            if (context.lifecycle != TerrainRuntimeLifecycle::Active)
                return Result<void>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
            if (!context.terrain.IsValid() || !context.revisions.IsValid() || !context.configuration.IsValid() ||
                context.renderGeneration == 0 || request.pass >= TerrainMaterialPass::Count)
                return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
            if (context.configuration != configuration.configuration || context.revisions.capability != configuration.capability)
                return Result<void>::Failure(MakeError(TerrainErrors::RevisionStale));
            if (request.materials.size() != request.layers.Data().layerCount)
                return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
            const auto revalidated = TerrainMaterialLayerSet::Create(request.layers.Data(), request.configuration);
            if (revalidated.HasError())
                return Result<void>::Failure(revalidated.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Admits only complete ordered exact-asset layer correspondence, preserving material classification. */
        [[nodiscard]] Result<void> ValidateLayerInputs(const TerrainMaterialBindingRequest &request) {
            const auto alpha = request.materials.front().descriptor.alphaMode;
            for (std::size_t index = 0; index < request.materials.size(); ++index) {
                const auto &input = request.materials[index];
                const auto &layer = request.layers.Data().layers[index];
                if (input.layer != layer.id || input.asset != layer.material || input.descriptor.alphaMode != alpha ||
                    input.resident.generation != request.context.renderGeneration ||
                    input.resident.pipeline.owner != request.variant.pipeline.owner ||
                    input.reflection.backend != request.variant.artifact.backend ||
                    input.reflection.interfaceSchemaVersion != request.variant.target.interfaceSchemaVersion)
                    return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (request.materials[previous].descriptor.id == input.descriptor.id)
                        return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreparedTerrainMaterialShader::Create */
    Result<PreparedTerrainMaterialShader> PreparedTerrainMaterialShader::Create(Render::ShaderManifest manifest,
                                                                                Render::ShaderPermutationModel model) {
        constexpr std::array<std::string_view, 7> names{"TERRAIN_LAYER_BIT_0", "TERRAIN_LAYER_BIT_1", "TERRAIN_LAYER_BIT_2",
                                                        "TERRAIN_LAYER_BIT_3", "TERRAIN_HOLES",       "TERRAIN_ALPHA_BIT_0",
                                                        "TERRAIN_ALPHA_BIT_1"};
        if (model.features.size() != names.size() || model.maximumVariantCount > 128)
            return Result<PreparedTerrainMaterialShader>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (model.features[index].bitIndex != index || model.features[index].defineName != names[index])
                return Result<PreparedTerrainMaterialShader>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        }
        std::array<Render::ShaderTargetRequirement, 8> targets{};
        if (manifest.targets.size() > targets.size())
            return Result<PreparedTerrainMaterialShader>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        const auto targetCount = static_cast<std::uint8_t>(manifest.targets.size());
        std::copy(manifest.targets.begin(), manifest.targets.end(), targets.begin());
        auto prepared = Render::PrepareShaderPermutationModel(std::move(manifest), std::move(model));
        if (prepared.HasError())
            return Result<PreparedTerrainMaterialShader>::Failure(prepared.ErrorValue());
        return Result<PreparedTerrainMaterialShader>::Success(
            PreparedTerrainMaterialShader{std::move(prepared).Value(), targets, targetCount});
    }

    /** @copydoc PreparedTerrainMaterialShader::Model */
    const Render::PreparedShaderPermutationModel &PreparedTerrainMaterialShader::Model() const noexcept {
        return model_;
    }

    /** @copydoc PreparedTerrainMaterialShader::AdmitsTarget */
    bool PreparedTerrainMaterialShader::AdmitsTarget(const Render::ShaderTargetRequirement &target) const noexcept {
        const auto fields = [](const Render::ShaderTargetRequirement &value) {
            return std::tie(value.backend, value.payloadFormat, value.descriptorVersion, value.interfaceSchemaVersion,
                            value.maximumBindings, value.maximumInlineConstantBytes, value.supportsCompute, value.supportsStorageResources);
        };
        for (const auto &declared : std::span{targets_}.first(targetCount_)) {
            if (fields(declared) == fields(target))
                return true;
        }
        return false;
    }

    /** @copydoc TerrainMaterialFeatureMask */
    Result<std::uint64_t> TerrainMaterialFeatureMask(const std::uint8_t layerCount, const bool holes,
                                                     const Render::MaterialAlphaMode alpha) {
        if (layerCount == 0 || layerCount > Terrain::TerrainDescriptorHardLimits::LayersPerTile ||
            static_cast<std::uint8_t>(alpha) > static_cast<std::uint8_t>(Render::MaterialAlphaMode::Additive))
            return Result<std::uint64_t>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        return Result<std::uint64_t>::Success(static_cast<std::uint64_t>(layerCount - 1U) | (holes ? 16U : 0U) |
                                              (static_cast<std::uint64_t>(alpha) << 5U));
    }

    /** @copydoc BlendTerrainPbrSamples */
    Result<TerrainPbrLayerSample> BlendTerrainPbrSamples(const Terrain::TerrainMaterialLayerSet &layers,
                                                         const Terrain::TerrainMaterialWeights &weights, const std::uint16_t tileLayerMask,
                                                         const std::span<const TerrainPbrLayerSample> samples) {
        const auto validWeights = Terrain::ValidateTerrainMaterialWeights(weights, layers, tileLayerMask);
        if (validWeights.HasError())
            return Result<TerrainPbrLayerSample>::Failure(validWeights.ErrorValue());
        if (samples.size() != weights.layerCount)
            return Result<TerrainPbrLayerSample>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        const auto unit = [](const float value) {
            return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
        };
        const auto color = [](const Math::Vec3 &value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && value.x >= 0.0F && value.y >= 0.0F &&
                   value.z >= 0.0F;
        };
        TerrainPbrLayerSample result{.normal = {}, .occlusion = 0.0F, .opacity = 0.0F};
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto &sample = samples[index];
            const float lengthSquared =
                sample.normal.x * sample.normal.x + sample.normal.y * sample.normal.y + sample.normal.z * sample.normal.z;
            if (!color(sample.albedo) || !color(sample.emissive) || !unit(sample.metallic) || !unit(sample.roughness) ||
                !unit(sample.occlusion) || !unit(sample.opacity) || !std::isfinite(lengthSquared) ||
                std::abs(lengthSquared - 1.0F) > 0.001F)
                return Result<TerrainPbrLayerSample>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
            const float weight = static_cast<float>(weights.values[index]) / 65535.0F;
            result.albedo = result.albedo + weight * sample.albedo;
            result.emissive = result.emissive + weight * sample.emissive;
            result.normal = result.normal + weight * sample.normal;
            result.metallic += weight * sample.metallic;
            result.roughness += weight * sample.roughness;
            result.occlusion += weight * sample.occlusion;
            result.opacity += weight * sample.opacity;
        }
        const float lengthSquared =
            result.normal.x * result.normal.x + result.normal.y * result.normal.y + result.normal.z * result.normal.z;
        if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12F)
            return Result<TerrainPbrLayerSample>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        const float inverseLength = 1.0F / std::sqrt(lengthSquared);
        result.normal = result.normal * inverseLength;
        if (!color(result.albedo) || !color(result.emissive))
            return Result<TerrainPbrLayerSample>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
        return Result<TerrainPbrLayerSample>::Success(result);
    }

    /** @copydoc PrepareTerrainMaterialBinding */
    Result<TerrainMaterialBindingData> PrepareTerrainMaterialBinding(const TerrainMaterialBindingRequest &request,
                                                                     const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<TerrainMaterialBindingData>::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
        if (const auto context = ValidateContext(request); context.HasError())
            return Result<TerrainMaterialBindingData>::Failure(context.ErrorValue());
        if (const auto layers = ValidateLayerInputs(request); layers.HasError())
            return Result<TerrainMaterialBindingData>::Failure(layers.ErrorValue());
        const auto features =
            TerrainMaterialFeatureMask(request.layers.Data().layerCount, request.holes, request.materials.front().descriptor.alphaMode);
        if (features.HasError())
            return Result<TerrainMaterialBindingData>::Failure(features.ErrorValue());
        if (request.permutation.featureMask != features.Value())
            return Result<TerrainMaterialBindingData>::Failure(MakeError(TerrainMaterialBindingErrors::VariantUnavailable));
        auto resolved = Render::ResolveShaderPermutation(request.shader.Model(), request.permutation);
        if (resolved.HasError())
            return Result<TerrainMaterialBindingData>::Failure(resolved.ErrorValue());
        const auto &variant = request.variant;
        if (variant.key != resolved.Value().key || (variant.expectedArtifact == Sha256Digest{}) ||
            variant.artifact.artifactKey != variant.expectedArtifact || variant.artifact.payload.empty() ||
            variant.artifact.payload.size() > 64U * 1024U * 1024U || !variant.pipeline.IsValid() || variant.pass != request.pass ||
            variant.target.backend != variant.artifact.backend || variant.target.payloadFormat != variant.artifact.payloadFormat ||
            !request.shader.AdmitsTarget(variant.target))
            return Result<TerrainMaterialBindingData>::Failure(MakeError(TerrainMaterialBindingErrors::VariantUnavailable));
        try {
            auto selection = std::move(resolved).Value();
            TerrainMaterialBindingData candidate{.layers = request.layers.Data(),
                                                 .context = request.context,
                                                 .permutation = std::move(selection.key),
                                                 .specializationValues = std::move(selection.specializationValues),
                                                 .artifact = variant.expectedArtifact,
                                                 .pipeline = variant.pipeline,
                                                 .pass = request.pass,
                                                 .holes = request.holes};
            candidate.materials.reserve(request.materials.size());
            for (const auto &input : request.materials) {
                if (cancellation.IsCancellationRequested())
                    return Result<TerrainMaterialBindingData>::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
                auto prepared = Render::PrepareStandardPbrMaterial(input.descriptor, input.reflection, input.resident, request.layerLimits);
                if (prepared.HasError())
                    return Result<TerrainMaterialBindingData>::Failure(prepared.ErrorValue());
                for (const auto &texture : prepared.Value().textures) {
                    if (texture.texture.owner != variant.pipeline.owner || texture.sampler.owner != variant.pipeline.owner)
                        return Result<TerrainMaterialBindingData>::Failure(MakeError(TerrainMaterialBindingErrors::Invalid));
                }
                candidate.materials.push_back(std::move(prepared).Value());
            }
            if (cancellation.IsCancellationRequested())
                return Result<TerrainMaterialBindingData>::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
            return Result<TerrainMaterialBindingData>::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Result<TerrainMaterialBindingData>::Failure(MakeError(Render::StandardPbrMaterialErrors::AllocationFailed));
        }
    }

    /** @copydoc TerrainMaterialBindingOwner::TerrainMaterialBindingOwner */
    TerrainMaterialBindingOwner::TerrainMaterialBindingOwner() : owner_(std::this_thread::get_id()) {}

    /** @copydoc TerrainMaterialBindingOwner::Publish */
    Result<std::shared_ptr<const TerrainMaterialBindingData>> TerrainMaterialBindingOwner::Publish(
        const TerrainMaterialBindingRequest &request, const TerrainMaterialBindingContext &currentContext,
        const std::uint64_t expectedPublication, const CancellationToken &cancellation) {
        using SnapshotResult = Result<std::shared_ptr<const TerrainMaterialBindingData>>;
        if (std::this_thread::get_id() != owner_)
            return SnapshotResult::Failure(MakeError(TerrainMaterialBindingErrors::WrongThread));
        if (closed_)
            return SnapshotResult::Failure(MakeError(Terrain::TerrainErrors::LifecycleUnavailable));
        if (request.context != currentContext || expectedPublication != (current_ ? lastPublication_ : 0))
            return SnapshotResult::Failure(MakeError(Terrain::TerrainErrors::RevisionStale));
        if (lastPublication_ == std::numeric_limits<std::uint64_t>::max())
            return SnapshotResult::Failure(MakeError(Terrain::TerrainErrors::GenerationExhausted));
        auto candidate = PrepareTerrainMaterialBinding(request, cancellation);
        if (candidate.HasError())
            return SnapshotResult::Failure(candidate.ErrorValue());
        try {
            auto owned = std::make_shared<TerrainMaterialBindingData>(std::move(candidate).Value());
            owned->publication = lastPublication_ + 1U;
            if (cancellation.IsCancellationRequested())
                return SnapshotResult::Failure(MakeError(Terrain::TerrainErrors::CompositionCancelled));
            current_ = std::move(owned);
            ++lastPublication_;
            return SnapshotResult::Success(current_);
        } catch (const std::bad_alloc &) {
            return SnapshotResult::Failure(MakeError(Render::StandardPbrMaterialErrors::AllocationFailed));
        }
    }

    /** @copydoc TerrainMaterialBindingOwner::Snapshot */
    Result<std::shared_ptr<const TerrainMaterialBindingData>> TerrainMaterialBindingOwner::Snapshot() const {
        if (std::this_thread::get_id() != owner_)
            return Result<std::shared_ptr<const TerrainMaterialBindingData>>::Failure(MakeError(TerrainMaterialBindingErrors::WrongThread));
        if (closed_)
            return Result<std::shared_ptr<const TerrainMaterialBindingData>>::Failure(
                MakeError(Terrain::TerrainErrors::LifecycleUnavailable));
        return Result<std::shared_ptr<const TerrainMaterialBindingData>>::Success(current_);
    }

    /** @copydoc TerrainMaterialBindingOwner::Shutdown */
    Result<void> TerrainMaterialBindingOwner::Shutdown() {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(TerrainMaterialBindingErrors::WrongThread));
        closed_ = true;
        current_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::TerrainRender
