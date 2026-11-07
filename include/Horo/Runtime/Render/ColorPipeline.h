#pragma once

/** @file ColorPipeline.h
 * @brief Cold immutable color-plan admission. No cooking, GPU realization, execution or publication is implied.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/Exposure.h"
#include "Horo/Runtime/Render/PostProcessGraph.h"
#include "Horo/Runtime/Render/RenderPresentationOutput.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Explicit scene-referred creative look role; output programs have their own distinct contract. */
    enum class ColorTransformRole : std::uint8_t {
        CreativeLook
    };
    /** @brief Cook-declared cubic LUT interpolation; no runtime alternative is selected. */
    enum class ColorLutInterpolation : std::uint8_t {
        Trilinear,
        Tetrahedral
    };
    /** @brief Explicit versioned bounded linear-AP1 domain; outside pixels are diagnosed, never clamped or bypassed. */
    enum class ColorLookDomainPolicy : std::uint8_t {
        RejectOutsideV1 = 1
    };
    /** @brief Distinguishes intentional bypass from a required but missing cooked creative look. */
    enum class ColorLookMode : std::uint8_t {
        Disabled,
        Enabled
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
        std::uint32_t schemaVersion{2}, configurationRelease{};
        Sha256Digest configurationDigest, payloadDigest, referenceDigest;
        ColorTransformRole role{ColorTransformRole::CreativeLook};
        ColorLutInterpolation interpolation{ColorLutInterpolation::Tetrahedral};
        ColorLookDomainPolicy domainPolicy{ColorLookDomainPolicy::RejectOutsideV1};
        std::uint32_t dimension{}; /**< Cubic RGB32F LUT; payload SHA hashes each IEEE754 float as four little-endian bytes, red varies
                                      fastest; 2..65, at most 64 MiB. */
        Math::Vec3 domainMinimum{}, domainMaximum{1.0F, 1.0F, 1.0F};
    };

    /** @brief Owns copied finite cooked creative-look RGB data, mapping exposed ACEScg to exposed ACEScg.
     * This cubic look is separate from the analytic output program and owns no output gamut, tone map or nits policy.
     * Domain/interpolation and cook configuration are explicit; no missing output program is replaced by a look LUT.
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
        /** @brief Returns the exact 2D RGBA32F atlas shape; texel(r+N*g,b) preserves red-fastest cube order.
         * @return Single-level sampled/copy-destination descriptor; the frontend must independently admit actual device limits. */
        [[nodiscard]] RenderTextureDescriptor AtlasDescriptor() const noexcept;
        /** @brief Borrows original RGB samples losslessly padded with alpha one as little-endian float32.
         * @return Immutable bytes retained for this transform's lifetime; no native residency is implied. */
        [[nodiscard]] std::span<const std::byte> AtlasUpload() const noexcept;
        /** @brief Returns the derived exact atlas payload identity. @return SHA256 of AtlasUpload bytes, not cook trust. */
        [[nodiscard]] Sha256Digest AtlasDigest() const noexcept;
        /** @brief Counts original RGB and derived atlas bytes together. @return Complete bounded owned payload size. */
        [[nodiscard]] std::uint64_t PayloadBytes() const noexcept;

    private:
        /** @brief Adopts validated data. @param descriptor Exact metadata. @param rgb Owned finite samples. */
        CookedColorTransform(ColorTransformDescriptor descriptor, std::vector<float> rgb);
        ColorTransformDescriptor descriptor_;
        std::vector<float> rgb_;
        std::vector<std::byte> atlas_;
        Sha256Digest atlasDigest_;
    };

    /** @brief Immutable cold transform pins; supplies no Scene/history, device, output or native-execution admission. */
    class ColorTransformSelection final {
    public:
        /** @brief Borrows the exact analytic generation. @return Pin valid for this selection's lifetime. */
        [[nodiscard]] const std::shared_ptr<const CookedColorOutputProgram> &Analytic() const noexcept;
        /** @brief Borrows the exact optional creative asset, including an explicitly disabled retained asset. @return Owned pin. */
        [[nodiscard]] const std::shared_ptr<const CookedColorTransform> &Look() const noexcept;
        /** @brief Reports intentional bypass or required look sampling. @return Validated selected mode. */
        [[nodiscard]] ColorLookMode Mode() const noexcept;

    private:
        friend Result<ColorTransformSelection> PrepareColorTransformSelection(const std::shared_ptr<const CookedColorOutputProgram> &,
                                                                              ColorLookMode,
                                                                              const std::shared_ptr<const CookedColorTransform> &,
                                                                              const ColorGradingSettings &, std::uint64_t);
        /** @brief Pins one already validated cold selection. @param analytic Exact program. @param mode Selected mode. @param look Asset
         * pin. */
        ColorTransformSelection(std::shared_ptr<const CookedColorOutputProgram> analytic, ColorLookMode mode,
                                std::shared_ptr<const CookedColorTransform> look) noexcept;
        std::shared_ptr<const CookedColorOutputProgram> analytic_;
        std::shared_ptr<const CookedColorTransform> look_;
        ColorLookMode mode_;
    };

    /** @brief Validates transform identity, supported grading and aggregate original/upload storage without inventing frame identities.
     * @param analytic Exact pinned output generation. @param mode Explicit look selection. @param look Optional immutable creative asset.
     * @param grading Numeric settings; valid nondefault controls are explicitly unsupported. @param maximumBytes Complete cold payload
     * ceiling.
     * @return Immutable pins or the same typed transform/grading/budget failure used by complete color-plan preparation.
     */
    [[nodiscard]] Result<ColorTransformSelection> PrepareColorTransformSelection(
        const std::shared_ptr<const CookedColorOutputProgram> &analytic, ColorLookMode mode,
        const std::shared_ptr<const CookedColorTransform> &look, const ColorGradingSettings &grading = {},
        std::uint64_t maximumBytes = 64U * 1024U * 1024U);

    /** @brief Explicit cold plan inputs; identity validation supplies no residency/capability/native-completion proof. */
    struct ColorPipelineRequest final {
        std::uint64_t generation{}, settingsGeneration{}, submission{};
        PostProcessTextureInput sceneColor; /**< UnexposedAcesCg RGBA16F only in this initial contract. */
        TemporalHistoryCompatibility compatibility;
        ExposureSettings exposureSettings;
        ExposureState exposure;
        ColorGradingSettings grading;
        ColorLookMode lookMode{ColorLookMode::Disabled};
        RenderPresentationOutput output; /**< Already admitted native output; this operation does not configure it. */
        std::shared_ptr<const CookedColorTransform> look;
        std::shared_ptr<const CookedColorOutputProgram> outputTransform;
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
        extern const ErrorCodeDescriptor InvalidTransform;   /**< Missing, corrupt or incompatible cooked transform. */
        extern const ErrorCodeDescriptor IncompatibleInput;  /**< Foreign scene/history/color generation or representation. */
        extern const ErrorCodeDescriptor UnsupportedGrading; /**< Numeric grading semantics are not implemented by this route. */
        extern const ErrorCodeDescriptor UnsupportedOutput;  /**< Unsupported or mismatching named output intent. */
        extern const ErrorCodeDescriptor CapacityExceeded;   /**< Complete transform pins exceed the admitted storage ceiling. */
        extern const ErrorCodeDescriptor Cancelled;          /**< Candidate preparation cancelled; active state is untouched. */
        extern const ErrorCodeDescriptor AllocationFailed;   /**< Cold immutable payload allocation failed. */
    }  // namespace ColorPipelineErrors
}  // namespace Horo::Render
