#pragma once
/** @file EditorViewportColorTypes.h @brief Inert explicit viewport color inputs, selected resource ports and bounded lifecycle diagnostics.
 */
#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/ColorPipeline.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"

#include <array>
#include <optional>

namespace Horo::Render::Internal {
    struct ColorOutputCookPackage;
}

namespace Horo::Editor {
    /** @brief One immutable real cooked program; the host supplies source-independent canonical manifest and artifact together. */
    struct EditorViewportColorProgram final {
        Render::ShaderManifest manifest;
        Render::CompiledShaderArtifact artifact;
    };

    /** @brief Explicit cold inputs, decoded by the host; this contract neither reads paths nor chooses a backend. */
    struct EditorViewportColorInputs final {
        std::shared_ptr<const Render::Internal::ColorOutputCookPackage> package;
        Render::CookedExposureGpuKernel exposure;
        Render::ColorLookMode lookMode{Render::ColorLookMode::Disabled};
        std::shared_ptr<const Render::CookedColorTransform> look;
        Render::ColorGradingSettings grading;
        std::uint64_t maximumTransformBytes{64U * 1024U * 1024U};
    };
    /** @brief Completion observation from the actual selected backend's registered resource owner. */
    enum class EditorViewportColorReadiness : std::uint8_t {
        Pending,
        Ready
    };
    /** @brief Observable host lifecycle state, independent of shader diagnostic colors. */
    enum class EditorViewportColorStatus : std::uint8_t {
        Unavailable,
        Preparing,
        Ready,
        Prepared,
        Encoded,
        Failed,
        Closed
    };

    namespace ViewportColorErrors {
        inline const ErrorCodeDescriptor Pending{ErrorDomainId{"horo.editor.viewport.color"},
                                                 ErrorCode{"editor.viewport.color.pending"},
                                                 ErrorSeverity::Info,
                                                 "Color resources are awaiting actual native completion.",
                                                 {},
                                                 true,
                                                 false};
    }

    /** @brief Selected host callbacks; serialized on the render owner, never asynchronous borrowed closures. */
    struct EditorViewportColorResourcePort final {
        void *context{};
        Result<EditorViewportColorReadiness> (*bufferReadiness)(void *, Render::RenderBufferHandle){};
        Result<void> (*writeUniform)(void *, Render::RenderBufferHandle, std::span<const std::byte>){};
    };

    /** @brief Bounded CPU diagnostics; automatic EV remains absent until genuine completed GPU observation exists. */
    struct EditorViewportColorDiagnostics final {
        EditorViewportColorStatus status{EditorViewportColorStatus::Unavailable};
        bool active{}, candidate{}, automatic{}, resetPending{true};
        std::uint64_t settingsGeneration{1}, acceptedSubmission{}, exposureGeneration{};
        std::uint64_t outputRevision{};
        bool unmanagedSdrPreview{}; /**< Native legacy SDR encoding only; never a calibrated display or physical HDR claim. */
        std::optional<float> automaticEv;
    };
}  // namespace Horo::Editor
