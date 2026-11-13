#include <Horo/PlatformServices/PlatformStableIdRegistry.h>
#include <concepts>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;
    static_assert(requires(const PlatformStableIdRegistry &previous, const Sha256Digest &base,
                           const PlatformStableIdRegistryCandidate &candidate) {
        { BuildPlatformStableIdRegistryReplacement(previous, base, candidate) } -> std::same_as<Result<PlatformStableIdRegistry>>;
        { previous.CanonicalDocument() } -> std::same_as<Result<std::string>>;
    });
}  // namespace
