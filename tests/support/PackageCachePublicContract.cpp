#include "Horo/Packages/PackageCache.h"

#include <type_traits>
#include <utility>

using Cache = Horo::Packages::PackageCacheStore;
using Archive = Horo::Packages::ValidatedPackageArchive;
using Envelope = Horo::Security::DetachedSignatureEnvelope;
static_assert(std::is_same_v<decltype(std::declval<Cache &>().Publish(std::declval<const Archive &>(), std::declval<const Envelope &>())),
                             Horo::Result<Horo::Packages::PackageCacheEntry>>);
static_assert(std::is_same_v<decltype(std::declval<Cache &>().LoadWithPublisherEvidence(std::declval<const Horo::Sha256Digest &>())),
                             Horo::Result<std::optional<Horo::Packages::PackageCachedArtifact>>>);
