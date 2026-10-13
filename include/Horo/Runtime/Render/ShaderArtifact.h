#pragma once

/** @file ShaderArtifact.h
 * @brief Shared bounded stage-package codec for offline cooks and runtime shader consumers.
 */

#include "Horo/Runtime/Render/ShaderManifest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Render {
    /** @brief Owned stage record emitted by an offline compiler adapter. */
    struct ShaderArtifactStage {
        ShaderStage stage{ShaderStage::Vertex};
        std::string entryPoint;
        std::vector<std::uint8_t> bytes;
    };

    /** @brief Synchronously borrowed record inside one validated cooked package. */
    struct ShaderArtifactStageView {
        ShaderStage stage{ShaderStage::Vertex};
        std::string_view entryPoint;
        std::span<const std::uint8_t> bytes;
    };

    /** @brief Finite borrowed decode result; every view expires with the input byte storage. */
    struct ShaderArtifactPackageView {
        static constexpr std::size_t MaximumStages = 8;
        std::array<ShaderArtifactStageView, MaximumStages> stages{};
        std::size_t stageCount{0};
    };

    /**
     * @brief Serializes canonical HOROSHDR version-one stage records with checked finite bounds.
     * @param backend Exact target backend family.
     * @param format Exact cooked payload format.
     * @param stages Nonempty unique stage/entry records, at most eight.
     * @param maximumBytes Finite total package byte envelope, at most 64 MiB.
     * @return Owned package or ShaderCompilerPipelineErrors::ToolOutputInvalid; performs no compilation.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> PackageShaderArtifactStages(ShaderTargetBackend backend, ShaderPayloadFormat format,
                                                                                std::span<const ShaderArtifactStage> stages,
                                                                                std::size_t maximumBytes);

    /**
     * @brief Validates a complete package and returns bounded borrowed stage views without allocating decode storage.
     * @param bytes Entire immutable package; trailing bytes are rejected.
     * @param backend Expected exact target family, never inferred from untrusted bytes.
     * @param format Expected exact payload format.
     * @param maximumBytes Finite total package byte envelope, at most 64 MiB.
     * @return Stage views or ShaderCompilerPipelineErrors::ToolOutputInvalid for malformed or foreign input.
     * @details Checks envelope version, lengths, stage enums, canonical entry names and duplicate records.
     * It grants no native support or reflection validity. Consumers separately validate the selected
     * manifest, artifact identity, target reflection and stage payload before native publication.
     * Error construction may allocate owned diagnostic metadata.
     */
    [[nodiscard]] Result<ShaderArtifactPackageView> DecodeShaderArtifactStages(std::span<const std::uint8_t> bytes,
                                                                               ShaderTargetBackend backend, ShaderPayloadFormat format,
                                                                               std::size_t maximumBytes);
}  // namespace Horo::Render
