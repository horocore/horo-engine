#pragma once

/**
 * @file ShaderPermutation.h
 * @brief Bounded shader permutation selection and runtime specialization contracts.
 */

#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/ShaderManifest.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Render {
    /** @brief Stable compile-time feature identity mapped to one bit in a permutation mask. */
    struct ShaderPermutationFeature {
        std::uint8_t bitIndex{0};
        std::string defineName;
    };

    /** @brief Canonical runtime value for one manifest-declared specialization input. */
    struct ShaderSpecializationValue {
        ShaderSpecializationId id;
        ShaderValueType type{ShaderValueType::Uint32};
        std::uint32_t valueBits{0};

        [[nodiscard]] auto operator<=>(const ShaderSpecializationValue &) const noexcept = default;
    };

    /**
     * @brief Explicit finite set of feature masks that may produce compiled variants.
     *
     * This inert value owns no callbacks, threads, backend objects, cancellation, or shutdown.
     * Every admitted mask is authored rather than generated from an implicit Cartesian product.
     */
    struct ShaderPermutationModel {
        std::uint32_t schemaVersion{0};
        std::size_t maximumVariantCount{0};
        std::vector<ShaderPermutationFeature> features;
        std::vector<std::uint64_t> admittedFeatureMasks;
    };

    /** @brief Stable logical identity for one compiled shader permutation. */
    struct ShaderPermutationKey {
        std::string shaderIdentity;
        std::uint64_t featureMask{0};
        RenderPassId passId;
        Sha256Digest vertexLayoutCompatibility;
        AssetCookTargetId target;

        [[nodiscard]] auto operator<=>(const ShaderPermutationKey &) const noexcept = default;
    };

    /** @brief Bounded caller-owned validation and resolution envelope. */
    struct ShaderPermutationLimits {
        std::size_t maximumFeatures{32};
        std::size_t maximumVariants{4'096};
        std::size_t maximumSpecializationValues{64};
        std::size_t maximumDefineNameBytes{128};
    };

    /** @brief One requested permutation plus runtime-only specialization overrides. */
    struct ShaderPermutationRequest {
        std::uint64_t featureMask{0};
        RenderPassId passId;
        Sha256Digest vertexLayoutCompatibility;
        AssetCookTargetId target;
        std::vector<ShaderSpecializationValue> specializationValues;
    };

    /** @brief Admitted compile-time key and complete canonical runtime specialization values. */
    struct ResolvedShaderPermutation {
        ShaderPermutationKey key;
        std::vector<ShaderSpecializationValue> specializationValues;
    };

    /** @brief Immutable, validated manifest and permutation model prepared outside runtime selection. */
    class PreparedShaderPermutationModel final {
    public:
        PreparedShaderPermutationModel(const PreparedShaderPermutationModel &) = default;
        PreparedShaderPermutationModel(PreparedShaderPermutationModel &&) noexcept = default;
        PreparedShaderPermutationModel &operator=(const PreparedShaderPermutationModel &) = default;
        PreparedShaderPermutationModel &operator=(PreparedShaderPermutationModel &&) noexcept = default;

    private:
        PreparedShaderPermutationModel(ShaderManifest manifest, ShaderPermutationModel model, ShaderPermutationLimits limits)
            : m_manifest(std::move(manifest)), m_model(std::move(model)), m_limits(limits) {}

        ShaderManifest m_manifest;
        ShaderPermutationModel m_model;
        ShaderPermutationLimits m_limits;

        friend Result<PreparedShaderPermutationModel> PrepareShaderPermutationModel(ShaderManifest, ShaderPermutationModel,
                                                                                    const ShaderPermutationLimits &);
        friend Result<ResolvedShaderPermutation> ResolveShaderPermutation(const PreparedShaderPermutationModel &,
                                                                          const ShaderPermutationRequest &);
    };

    /**
     * @brief Validates an explicit finite permutation model against a shader manifest.
     * @param manifest Validated shader interface declaring specialization inputs.
     * @param model Canonical feature declarations and admitted masks.
     * @param limits Finite validation envelope.
     * @return Success or a stable ShaderPermutationErrors failure.
     */
    [[nodiscard]] Result<void> ValidateShaderPermutationModel(const ShaderManifest &manifest, const ShaderPermutationModel &model,
                                                              const ShaderPermutationLimits &limits = {});

    /**
     * @brief Validates and owns an immutable manifest/model snapshot for repeated runtime selection.
     * @param manifest Shader interface and specialization declarations to validate and own.
     * @param model Finite permutation model to validate and own.
     * @param limits Finite validation and subsequent request envelope.
     * @return Prepared immutable model, or a stable ShaderPermutationErrors failure.
     */
    [[nodiscard]] Result<PreparedShaderPermutationModel> PrepareShaderPermutationModel(ShaderManifest manifest,
                                                                                       ShaderPermutationModel model,
                                                                                       const ShaderPermutationLimits &limits = {});

    /**
     * @brief Resolves one declared compile-time variant and its runtime specialization values.
     * @param prepared Immutable manifest/model snapshot validated at its loading boundary.
     * @param request Requested feature mask, logical identities, and canonical overrides.
     * @return Admitted key plus complete values, or a typed failure without fallback or compilation.
     */
    [[nodiscard]] Result<ResolvedShaderPermutation> ResolveShaderPermutation(const PreparedShaderPermutationModel &prepared,
                                                                             const ShaderPermutationRequest &request);
}  // namespace Horo::Render
