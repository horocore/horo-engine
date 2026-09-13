#include "Horo/Runtime/Render/ShaderPermutation.h"

#include "Horo/Runtime/Render/ShaderPermutationErrors.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t SupportedSchemaVersion = 1;
        constexpr std::size_t HardMaximumFeatures = 63;
        constexpr std::size_t HardMaximumVariants = 65'536;
        constexpr std::size_t HardMaximumSpecializationValues = 512;
        constexpr std::size_t HardMaximumDefineNameBytes = 256;

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsValidLimits(const ShaderPermutationLimits &limits) noexcept {
            return limits.maximumFeatures > 0 && limits.maximumFeatures <= HardMaximumFeatures && limits.maximumVariants > 0 &&
                   limits.maximumVariants <= HardMaximumVariants && limits.maximumSpecializationValues > 0 &&
                   limits.maximumSpecializationValues <= HardMaximumSpecializationValues && limits.maximumDefineNameBytes > 0 &&
                   limits.maximumDefineNameBytes <= HardMaximumDefineNameBytes;
        }

        [[nodiscard]] bool IsDefineCharacter(const unsigned char value) noexcept {
            return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
        }

        [[nodiscard]] bool IsValidDefineName(const std::string &value, const std::size_t maximumBytes) noexcept {
            if (value.empty() || value.size() > maximumBytes || value.front() < 'A' || value.front() > 'Z')
                return false;
            return std::ranges::all_of(value, [](const char character) {
                return IsDefineCharacter(static_cast<unsigned char>(character));
            });
        }

        [[nodiscard]] bool IsZeroDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t value) {
                return value == 0;
            });
        }

        [[nodiscard]] const ShaderSpecializationInput *FindSpecialization(const ShaderManifest &manifest,
                                                                          const ShaderSpecializationId id) noexcept {
            const auto found = std::ranges::lower_bound(manifest.specializationInputs, id, {}, &ShaderSpecializationInput::id);
            return found != manifest.specializationInputs.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        [[nodiscard]] bool IsValidValue(const ShaderSpecializationValue &value) noexcept {
            if (value.type == ShaderValueType::Bool32)
                return value.valueBits <= 1;
            if (value.type == ShaderValueType::Float32)
                return std::isfinite(std::bit_cast<float>(value.valueBits));
            return value.type == ShaderValueType::Int32 || value.type == ShaderValueType::Uint32;
        }

        [[nodiscard]] Result<void> ValidateSpecializationOverrides(const ShaderManifest &manifest, const ShaderPermutationRequest &request,
                                                                   const ShaderPermutationLimits &limits) {
            if (request.specializationValues.size() > limits.maximumSpecializationValues)
                return Failure(ShaderPermutationErrors::InvalidSpecialization);
            for (std::size_t index = 0; index < request.specializationValues.size(); ++index) {
                const ShaderSpecializationValue &value = request.specializationValues[index];
                if (!value.id.IsValid() || (index > 0 && request.specializationValues[index - 1].id >= value.id))
                    return Failure(ShaderPermutationErrors::NonCanonicalInput);
                const ShaderSpecializationInput *declaration = FindSpecialization(manifest, value.id);
                if (declaration == nullptr || declaration->type != value.type || !IsValidValue(value))
                    return Failure(ShaderPermutationErrors::InvalidSpecialization);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateShaderPermutationModel */
    Result<void> ValidateShaderPermutationModel(const ShaderManifest &manifest, const ShaderPermutationModel &model,
                                                const ShaderPermutationLimits &limits) {
        if (!IsValidLimits(limits))
            return Failure(ShaderPermutationErrors::InvalidLimits);
        if (ValidateShaderManifest(manifest).HasError() || model.schemaVersion != SupportedSchemaVersion ||
            model.maximumVariantCount == 0 || model.maximumVariantCount > limits.maximumVariants ||
            model.features.size() > limits.maximumFeatures || model.admittedFeatureMasks.empty() ||
            model.admittedFeatureMasks.size() > model.maximumVariantCount)
            return Failure(ShaderPermutationErrors::InvalidModel);

        std::uint64_t declaredBits = 0;
        for (std::size_t index = 0; index < model.features.size(); ++index) {
            const ShaderPermutationFeature &feature = model.features[index];
            if (feature.bitIndex >= HardMaximumFeatures || !IsValidDefineName(feature.defineName, limits.maximumDefineNameBytes))
                return Failure(ShaderPermutationErrors::InvalidModel);
            if (index > 0 &&
                (model.features[index - 1].bitIndex >= feature.bitIndex || model.features[index - 1].defineName >= feature.defineName))
                return Failure(ShaderPermutationErrors::NonCanonicalInput);
            declaredBits |= std::uint64_t{1} << feature.bitIndex;
        }
        for (std::size_t index = 0; index < model.admittedFeatureMasks.size(); ++index) {
            const std::uint64_t mask = model.admittedFeatureMasks[index];
            if ((mask & ~declaredBits) != 0)
                return Failure(ShaderPermutationErrors::InvalidModel);
            if (index > 0 && model.admittedFeatureMasks[index - 1] >= mask)
                return Failure(ShaderPermutationErrors::NonCanonicalInput);
        }
        return Result<void>::Success();
    }

    /** @copydoc ResolveShaderPermutation */
    Result<ResolvedShaderPermutation> ResolveShaderPermutation(const ShaderManifest &manifest, const ShaderPermutationModel &model,
                                                               const ShaderPermutationRequest &request,
                                                               const ShaderPermutationLimits &limits) {
        if (const Result<void> validation = ValidateShaderPermutationModel(manifest, model, limits); validation.HasError())
            return Result<ResolvedShaderPermutation>::Failure(validation.ErrorValue());
        if (!request.passId.IsValid() || IsZeroDigest(request.vertexLayoutCompatibility) || !request.target.IsValid())
            return Result<ResolvedShaderPermutation>::Failure(MakeError(ShaderPermutationErrors::InvalidRequest));
        if (!std::ranges::binary_search(model.admittedFeatureMasks, request.featureMask))
            return Result<ResolvedShaderPermutation>::Failure(MakeError(ShaderPermutationErrors::UnsupportedPermutation));
        if (const Result<void> specialization = ValidateSpecializationOverrides(manifest, request, limits); specialization.HasError())
            return Result<ResolvedShaderPermutation>::Failure(specialization.ErrorValue());

        try {
            ResolvedShaderPermutation resolved{.key = {.shaderIdentity = manifest.sourceIdentity,
                                                       .featureMask = request.featureMask,
                                                       .passId = request.passId,
                                                       .vertexLayoutCompatibility = request.vertexLayoutCompatibility,
                                                       .target = request.target}};
            resolved.specializationValues.reserve(manifest.specializationInputs.size());
            auto overrideIt = request.specializationValues.begin();
            for (const ShaderSpecializationInput &input : manifest.specializationInputs) {
                if (overrideIt != request.specializationValues.end() && overrideIt->id == input.id) {
                    resolved.specializationValues.push_back(*overrideIt);
                    ++overrideIt;
                } else {
                    resolved.specializationValues.push_back({input.id, input.type, input.defaultValueBits});
                }
            }
            return Result<ResolvedShaderPermutation>::Success(std::move(resolved));
        } catch (const std::bad_alloc &) {
            return Result<ResolvedShaderPermutation>::Failure(MakeError(ShaderPermutationErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
