#pragma once

/** @file TerrainMaterialBinding.h
 * @brief Host-composed terrain material admission through standard PBR and cooked shader contracts.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderPermutation.h"
#include "Horo/Runtime/Render/StandardPbrMaterial.h"
#include "Horo/Terrain/TerrainMaterial.h"

#include <array>
#include <memory>
#include <optional>
#include <thread>

namespace Horo::TerrainRender {
    /** @brief Exact semantic pass family for a cooked terrain material variant. */
    enum class TerrainMaterialPass : std::uint8_t {
        Color,
        Depth,
        Shadow,
        Motion,
        Count
    };

    /** @brief Validated terrain-specific shader feature vocabulary; source compilation remains Assets-owned. */
    class PreparedTerrainMaterialShader final {
    public:
        /**
         * @brief Prepares the finite shared permutation model with the canonical terrain bit definitions.
         * @param manifest Exact declared shader identity and interface.
         * @param model Explicit masks using seven canonical terrain bits and at most 128 variants.
         * @return Owned validated model or original shader/terrain failure; never invents a permutation.
         */
        [[nodiscard]] static Result<PreparedTerrainMaterialShader> Create(Render::ShaderManifest manifest,
                                                                          Render::ShaderPermutationModel model);
        /** @brief Returns the immutable shared shader model. @return Borrowed model for synchronous resolution. */
        [[nodiscard]] const Render::PreparedShaderPermutationModel &Model() const noexcept;
        /** @brief Checks exact manifest-declared target requirements. @param target Cooked target. @return Whether admitted. */
        [[nodiscard]] bool AdmitsTarget(const Render::ShaderTargetRequirement &target) const noexcept;

    private:
        PreparedTerrainMaterialShader(Render::PreparedShaderPermutationModel model, std::array<Render::ShaderTargetRequirement, 8> targets,
                                      std::uint8_t targetCount)
            : model_(std::move(model)), targets_(targets), targetCount_(targetCount) {}

        Render::PreparedShaderPermutationModel model_;
        std::array<Render::ShaderTargetRequirement, 8> targets_;
        std::uint8_t targetCount_;
    };

    /** @brief One exact resolved material asset supplied by the host's material/asset authority. */
    struct TerrainLayerMaterialInput final {
        Terrain::TerrainLayerId layer{};
        Terrain::TerrainMaterialAssetId asset{};
        Render::StandardPbrMaterialDescriptor descriptor{};
        Render::NormalizedShaderReflection reflection{};
        Render::StandardPbrResidentInputs resident{};
    };

    /** @brief Exact cooked permutation, expected artifact identity and renderer-owned pipeline generation. */
    struct TerrainCookedMaterialVariant final {
        Render::ShaderPermutationKey key{};
        Sha256Digest expectedArtifact{};
        const Render::CompiledShaderArtifact &artifact;
        Render::RenderPipelineHandle pipeline{};
        TerrainMaterialPass pass{TerrainMaterialPass::Color};
        Render::ShaderTargetRequirement target{};
    };

    /** @brief Generation-scoped terrain and renderer admission facts captured by application composition. */
    struct TerrainMaterialBindingContext final {
        Terrain::TerrainRuntimeHandle terrain{};
        Terrain::TerrainSnapshotRevision revisions{};
        Terrain::TerrainConfigurationRevision configuration{};
        Terrain::TerrainRuntimeLifecycle lifecycle{Terrain::TerrainRuntimeLifecycle::Active};
        std::uint64_t renderGeneration{}; /**< Non-zero resource-owner generation, rechecked before publication. */
        [[nodiscard]] constexpr auto operator<=>(const TerrainMaterialBindingContext &) const noexcept = default;
    };

    /** @brief Immutable load-time request borrowed only during synchronous preparation. */
    struct TerrainMaterialBindingRequest final {
        const Terrain::TerrainMaterialLayerSet &layers;
        const Terrain::TerrainConfigurationSnapshot &configuration;
        const PreparedTerrainMaterialShader &shader;
        Render::ShaderPermutationRequest permutation{};
        TerrainCookedMaterialVariant variant;
        std::span<const TerrainLayerMaterialInput> materials{};
        TerrainMaterialPass pass{TerrainMaterialPass::Color};
        bool holes{}; /**< Must match the cooked tile payload; no hole removal fallback is allowed. */
        TerrainMaterialBindingContext context{};
        Render::StandardPbrMaterialLimits layerLimits{};
    };

    /** @brief Wholly owned material projection; renderer resource handles are references, never GPU ownership. */
    struct TerrainMaterialBindingData final {
        Terrain::TerrainMaterialLayerSetData layers{};
        TerrainMaterialBindingContext context{};
        Render::ShaderPermutationKey permutation{};
        std::vector<Render::ShaderSpecializationValue> specializationValues{};
        Sha256Digest artifact{};
        Render::RenderPipelineHandle pipeline{};
        TerrainMaterialPass pass{TerrainMaterialPass::Color};
        bool holes{};
        std::vector<Render::ResidentStandardPbrMaterial> materials{};
        std::uint64_t publication{};
    };

    namespace TerrainMaterialBindingErrors {
        /** @brief Malformed layer/material correspondence, unsupported semantics, or request bounds. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief The requested exact permutation, artifact or target is unavailable or incompatible. */
        extern const ErrorCodeDescriptor VariantUnavailable;
        /** @brief A publication operation ran outside the creating host/render owner lane. */
        extern const ErrorCodeDescriptor WrongThread;
    }  // namespace TerrainMaterialBindingErrors

    /**
     * @brief Builds the terrain-specific feature mask used by the shared shader permutation resolver.
     * @param layerCount Exact semantic layer count, one to sixteen.
     * @param holes Whether the immutable tile payload contains holes.
     * @param alpha Exact authored classification shared by all layers.
     * @return Bits 0..3 encode count minus one, bit 4 holes, bits 5..6 alpha; invalid values fail.
     * @details These compile-affecting fields must be declared by the supplied shader model. UV scale and
     * scalar PBR values remain runtime parameters and never create variants.
     */
    [[nodiscard]] Result<std::uint64_t> TerrainMaterialFeatureMask(std::uint8_t layerCount, bool holes, Render::MaterialAlphaMode alpha);

    /** @brief One evaluated layer in linear color and a shared tangent frame, after UV-scaled texture sampling. */
    struct TerrainPbrLayerSample final {
        Math::Vec3 albedo{};
        Math::Vec3 normal{0.0F, 0.0F, 1.0F};
        Math::Vec3 emissive{}; /**< Linear emitted radiance, including authored intensity. */
        float metallic{};
        float roughness{};
        float occlusion{1.0F};
        float opacity{1.0F};
    };

    /**
     * @brief Evaluates the canonical linear weighted PBR surface and renormalizes the blended tangent normal.
     * @param layers Exact admitted semantic order.
     * @param weights Canonical UNORM16 weights summing to 65535.
     * @param tileLayerMask Legal non-empty subset; excluded layer weights must be zero.
     * @param samples One finite evaluated sample per semantic layer, in a shared tangent frame.
     * @return Blended surface or typed failure for malformed weights/samples or a degenerate normal.
     * @details Allocation-free reference semantics for cook/shader parity. Sampling applies each layer's
     * UV scale before evaluation. No height/slope/noise modulation or implicit layer substitution occurs.
     */
    [[nodiscard]] Result<TerrainPbrLayerSample> BlendTerrainPbrSamples(const Terrain::TerrainMaterialLayerSet &layers,
                                                                       const Terrain::TerrainMaterialWeights &weights,
                                                                       std::uint16_t tileLayerMask,
                                                                       std::span<const TerrainPbrLayerSample> samples);

    /**
     * @brief Prepares actual standard PBR layer bytes and one exact cooked terrain shader binding.
     * @param request Captured semantic layers, resolved material assets, shader model/artifact and context.
     * @param cancellation Caller-owned cooperative operation state.
     * @return Owned detached binding or original typed terrain/shader/PBR failure without publication/fallback.
     * @details Bounded load-time synchronous work. No worker, source compilation, native creation or ambient
     * registration occurs. The host retains exact artifact/resource leases through upload and GPU retirement.
     */
    [[nodiscard]] Result<TerrainMaterialBindingData> PrepareTerrainMaterialBinding(const TerrainMaterialBindingRequest &request,
                                                                                   const CancellationToken &cancellation = {});

    /** @brief Host-owned publication lane; snapshots retain CPU values across replacement and shutdown. */
    class TerrainMaterialBindingOwner final {
    public:
        /** @brief Establishes the explicit creating host/render owner lane without activating a backend. */
        TerrainMaterialBindingOwner();
        TerrainMaterialBindingOwner(const TerrainMaterialBindingOwner &) = delete;
        TerrainMaterialBindingOwner &operator=(const TerrainMaterialBindingOwner &) = delete;
        /**
         * @brief Prepares and publishes a complete material generation at the host/render safe point.
         * @param request Immutable preparation inputs captured against currentContext.
         * @param currentContext Exact current terrain/render evidence; mismatches are stale.
         * @param expectedPublication Zero for insert, otherwise exact current publication for replacement.
         * @param cancellation Caller-owned cancellation, checked before publication.
         * @return New immutable snapshot, or failure preserving the prior publication.
         * @pre Host retains all renderer resources and artifact leases until every snapshot/frame retires.
         */
        [[nodiscard]] Result<std::shared_ptr<const TerrainMaterialBindingData>> Publish(const TerrainMaterialBindingRequest &request,
                                                                                        const TerrainMaterialBindingContext &currentContext,
                                                                                        std::uint64_t expectedPublication,
                                                                                        const CancellationToken &cancellation = {});
        /** @brief Captures the current publication on the owner lane. @return Snapshot or wrong-thread/closed failure. */
        [[nodiscard]] Result<std::shared_ptr<const TerrainMaterialBindingData>> Snapshot() const;
        /**
         * @brief Idempotently closes admission and releases the owner's CPU snapshot reference.
         * @return Success or wrong-thread failure; already-issued snapshots remain valid.
         * @details Performs no GPU destruction or wait. The host requests resource retirement separately.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        std::thread::id owner_;
        bool closed_{};
        std::uint64_t lastPublication_{};
        std::shared_ptr<const TerrainMaterialBindingData> current_{};
    };
}  // namespace Horo::TerrainRender
