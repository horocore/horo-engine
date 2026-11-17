#pragma once
/** @file ExposureGpuKernel.h
 * @brief Closed offline-cooked exposure histogram/adaptation family and native preparation lease.
 */
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <array>
#include <memory>

namespace Horo::Render {
    /** @brief Logical portable bindings; native register slots are t0,u1,t2,u3,b4 in space0. */
    enum class ExposureGpuBinding : std::uint32_t {
        SceneColor = 1,
        Histogram = 2,
        PreviousExposure = 3,
        OutputExposure = 4,
        Dispatch = 5
    };
    /** @brief Fixed ordered compute stages; there is no arbitrary compute entry admission. */
    enum class ExposureGpuStage : std::uint8_t {
        ClearHistogram,
        BuildHistogram,
        Adapt
    };

    /** @brief One real offline artifact and final-target reflection. */
    struct CookedExposureGpuStage final {
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
        ShaderReflectionCandidate reflection;
    };

    /** @brief Exact clear/build/adapt artifacts in stage enum order. */
    struct CookedExposureGpuKernel final {
        std::array<CookedExposureGpuStage, 3> stages;
    };

    /** @brief Native immutable pipeline-family lease; actual Metal implementation retains the native pipelines and a
     * bounded submission journal. Host releases on render owner thread; submitted commands retain native resources
     * until terminal completion. This interface admits no custom callbacks or CPU execution substitute.
     */
    class IResidentExposureGpuKernel {
    public:
        virtual ~IResidentExposureGpuKernel() = default;
    };

    /** @brief Builds one closed-stage cook manifest. @param stage Fixed stage. @param target Explicit cook target.
     * @return Logical manifest; unknown stages produce an invalid empty entry list rejected by validation.
     */
    [[nodiscard]] ShaderManifest MakeExposureGpuShaderManifest(ExposureGpuStage stage, const ShaderTargetRequirement &target);
    /** @brief Validates all three exact stage interfaces, payload envelopes and final target reflection.
     * @param kernel Complete cooked family. @param backend Explicit selected backend.
     * @return Normalized binding maps or typed invalid/unsupported failure; no native realization occurs.
     */
    [[nodiscard]] Result<std::array<NormalizedShaderReflection, 3>> ValidateCookedExposureGpuKernel(const CookedExposureGpuKernel &kernel,
                                                                                                    ShaderTargetBackend backend);
    /** @brief Borrows an exact compute entry from the standard HOROSHDR envelope.
     * @param stage Fixed expected stage. @param artifact Actual offline artifact.
     * @return Borrowed native bytes or typed failure; caller retains artifact storage.
     */
    [[nodiscard]] Result<std::span<const std::uint8_t>> ExposureGpuNativePayload(ExposureGpuStage stage,
                                                                                 const CompiledShaderArtifact &artifact);

    namespace ExposureGpuErrors {
        extern const ErrorCodeDescriptor InvalidInput; /**< Malformed ABI, cook payload or foreign/stale identity. */
        extern const ErrorCodeDescriptor Unsupported;  /**< Backend has no actual native exposure executor. */
        extern const ErrorCodeDescriptor Capacity;     /**< Finite storage/journal/work bound exhausted. */
    }  // namespace ExposureGpuErrors
}  // namespace Horo::Render
