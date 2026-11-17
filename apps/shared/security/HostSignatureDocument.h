#pragma once
/** @file HostSignatureDocument.h
 * @brief Non-installed application host decoder shared with the existing package authoring command.
 */
#include "Horo/Packages/PackagePublisherVerification.h"

namespace Horo::Application::Internal {
    inline constexpr std::size_t MaximumHostSignatureDocumentBytes = 1U << 20U;

    /** @brief Decodes the existing schema-one detached envelope, without granting trust or verifying a signature.
     * @param bytes Complete bounded author-tool document, owned by the caller for this synchronous operation.
     * @return Complete exact digest/identity/signature value, or typed malformed/depth/duplicate/size failure.
     * @throws std::bad_alloc Preparation allocation failure; no partially decoded state escapes.
     */
    [[nodiscard]] Result<Security::DetachedSignatureEnvelope> DecodeDetachedSignatureDocument(std::span<const std::byte> bytes);

    /** @brief Decodes existing schema-one publisher records; the host must independently approve their source.
     * @param bytes Complete bounded host-supplied trust document, never project-selected trust.
     * @param seed Explicit caller-owned policy; existing records, resource limits and audit capacity are preserved.
     * A fresh host passes an explicitly default-constructed policy.
     * @return Copy of seed with fully validated records appended and the document unsigned policy applied, or a typed failure.
     * Combined existing and incoming records must fit the caller limit before any copy or append.
     * @details Parsing does not approve a root. Package verification and host ArtifactVerifier still enforce their
     * own exact trust/use policy. The decoder neither reads paths nor discovers/configures an ambient root store.
     * @throws std::bad_alloc Preparation allocation failure; prior caller-owned policies remain unchanged.
     */
    [[nodiscard]] Result<Packages::PackagePublisherVerificationPolicy> DecodePublisherTrustDocument(
        std::span<const std::byte> bytes, const Packages::PackagePublisherVerificationPolicy &seed);

    /** @brief Encodes the existing author-tool signature document with unchanged field names/hex representation.
     * @param envelope Exact detached envelope to serialize. @return Deterministic schema-one JSON with trailing newline.
     * @throws std::bad_alloc Tooling allocation failure. No signature or trust authority is issued.
     */
    [[nodiscard]] std::string EncodeDetachedSignatureDocument(const Security::DetachedSignatureEnvelope &envelope);
}  // namespace Horo::Application::Internal
