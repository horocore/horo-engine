#include "ShaderCompilerToolchainCatalog.h"

#include <array>
#include <memory>
#include <optional>
#include <ranges>

namespace Horo::Render {
    namespace {
        // Distribution archives and executables were retrieved from the named upstream source and
        // reviewed on 2026-09-13. The companion lock document records runtime dependencies and licenses.
        constexpr std::array ApprovedTools{
            ApprovedShaderCompilerTool{ShaderCompilerTool::Dxc, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:b1bfa493d5c780b94c20b8b5f5aed50d1c4d03339cd55f496bd223eedeec1734", "NCSA-and-MIT"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvTools, "linux-x86_64-ubuntu-26.04", "ubuntu-2026.1-1",
                                       "https://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-tools/spirv-tools_2026.1-1_amd64.deb",
                                       "sha256:24e972ed4f2e92ada6f64b32ff40550fda02038385656736af871ca3dcb2b867",
                                       "sha256:85367fefdb7e93ae45654255ac2b7f8dc7056b6df78a6fdeb03ce395c7477239", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvCross, "linux-x86_64-ubuntu-26.04", "ubuntu-2021.01.15+1.4.335.0-1",
                                       "https://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-cross/"
                                       "spirv-cross_2021.01.15+1.4.335.0-1_amd64.deb",
                                       "sha256:50d11b7efc263240d04b015fecfd419377a4a3e2a9cb59387f2229aae03e4e7f",
                                       "sha256:335caee5ce86daefc3dee5e13100c2118a1a1cccb183a8c6df817090e0cbb976", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::DxilValidator, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:87cc9c1e459a7d6a0dc52c7b319d627d0f6078a2be95621e5a98b3cf12f255cb", "NCSA-and-MIT"},
        };

        [[nodiscard]] std::optional<Sha256Digest> ParseLockedDigest(const std::string_view value) {
            if (auto parsed = ParseSha256(value); parsed.HasValue())
                return parsed.Value();
            return std::nullopt;
        }

        [[nodiscard]] const ApprovedShaderCompilerTool *FindBuiltInApproval(const std::string_view hostPlatform,
                                                                            const ShaderCompilerToolIdentity &identity) {
            const auto found = std::ranges::find_if(ApprovedTools, [&](const ApprovedShaderCompilerTool &entry) {
                if (const auto archiveDigest = ParseLockedDigest(entry.archiveSha256); archiveDigest.has_value())
                    return entry.hostPlatform == hostPlatform && entry.tool == identity.tool && entry.release == identity.release &&
                           *archiveDigest == identity.buildDigest;
                return false;
            });
            return found == ApprovedTools.end() ? nullptr : std::to_address(found);
        }
    }  // namespace

    namespace ShaderCompilerToolchainDetail {
        bool IsApproved(const ShaderCompilerToolchainConfiguration &configuration, const ShaderCompilerToolInstallation &tool) {
            if (configuration.verifiedCatalog)
                return configuration.verifiedCatalog->Approves(configuration.hostPlatform, tool.identity, tool.executableDigest);
            const ApprovedShaderCompilerTool *approved = FindBuiltInApproval(configuration.hostPlatform, tool.identity);
            if (approved == nullptr)
                return false;
            if (const auto approvedExecutable = ParseLockedDigest(approved->executableSha256); approvedExecutable.has_value())
                return *approvedExecutable == tool.executableDigest;
            return false;
        }
    }  // namespace ShaderCompilerToolchainDetail

    /** @copydoc ApprovedShaderCompilerTools */
    std::span<const ApprovedShaderCompilerTool> ApprovedShaderCompilerTools() noexcept {
        return ApprovedTools;
    }
}  // namespace Horo::Render
