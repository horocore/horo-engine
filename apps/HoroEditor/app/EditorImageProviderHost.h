#pragma once

/** @file EditorImageProviderHost.h @brief Explicit installed image-provider inspection for the editor composition. */
#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Runtime/Render/ImageProviderRegistry.h"
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Security/ArtifactSignature.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Render {
    class RenderFrontend;
}

namespace Horo::Editor {
    /** @brief Independent invocation paths for explicit inspection or supported viewport activation. */
    struct EditorImageProviderInspection final {
        Render::ReconstructionKind mode;
        std::filesystem::path productRoot;
        std::filesystem::path trustedPublishers;
        bool activateViewport{};
        bool scaleViewport{}; /**< Explicit upscaling activation rather than same-size temporal rendering. */
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
        bool temporalGuidesVerified{}; /**< Both temporal guide programs were admitted from this inventory. */
    };

    /** @brief Storage from the exact detached-signed product inventory, retained through selected Vulkan admission. */
    struct EditorSignedSpatialProvider final {
        Render::CookedNativeReconstructionKernel kernel;
        std::vector<std::uint8_t> scenePayload;
        std::vector<std::uint8_t> sceneReflection;
    };

    /** @brief Exact same-operation shader bytes retained from one authenticated product inventory. */
    struct EditorSignedProviderShader final {
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> reflection;
    };

    /** @brief Complete signed Vulkan temporal family and its readiness on the selected renderer. */
    struct EditorSignedTemporalProvider final {
        EditorImageProviderAvailability availability;
        std::array<Render::CookedNativeReconstructionKernel, 2> kernels; /**< Seed and history, in that order. */
        EditorSignedProviderShader sceneLinear;
        EditorSignedProviderShader linearDepth;
        EditorSignedProviderShader motion;
        std::optional<Render::CookedNativeReconstructionKernel> upscaleFallback;
    };

    /** @brief Parses explicit and mutually exclusive inspection or viewport activation arguments. @param args Process arguments including
     * argv[0].
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
    [[nodiscard]] Result<EditorImageProviderAvailability> InspectEditorImageProvider(const Render::RenderFrontend &frontend,
                                                                                     const EditorImageProviderInspection &request,
                                                                                     const EditorProviderProductIdentity &expected,
                                                                                     const DurableFileSystem &files,
                                                                                     const Security::ArtifactVerifier &verifier);

    /** @brief Verifies an explicit installed native spatial provider and matching scene-linear shader from the same signed inventory.
     * @param frontend Selected renderer on its cold owner. @param request Explicit spatial activation with independent trust.
     * @param expected Current executable product identity. @param files Bounded host file authority.
     * @param verifier Verifier supplied by the independent publisher policy.
     * @return Exact owned verified payloads or typed signature, compatibility, or artifact error.
     */
    [[nodiscard]] Result<EditorSignedSpatialProvider> LoadEditorSignedSpatialProvider(const Render::RenderFrontend &frontend,
                                                                                      const EditorImageProviderInspection &request,
                                                                                      const EditorProviderProductIdentity &expected,
                                                                                      const DurableFileSystem &files,
                                                                                      const Security::ArtifactVerifier &verifier);

    /** @brief Returns the complete verified temporal family for cold viewport preparation.
     * @param frontend Selected renderer on its owner thread outside a frame.
     * @param request Explicit temporal mode and product root.
     * @param expected Current editor release identity.
     * @param files Bounded host read authority.
     * @param verifier Verifier from independent publisher policy.
     * @return Owned verified cook bytes and backend readiness, or a typed failure for an incomplete or invalid family.
     * @details This operation queries capability without changing the active renderer or realizing any resources. */
    [[nodiscard]] Result<EditorSignedTemporalProvider> LoadEditorSignedTemporalProvider(const Render::RenderFrontend &frontend,
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
