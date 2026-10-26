#pragma once
/** @file VerifiedShaderHostCatalog.h @brief App-owned authenticated immutable shader-tool approvals. */
#include "Horo/Security/ArtifactSignature.h"
#include "ShaderCompilerToolchain.h"

namespace Horo::Application::Internal {
    /** @brief One exact signed approval tuple; paths are supplied independently by the host. */
    struct ShaderHostToolApproval final {
        Render::ShaderCompilerToolIdentity identity;
        Sha256Digest executableDigest;
    };

    /** @brief Only ArtifactVerifier-authenticated immutable snapshots can issue this existing compiler trust capability. */
    class VerifiedShaderHostCatalog final : public Render::IVerifiedShaderCompilerToolCatalog {
        class ConstructionKey final {
            friend class VerifiedShaderHostCatalog;
            ConstructionKey() noexcept = default;

        public:
            ConstructionKey(const ConstructionKey &) noexcept = default;
        };

    public:
        /** @brief Factory-only construction after bounded catalog authentication and decoding. */
        VerifiedShaderHostCatalog(ConstructionKey, Security::VerifiedArtifactEvidence evidence, std::string host,
                                  std::vector<ShaderHostToolApproval> tools);
        /** @brief Authenticates exact bounded bytes before decoding any tool approvals.
         * @param bytes Immutable catalog snapshot, at most64KiB, schema1.
         * @param envelope Detached signature for these exact bytes.
         * @param verifier Existing security verifier over independently host-approved roots, never roots from bytes.
         * @param actualHost Actual host-composition platform identity, not a project/catalog-selected override.
         * @return Immutable catalog or original security failure/typed closed-schema error; no processes or files are touched.
         */
        [[nodiscard]] static Result<std::shared_ptr<const VerifiedShaderHostCatalog>> Create(
            std::span<const std::byte> bytes, const Security::DetachedSignatureEnvelope &envelope,
            const Security::ArtifactVerifier &verifier, std::string_view actualHost);
        /** @copydoc Render::IVerifiedShaderCompilerToolCatalog::Approves */
        [[nodiscard]] bool Approves(std::string_view hostPlatform, const Render::ShaderCompilerToolIdentity &identity,
                                    const Sha256Digest &executableDigest) const noexcept override;
        /** @brief Borrows exact signed tuples for constructing explicit installed-tool requests. @return Immutable approvals. */
        [[nodiscard]] std::span<const ShaderHostToolApproval> Tools() const noexcept;
        /** @brief Releases immutable approvals; never modifies independent host roots or tool installations. */
        ~VerifiedShaderHostCatalog() override = default;

    private:
        Security::VerifiedArtifactEvidence evidence_;
        std::string host_;
        std::vector<ShaderHostToolApproval> tools_;
    };
}  // namespace Horo::Application::Internal
