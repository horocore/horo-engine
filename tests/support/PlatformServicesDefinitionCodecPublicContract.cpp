#include <Horo/Application/PlatformServicesAuthoring.h>
#include <Horo/Application/PlatformServicesDefinitionCodec.h>
#include <Horo/Application/PlatformServicesProjectCook.h>
#include <Horo/Application/PlatformServicesProjectSources.h>
#include <concepts>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::PlatformServices;
    static_assert(requires(const PlatformServicesProjectCookRequest &request, const Assets::AssetSourceIdentity &expected,
                           const ExclusiveFileLock &lease, DurableFileSystem &files, const PlatformProviderManifestCookOutput &output,
                           std::span<const std::byte> bytes, const Sha256Digest &fingerprint) {
        {
            CookAndPublishPlatformServicesProject(request, expected, lease, files)
        } -> std::same_as<Result<PlatformServicesProjectCookPublication>>;
        { EncodePlatformProviderManifestCookGeneration(output) } -> std::same_as<Result<std::vector<std::byte>>>;
        { DecodePlatformProviderManifestCookGeneration(bytes, fingerprint) } -> std::same_as<Result<PlatformProviderManifestCookOutput>>;
    });
    static_assert(requires(const std::filesystem::path &root, const ProjectCompatibilitySnapshot &admission,
                           const PlatformServicesProjectSourceSnapshot &snapshot) {
        { CapturePlatformServicesProjectSources(root, admission, {}) } -> std::same_as<Result<PlatformServicesProjectSourceSnapshot>>;
        { RevalidatePlatformServicesProjectSources(snapshot) } -> std::same_as<Result<void>>;
        { snapshot.SourceFingerprint() } -> std::same_as<const Sha256Digest &>;
    });
    static_assert(requires(std::string_view source, const PlatformStableIdRegistry &ids, const AchievementDefinitionRegistry &achievements,
                           const StatDefinitionRegistry &stats, const LeaderboardDefinitionRegistry &leaderboards,
                           const PresenceDefinitionRegistry &presence) {
        {
            PreparePlatformServicesInitialDocuments(source, PlatformServicesIdSalt{})
        } -> std::same_as<Result<PlatformServicesInitialDocuments>>;
        { DecodePlatformAchievementDefinitions(source, ids) } -> std::same_as<Result<AchievementDefinitionRegistry>>;
        { DecodePlatformStatDefinitions(source, ids) } -> std::same_as<Result<StatDefinitionRegistry>>;
        { DecodePlatformLeaderboardDefinitions(source, ids, stats) } -> std::same_as<Result<LeaderboardDefinitionRegistry>>;
        { DecodePlatformPresenceDefinitions(source, ids) } -> std::same_as<Result<PresenceDefinitionRegistry>>;
        { EncodePlatformAchievementDefinitions(achievements) } -> std::same_as<Result<std::string>>;
        { EncodePlatformStatDefinitions(stats) } -> std::same_as<Result<std::string>>;
        { EncodePlatformLeaderboardDefinitions(leaderboards) } -> std::same_as<Result<std::string>>;
        { EncodePlatformPresenceDefinitions(presence) } -> std::same_as<Result<std::string>>;
    });
}  // namespace
