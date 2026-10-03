#pragma once

/**
 * @file ReleaseSigningBoundary.h
 * @brief Verified pre-sign handoff to a host-owned platform signing adapter.
 */

#include "Horo/Release/ReleaseArtifactManifest.h"

#include <filesystem>

namespace Horo::Release {
    /** @brief Frozen signing inputs; the credential is an opaque handle, never secret material. */
    struct ReleaseSigningRequest final {
        const ReleaseExecutionPlan &plan;
        const ReleasePreSignInventory &inventory;
        std::filesystem::path stageRoot;
        ReleaseCredentialHandle credential;
    };

    /** @brief Host adapter that resolves the opaque handle only inside its signing operation. */
    class IReleaseSigningBackend {
    public:
        virtual ~IReleaseSigningBackend() = default;

        /**
         * @brief Performs platform signing on the verified private stage.
         * @param stageRoot Private stage whose unsigned bytes were just verified.
         * @param credential Opaque handle selected by the frozen plan.
         * @return Backend outcome; secrets must remain inside the adapter.
         */
        [[nodiscard]] virtual Result<void> Sign(const std::filesystem::path &stageRoot, ReleaseCredentialHandle credential) = 0;
    };

    /**
     * @brief Rejects unauthorized or changed unsigned input before invoking a signing backend.
     * @param request Frozen plan, pre-sign inventory, private stage, and selected handle.
     * @param backend Host-owned platform signer; invoked only after complete input verification.
     * @return Backend result or a typed input failure. Final signed bytes still require final verification.
     */
    [[nodiscard]] Result<void> SignVerifiedReleaseStage(const ReleaseSigningRequest &request, IReleaseSigningBackend &backend);
}  // namespace Horo::Release
