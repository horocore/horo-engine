#pragma once
/** @file PackageCachePublisherEnvelope.h @brief Target-private inert storage encoding; verification remains in PackageSecurity. */
#include "Horo/Security/ArtifactSignatureEnvelope.h"

namespace Horo::Packages::CacheDetail {
    inline constexpr std::size_t MaximumPublisherEnvelopeBytes = 2048;
    /** @brief Encodes bounded exact detached bytes without granting trust. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodePublisherEnvelope(const Security::DetachedSignatureEnvelope &signature,
                                                                         const Sha256Digest &digest);
    /** @brief Rejects malformed, unsupported or differently bound storage before returning an untrusted envelope. */
    [[nodiscard]] Result<Security::DetachedSignatureEnvelope> DecodePublisherEnvelope(std::span<const std::byte> bytes,
                                                                                      const Sha256Digest &digest);
}  // namespace Horo::Packages::CacheDetail
