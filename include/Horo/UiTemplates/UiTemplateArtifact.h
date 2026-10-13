#pragma once

/** @file UiTemplateArtifact.h
 * @brief Bounded runtime-only ordinary/rich artifact transfer, independent of template source/catalog authority.
 */
#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

namespace Horo::UiTemplates {
    /** @brief Encodes fixed protocol version/algorithm, exact cache commitments and owned ordinary/rich bytes.
     * @param document Complete cooked projection. @param limits Admitted aggregate content policy.
     * @return Canonical artifact or a typed capacity/integrity failure.
     */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeTemplateArtifact(const UiTemplateCookedDocument &document,
                                                                           const UiTemplateLimits &limits);

    /** @brief Decodes strict bounded runtime-only bytes and verifies their complete canonical cache commitment.
     * @param bytes Untrusted artifact; borrowed only during decoding.
     * @param verifiedManifest Exact manifest obtained from the application's Assets/Packages authority.
     * @param limits Admitted aggregate content policy; cannot exceed compiled ceilings.
     * @return Owned ordinary/rich output or a typed version/shape/integrity/target/capacity failure.
     * @details No linked records, source templates, catalog leases, callbacks or ambient resource lookup are accepted.
     */
    [[nodiscard]] Result<UiTemplateCookedDocument> DecodeTemplateArtifact(std::span<const std::uint8_t> bytes,
                                                                          const Sha256Digest &verifiedManifest,
                                                                          const UiTemplateLimits &limits);
}  // namespace Horo::UiTemplates
