#pragma once
/** @file RuntimeUiPaintShaderPackage.h @brief App-private canonical UI shader package value admission. */
#include "Horo/Runtime/Render/UiPaintKernel.h"

namespace Horo::Application::Internal {
    /** @brief Returns the exact shipped UI Metal target without discovering or activating any backend. */
    [[nodiscard]] Render::ShaderTargetRequirement ProjectUiPaintMetalTarget();
    /** @brief Admits the packaged payload and the single canonical same-cook reflection record.
     * @param payload Exact complete HOROSHDR envelope. @param metadata Exact bounded canonical metadata.
     * @param selectedBackend Actual host-selected backend; every non-Metal profile fails closed.
     * @return Owned strict kernel or original typed malformed/identity/interface failure.
     * @details No native realization, file access, compiler invocation or trust approval occurs here. */
    [[nodiscard]] Result<std::shared_ptr<const Render::CookedUiPaintKernel>> DecodeProjectUiPaintMetalPackage(
        std::span<const std::uint8_t> payload, std::span<const std::uint8_t> metadata, Render::ShaderTargetBackend selectedBackend);

    /** @brief Admits a package for a selected Vulkan scene canvas with exact offscreen and stencil formats.
     * @param payload Exact complete HOROSHDR envelope. @param metadata Exact bounded canonical metadata.
     * @param selectedBackend Actual host-selected backend; every non-Vulkan profile fails closed.
     * @return Owned strict offscreen kernel or an original typed malformed/identity/pipeline-interface failure.
     * @pre Serialized bytes are supplied by the caller's admitted asset source. No file access, runtime compilation or publisher
     * selection occurs here. */
    [[nodiscard]] Result<std::shared_ptr<const Render::CookedUiPaintKernel>> DecodeProjectUiPaintVulkanScenePackage(
        std::span<const std::uint8_t> payload, std::span<const std::uint8_t> metadata, Render::ShaderTargetBackend selectedBackend);
}  // namespace Horo::Application::Internal
