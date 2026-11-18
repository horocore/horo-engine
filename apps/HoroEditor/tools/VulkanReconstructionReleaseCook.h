#pragma once

/** @file VulkanReconstructionReleaseCook.h @brief Normal offline cook and owned pre-sign release slice for native Vulkan reconstruction. */
#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Horo::Editor::Offline {
    /** @brief Creates the canonical reviewed-tool compile request shared with standalone scene-linear cooking.
     * @param source Owned ordinary editor viewport source. @param tools DXC, SPIR-V Tools, SPIRV-Cross in exact order.
     * @return Exact same-operation scene-linear shader compilation/reflection request. */
    [[nodiscard]] Render::ShaderCompilationRequest MakeVulkanSceneLinearCookRequest(
        std::vector<std::uint8_t> source, std::span<const Render::ShaderCompilerToolIdentity, 3> tools);

    /** @brief Cooks all native Vulkan reconstruction entries and publishes one verified owned stage directory.
     * @param destination New absolute output directory under an exclusive host-owned parent.
     * @param scratch Existing independent absolute compiler scratch root.
     * @param candidate Release job's nonzero pre-sign inventory identity.
     * @param source Borrowed bounded authored shader bytes for each synchronous compile request.
     * @param tools Real locked tool identities in DXC, SPIR-V Tools and SPIRV-Cross order.
     * @param compiler Host-owned compiler executing the declared and validated offline route.
     * @param cancellation Optional owning release cancellation through the final verification fence.
     * @return Canonical unsigned inventory of exact published files or typed failure before owned publication.
     */
    [[nodiscard]] Result<Release::ReleasePreSignInventory> CookVulkanReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        const std::vector<std::uint8_t> &source, std::span<const Render::ShaderCompilerToolIdentity, 3> tools,
        const Render::IShaderCompilerAdapter &compiler, const CancellationToken &cancellation = {});
    /** @brief Cooks one complete, atomic signed-spatial release input tree.
     * @param destination New absolute stage directory under an exclusive host-owned parent.
     * @param scratch Existing independent absolute compiler scratch root. @param candidate Nonzero release candidate identity.
     * @param reconstruction Borrowed bounded authored reconstruction source for all three native entries.
     * @param sceneLinear Borrowed bounded authored viewport source for the exact scene-linear material.
     * @param tools Reviewed DXC, SPIR-V Tools and SPIRV-Cross identities, in order.
     * @param compiler Existing real offline adapter. @param cancellation Owning release cancellation through verification.
     * @return Canonical unsigned inventory for all eight exact staged files, or typed failure before publication.
     */
    [[nodiscard]] Result<Release::ReleasePreSignInventory> CookVulkanSpatialReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        const std::vector<std::uint8_t> &reconstruction, const std::vector<std::uint8_t> &sceneLinear,
        std::span<const Render::ShaderCompilerToolIdentity, 3> tools, const Render::IShaderCompilerAdapter &compiler,
        const CancellationToken &cancellation = {});
}  // namespace Horo::Editor::Offline
