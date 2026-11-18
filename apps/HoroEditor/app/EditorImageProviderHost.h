#pragma once

/** @file EditorImageProviderHost.h @brief Explicit installed image-provider inspection for the editor composition. */
#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Runtime/Render/ImageProviderRegistry.h"
#include "Horo/Security/ArtifactSignature.h"

#include <filesystem>
#include <optional>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Render {
    class RenderFrontend;
}

namespace Horo::Editor {
    /** @brief Independent invocation paths and requested mode for read-only selected-renderer inspection. */
    struct EditorImageProviderInspection final {
        Render::ReconstructionKind mode;
        std::filesystem::path productRoot;
        std::filesystem::path trustedPublishers;
    };

    /** @brief Current executable release identity compared to the signed installed inventory. */
    struct EditorProviderProductIdentity final {
        Release::EngineProductVersion version;
        Release::DistributionPlatform platform;
        Release::DistributionArchitecture architecture;
    };

    /** @brief One exact selected backend observation and its authenticated inventory. */
    struct EditorImageProviderAvailability final {
        Render::ImageProviderRoute route;
        Render::ImageProviderAvailability support;
        Sha256Digest inventory;
    };

    /** @brief Parses only explicit image-provider inspection arguments. @param args Process arguments including argv[0].
     * @return A complete request, no request, or a typed invalid-input failure. */
    [[nodiscard]] Result<std::optional<EditorImageProviderInspection>> ParseEditorImageProviderInspection(std::span<char *const> args);

    /** @brief Verifies and admits the exact selected built-in provider from a detached signed release inventory.
     * @param frontend Selected renderer, still on its owner thread outside a frame.
     * @param request Explicit requested mode and host product/trust paths.
     * @param expected Current editor release identity from the executable.
     * @param files Host-owned bounded read authority over the quiescent product root.
     * @param verifier Independent product publisher verifier supplied by host policy.
     * @return Effective route support or typed read, signature, package or shader failure.
     * @details Installation verification reads the complete requested artifacts into owned buffers, checks signed length and digest,
     * then decodes those same buffers. Inspection does not realize provider kernels or change rendering selection.
     */
    [[nodiscard]] Result<EditorImageProviderAvailability> InspectEditorImageProvider(Render::RenderFrontend &frontend,
                                                                                     const EditorImageProviderInspection &request,
                                                                                     const EditorProviderProductIdentity &expected,
                                                                                     const DurableFileSystem &files,
                                                                                     const Security::ArtifactVerifier &verifier);

    /** @brief Creates the standalone editor's verifier from an explicitly supplied independent publisher document.
     * @param files Host-authorized bounded file reader. @param trust Explicit host publisher document.
     * @param nowMilliseconds Actual host wall time. @return Verifier using only unexpired, non-revoked signed-only roots. */
    [[nodiscard]] Result<Security::ArtifactVerifier> ReadEditorImageProviderTrust(const DurableFileSystem &files,
                                                                                  const std::filesystem::path &trust,
                                                                                  std::uint64_t nowMilliseconds);

    /** @brief Provides a stable portable inspection value for terminal diagnostics. @param value Exact renderer readiness.
     * @return Borrowed value with static lifetime. */
    [[nodiscard]] const char *EditorImageProviderReadinessName(Render::ImageProviderReadiness value) noexcept;
}  // namespace Horo::Editor
