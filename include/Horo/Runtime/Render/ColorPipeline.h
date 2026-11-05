#pragma once

/** @file ColorPipeline.h
 * @brief Cold immutable color-plan admission. No cooking, GPU realization, execution or publication is implied.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/Exposure.h"
#include "Horo/Runtime/Render/PostProcessGraph.h"
#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Explicit scene-look, production ACES2 or named diagnostic route. */
    enum class ColorTransformRole : std::uint8_t {
        CreativeLook,
        Aces2Output,
        NeutralDiagnostic
    };
    /** @brief Cook-declared cubic LUT interpolation; no runtime alternative is selected. */
    enum class ColorLutInterpolation : std::uint8_t {
        Trilinear,
        Tetrahedral
    };
    /** @brief Final target quantization policy after display-linear UI/accessibility composition. */
    enum class ColorDitherPolicy : std::uint8_t {
        None,
        Ordered
    };

    /** @brief Cook-issued identity; the config release, transform options and reference corpus must be pinned by the cooker.
     * Hash agreement validates payload integrity only, never Academy conformance or shader/native qualification.
     */
    struct ColorTransformDescriptor final {
        std::uint32_t schemaVersion{1}, configurationRelease{};
        Sha256Digest configurationDigest, payloadDigest, referenceDigest;
        ColorTransformRole role{ColorTransformRole::Aces2Output};
        ColorLutInterpolation interpolation{ColorLutInterpolation::Tetrahedral};
        std::uint32_t dimension{}; /**< Cubic RGB32F LUT; payload SHA hashes each IEEE754 float as four little-endian bytes, red varies
                                      fastest; 2..65, at most 64 MiB. */
        Math::Vec3 domainMinimum{}, domainMaximum{1.0F, 1.0F, 1.0F};
        RenderOutputColor output; /**< Exact final encoding intent; output LUT values themselves are display-linear. */
        float referenceWhiteNits{}, peakNits{}, blackNits{};
    };

    /** @brief Owns copied finite cooked RGB data. Look maps exposed ACEScg to exposed ACEScg; output maps exposed ACEScg
     * through the cook-pinned AP1/interchange/ACES2 route into target display-linear RGB. No runtime approximation is selected.
     * NeutralDiagnostic is an explicit separate role; a missing Aces2Output never falls back to it.
     */
    class CookedColorTransform final {
    public:
        /** @brief Copies one cold bounded LUT after metadata, finite-value and portable payload-hash validation.
         * @param descriptor Cook-issued metadata; conformance/trust remain the cook owner's responsibility.
         * @param rgb Exact cubic RGB32F samples, red coordinate varies fastest.
         * @return Immutable copied payload or typed invalid-transform/allocation failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<const CookedColorTransform>> Create(ColorTransformDescriptor descriptor,
                                                                                        std::span<const float> rgb);
        /** @brief Borrows immutable pinned metadata. @return Descriptor valid for this object lifetime. */
        [[nodiscard]] const ColorTransformDescriptor &Descriptor() const noexcept;
        /** @brief Borrows finite immutable LUT values. @return Samples valid for this object lifetime. */
        [[nodiscard]] std::span<const float> Rgb() const noexcept;

    private:
        /** @brief Adopts validated data. @param descriptor Exact metadata. @param rgb Owned finite samples. */
        CookedColorTransform(ColorTransformDescriptor descriptor, std::vector<float> rgb);
        ColorTransformDescriptor descriptor_;
        std::vector<float> rgb_;
    };

    /** @brief Explicit cold plan inputs; identity validation supplies no residency/capability/native-completion proof. */
    struct ColorPipelineRequest final {
        std::uint64_t generation{}, settingsGeneration{}, submission{};
        PostProcessTextureInput sceneColor; /**< UnexposedAcesCg RGBA16F only in this initial contract. */
        TemporalHistoryCompatibility compatibility;
        ExposureSettings exposureSettings;
        ExposureState exposure;
        ColorGradingSettings grading;
        RenderPresentationOutput output; /**< Already admitted native output; this operation does not configure it. */
        std::shared_ptr<const CookedColorTransform> look, outputTransform;
        ColorTransformRole requiredOutput{ColorTransformRole::Aces2Output};
        ColorDitherPolicy dither{ColorDitherPolicy::Ordered};
        std::uint32_t outputBitDepth{8};
        std::uint64_t maximumTransformBytes{64U * 1024U * 1024U};
        CancellationToken cancellation;
    };

    /** @brief Immutable candidate retaining exact copied metadata and immutable cooked transforms. Borrowed Scene/exposure
     * resources still require frontend residency leases through native completion. Preparation changes no active state.
     * The frontend must compare all identities again at RenderSafePoint and atomically publish, retaining old generations
     * for in-flight work. This value API supplies neither publication authority nor an abstract/unimplemented executor.
     */
    class ColorPipelinePlan final {
    public:
        /** @brief Borrows exact immutable inputs. @return Metadata and pins valid for this plan lifetime. */
        [[nodiscard]] const ColorPipelineRequest &Inputs() const noexcept;

    private:
        friend Result<ColorPipelinePlan> PrepareColorPipeline(const ColorPipelineRequest &);
        /** @brief Adopts a validated candidate. @param request Complete copied metadata and immutable transform pins. */
        explicit ColorPipelinePlan(ColorPipelineRequest request);
        ColorPipelineRequest request_;
    };

    /** @brief Validates an inert candidate without mutating any active plan or waiting for native work.
     * @param request Exact scene/view/exposure/output metadata, transforms and cold storage bound.
     * @return Immutable candidate or typed cancellation, incompatibility, unsupported output, transform or budget failure.
     */
    [[nodiscard]] Result<ColorPipelinePlan> PrepareColorPipeline(const ColorPipelineRequest &request);

    namespace ColorPipelineErrors {
        extern const ErrorCodeDescriptor InvalidTransform;  /**< Missing, corrupt or incompatible cooked transform. */
        extern const ErrorCodeDescriptor IncompatibleInput; /**< Foreign scene/history/color generation or representation. */
        extern const ErrorCodeDescriptor UnsupportedOutput; /**< Unsupported or mismatching named output intent. */
        extern const ErrorCodeDescriptor CapacityExceeded;  /**< Complete transform pins exceed the admitted storage ceiling. */
        extern const ErrorCodeDescriptor Cancelled;         /**< Candidate preparation cancelled; active state is untouched. */
        extern const ErrorCodeDescriptor AllocationFailed;  /**< Cold immutable payload allocation failed. */
    }  // namespace ColorPipelineErrors
}  // namespace Horo::Render
