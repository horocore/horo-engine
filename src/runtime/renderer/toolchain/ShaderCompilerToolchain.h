#pragma once

#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo {
    class IExternalProcessRunner;
}

namespace Horo::Render {
    /** @brief One immutable executable admitted by the reviewed shader-tool lock catalog. */
    struct ShaderCompilerToolInstallation final {
        ShaderCompilerToolIdentity identity; /**< Exact archive/package identity required by target descriptors. */
        std::filesystem::path executable;    /**< Absolute host-selected executable path. */
        Sha256Digest executableDigest;       /**< Reviewed digest of the executable bytes at `executable`. */
    };

    /** @brief Host-owned paths and finite process/file bounds for production shader compilation. */
    struct ShaderCompilerToolchainConfiguration final {
        std::string hostPlatform;                                          /**< Exact host lock-catalog identity. */
        std::filesystem::path scratchRoot;                                 /**< Absolute host-owned isolated scratch root. */
        std::vector<ShaderCompilerToolInstallation> tools;                 /**< Sorted unique installed tools. */
        std::chrono::milliseconds processTimeout{std::chrono::minutes{5}}; /**< Per-process timeout. */
        std::size_t maximumToolBinaryBytes{256U * 1024U * 1024U};          /**< Verification read bound. */
        std::size_t maximumProcessOutputBytes{1024U * 1024U};              /**< Per-invocation output bound. */
    };

    /** @brief One reviewed host executable lock and its source/archive provenance. */
    struct ApprovedShaderCompilerTool final {
        ShaderCompilerTool tool{ShaderCompilerTool::Dxc}; /**< Logical role within the selected route. */
        std::string_view hostPlatform;                    /**< Exact supported cook-host identity. */
        std::string_view release;                         /**< Reviewed upstream/distribution release. */
        std::string_view source;                          /**< Immutable upstream artifact provenance. */
        std::string_view archiveSha256;                   /**< Canonical SHA-256 of downloaded archive/package. */
        std::string_view executableSha256;                /**< Canonical SHA-256 of executable bytes. */
        std::string_view license;                         /**< Reviewed SPDX-like license summary. */
    };

    /**
     * @brief Returns the immutable built-in shader-tool lock catalog.
     * @return Process-lifetime catalog sorted by host and tool role.
     */
    [[nodiscard]] std::span<const ApprovedShaderCompilerTool> ApprovedShaderCompilerTools() noexcept;

    struct ExternalShaderCompilerAdapterState;

    /**
     * @brief Production shell-free adapter for DXC, SPIRV-Tools, SPIRV-Cross, and Apple Metal tools.
     * @details The adapter shares ownership of the process runner. The configured runner must support concurrent
     * calls. Each Compile call owns an isolated scratch directory which is removed before return. Tool identity
     * and executable content are verified when the adapter is created, before any source is written.
     */
    class ExternalShaderCompilerAdapter final : public IShaderCompilerAdapter {
    public:
        /** @brief Releases adapter state without touching host-owned tool installations. */
        ~ExternalShaderCompilerAdapter() noexcept override;
        ExternalShaderCompilerAdapter(const ExternalShaderCompilerAdapter &) = delete;
        ExternalShaderCompilerAdapter &operator=(const ExternalShaderCompilerAdapter &) = delete;
        /** @brief Moves an adapter while preserving its shared host execution state. @param other Adapter to move from. */
        ExternalShaderCompilerAdapter(ExternalShaderCompilerAdapter &&) noexcept;
        /** @brief Replaces this adapter with moved state. @param other Adapter to move from. @return This adapter. */
        ExternalShaderCompilerAdapter &operator=(ExternalShaderCompilerAdapter &&) noexcept;

        /**
         * @brief Validates host configuration and creates a production adapter.
         * @param configuration Exact host platform, scratch root, tool paths, identities, and finite bounds.
         * @param processes Shared shell-free process runner with concurrent-call support.
         * @return Move-only adapter, or a typed configuration/tool-lock failure.
         */
        [[nodiscard]] static Result<ExternalShaderCompilerAdapter> Create(ShaderCompilerToolchainConfiguration configuration,
                                                                          std::shared_ptr<IExternalProcessRunner> processes);

        /** @copydoc IShaderCompilerAdapter::Compile */
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                  const CancellationToken &cancellation) const override;

    private:
        explicit ExternalShaderCompilerAdapter(std::shared_ptr<ExternalShaderCompilerAdapterState> state) noexcept;
        std::shared_ptr<ExternalShaderCompilerAdapterState> state_;
    };
}  // namespace Horo::Render
