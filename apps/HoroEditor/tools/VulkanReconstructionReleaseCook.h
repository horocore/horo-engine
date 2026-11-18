#pragma once

/** @file VulkanReconstructionReleaseCook.h @brief Normal offline cook and owned pre-sign release slice for native Vulkan reconstruction. */
#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <filesystem>

namespace Horo::Editor::Offline {
    /** @brief Cooks all native Vulkan reconstruction entries and publishes one verified owned stage directory.
     * @param destination New absolute output directory under an exclusive host-owned parent.
     * @param scratch Existing independent absolute compiler scratch root.
     * @param candidate Release job's nonzero pre-sign inventory identity.
     * @param source Bounded authored shader source used by the same request for each entry.
     * @param tools Real locked tool identities in DXC, SPIR-V Tools and SPIRV-Cross order.
     * @param compiler Host-owned compiler executing the declared and validated offline route.
     * @param cancellation Optional owning release cancellation through the final verification fence.
     * @return Canonical unsigned inventory of exact published files or typed failure before owned publication.
     */
    [[nodiscard]] Result<Release::ReleasePreSignInventory> CookVulkanReconstructionRelease(
        const std::filesystem::path &destination, const std::filesystem::path &scratch, Release::ReleaseCandidateId candidate,
        std::vector<std::uint8_t> source, std::span<const Render::ShaderCompilerToolIdentity, 3> tools,
        const Render::IShaderCompilerAdapter &compiler, const CancellationToken &cancellation = {});
}  // namespace Horo::Editor::Offline
