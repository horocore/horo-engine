#pragma once

/** @file EditorNativeReconstructionReleaseLayout.h @brief Shared owned Vulkan reconstruction release slices. */
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"

#include <array>
#include <string>
#include <string_view>

namespace Horo::Editor::NativeReconstructionRelease {
    using Render::NativeReconstructionEntry;

    /** @brief Complete native Vulkan reconstruction family owned by the editor release stage. */
    inline constexpr std::array Entries{NativeReconstructionEntry::Spatial, NativeReconstructionEntry::TemporalSeed,
                                        NativeReconstructionEntry::TemporalHistory};

    inline constexpr std::size_t MaximumPayloadBytes = 16U * 1024U * 1024U;
    inline constexpr std::size_t MaximumReflectionBytes = 1024U * 1024U;

    /** @brief Canonical product-relative prefix for a closed stage, or empty for an unknown entry. */
    inline std::string Prefix(NativeReconstructionEntry entry) {
        const auto name = Render::NativeReconstructionEntryName(entry);
        if (name.empty())
            return {};
        return "assets/shaders/runtime/native-reconstruction-vulkan/" + std::string{name};
    }

    /** @brief Exact scene-linear material files consumed by explicit signed spatial startup. */
    inline constexpr std::string_view SceneLinearPayloadPath = "assets/shaders/editor/viewport-scene-linear-vulkan.horoshdr";
    inline constexpr std::string_view SceneLinearReflectionPath = "assets/shaders/editor/viewport-scene-linear-vulkan.reflection.json";

    /** @brief Exact portable target contract shared by the release cook and signed editor. */
    inline constexpr Render::ShaderTargetRequirement Target{.backend = Render::ShaderTargetBackend::Vulkan,
                                                            .payloadFormat = Render::ShaderPayloadFormat::SpirV16,
                                                            .descriptorVersion = 1,
                                                            .interfaceSchemaVersion = 1,
                                                            .maximumBindings = 16,
                                                            .maximumInlineConstantBytes = 128,
                                                            .supportsCompute = true,
                                                            .supportsStorageResources = true};
}  // namespace Horo::Editor::NativeReconstructionRelease
