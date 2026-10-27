#pragma once
/** @file ArtifactSignatureEnvelope.h
 * @brief Neutral detached-signature data only; no verifier, trust, filesystem or provider authority.
 */
#include "Horo/Foundation/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Horo::Security {
    /** @brief Approved detached-signature algorithms understood by the security boundary. */
    enum class SignatureAlgorithm : std::uint8_t {
        EcdsaP256Sha256 /**< SEC1 P-256 ECDSA over a SHA-256 payload digest. */
    };

    /** @brief Detached signature envelope bound to one exact artifact digest and signing identity. */
    struct DetachedSignatureEnvelope {
        SignatureAlgorithm algorithm{SignatureAlgorithm::EcdsaP256Sha256}; /**< Declared verification algorithm. */
        std::string publisherId;                                           /**< Stable publisher identity. */
        std::string keyId;                                                 /**< Publisher-scoped signing key identity. */
        Sha256Digest artifactDigest;                                       /**< Digest of the exact signed artifact bytes. */
        std::vector<std::byte> signature;                                  /**< Provider-specific detached signature bytes. */
    };

}  // namespace Horo::Security
