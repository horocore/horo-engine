#pragma once

#include "Horo/Platform/ExternalProcess.h"
#include "ShaderCompilerToolchain.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Render::ShaderCompilerToolchainDetail {
    struct ToolArtifactRecord final {
        ShaderStage stage{ShaderStage::Vertex};
        std::string entryPoint;
        std::vector<std::uint8_t> bytes;
    };

    [[nodiscard]] Result<std::vector<std::uint8_t>> ReadBoundedFile(const std::filesystem::path &path, std::size_t maximumBytes,
                                                                    const ErrorCodeDescriptor &failure);
    [[nodiscard]] Result<void> WriteFile(const std::filesystem::path &path, std::span<const std::uint8_t> bytes);
    [[nodiscard]] Result<std::vector<std::uint8_t>> PackageStages(ShaderTargetBackend backend, ShaderPayloadFormat format,
                                                                  const std::vector<ToolArtifactRecord> &stages, std::size_t maximumBytes);
    [[nodiscard]] std::string_view StageProfile(ShaderStage stage) noexcept;
    [[nodiscard]] std::string_view StageName(ShaderStage stage) noexcept;
    [[nodiscard]] bool IsSpirV(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool IsDxil(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool IsGlsl410(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::string SanitizeLine(std::string line, const std::filesystem::path &scratch, const std::filesystem::path &source);
    [[nodiscard]] ShaderCompilerDiagnostic MakeToolDiagnostic(std::string message, bool truncated, std::string_view sourceIdentity,
                                                              ProcessOutputStream stream);
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
