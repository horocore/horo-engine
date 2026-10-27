#include "Horo/Security/ArtifactSignatureEnvelope.h"

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<std::underlying_type_t<Horo::Security::SignatureAlgorithm>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(Horo::Security::SignatureAlgorithm::EcdsaP256Sha256) == 0);
static_assert(std::is_same_v<decltype(std::declval<Horo::Security::DetachedSignatureEnvelope>().artifactDigest), Horo::Sha256Digest>);
static_assert(std::is_same_v<decltype(std::declval<Horo::Security::DetachedSignatureEnvelope>().signature), std::vector<std::byte>>);
static_assert(std::is_move_constructible_v<Horo::Security::DetachedSignatureEnvelope>);
