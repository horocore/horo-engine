#include "NativeNavigationBakeComposition.h"

#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Platform/NativeProjectIoOwners.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>

namespace Horo::Application::Internal {
    namespace {
        namespace Nav = Navigation;

        /** @brief Generates only private operation namespace UUIDs from native entropy, never authored asset identities. */
        Result<Assets::AssetId> NewOperation() {
            std::array<std::byte, 16> bytes{};
            auto entropy = Platform::CreateNativeSecureRandomSource();
            if (auto filled = entropy->Fill(bytes); filled.HasError())
                return Result<Assets::AssetId>::Failure(filled.ErrorValue());
            bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
            bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
            std::array<std::uint8_t, 16> identity{};
            std::ranges::transform(bytes, identity.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(identity));
        }

        /** @brief Hashes explicit versioned host semantics without test-only provider stamps. */
        Sha256Digest Digest(std::string_view text) {
            return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
        }

        /** @brief Defines the one native headless output namespace, provider and bounded bake budget. */
        NavigationBakeServiceConfig ServiceConfig(Assets::AssetId definition, std::shared_ptr<const ImmutableFileStorage> cache,
                                                  std::shared_ptr<DurableFileSystem> publication, const std::filesystem::path &output,
                                                  std::shared_ptr<const Navigation::INavigationMeshBuilder> builder) {
            return {.definition = definition,
                    .artifactType = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName).Value(),
                    .target = AssetCookTargetId::Parse("headless-null").Value(),
                    .cacheFiles = std::move(cache),
                    .targetRoot = output,
                    .builder = std::move(builder),
                    .files = std::move(publication),
                    .budget = {1, 512ULL * 1024ULL * 1024ULL, 128ULL * 1024ULL * 1024ULL, 8, 1024ULL * 1024ULL * 1024ULL,
                               Duration::FromMilliseconds(30'000)},
                    .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                    .newOperationId = NewOperation};
        }

    }  // namespace

    /** @copydoc CaptureNativeNavigationBakeSource */
    Result<std::shared_ptr<const NavigationProjectBakeSource>> CaptureNativeNavigationBakeSource(
        std::shared_ptr<const Platform::IProjectReadFiles> files, const ProjectCompatibilitySnapshot &compatibility,
        std::shared_ptr<std::atomic<bool>> closed, const NavigationBakeInvocation &invocation) {
        if (!compatibility.metadata || !closed)
            return Result<std::shared_ptr<const NavigationProjectBakeSource>>::Failure(MakeError(Nav::NavigationErrors::BakeInputInvalid));
        Platform::ProjectReadContext read{.projectIdentity = compatibility.metadata->projectId,
                                          .projectGeneration = 1,
                                          .cancellation = invocation.cancellation,
                                          .deadline = invocation.deadline,
                                          .authorityStopped = [closed = std::move(closed), stopped = invocation.authorityStopped] {
            return closed->load() || (stopped && stopped());
        }};
        auto source = NavigationProjectBakeSource::Capture(std::move(files), std::move(read), compatibility);
        if (source.HasError())
            return source;
        if (source.Value()->Settings().Input().defaultProvider != "recast_detour" || source.Value()->Packages().intent ||
            source.Value()->Packages().lock)
            return Result<std::shared_ptr<const NavigationProjectBakeSource>>::Failure(
                MakeError(Nav::NavigationErrors::OperationUnsupported,
                          "The native navigation command requires the explicit Recast provider and no unadmitted package source"));
        return source;
    }

    /** @copydoc ComposeNativeNavigationBake */
    Result<NavigationBakeApplicationConfig> ComposeNativeNavigationBake(const std::filesystem::path &root, Assets::AssetId definition,
                                                                        ProjectCompatibilitySnapshot compatibility,
                                                                        std::shared_ptr<std::atomic<bool>> closed,
                                                                        std::chrono::steady_clock::time_point deadline) {
        using Return = Result<NavigationBakeApplicationConfig>;
        if (!compatibility.metadata || !definition.IsValid() || !closed)
            return Return::Failure(MakeError(Navigation::NavigationErrors::BakeInputInvalid));
        const auto relative = std::filesystem::path{definition.ToString()} / "headless-null";
        const auto cacheNamespace = (std::filesystem::path{".horo/cache/navigation"} / relative).generic_string();
        const auto publicationNamespace = (std::filesystem::path{".horo/cooked/navigation"} / relative).generic_string();
        auto owners =
            Platform::CreateNativeProjectIoOwners(root, compatibility.metadata->projectId, 1, cacheNamespace, publicationNamespace);
        if (owners.HasError())
            return Return::Failure(owners.ErrorValue());
        const auto output = root / publicationNamespace;
        if (const auto admitted = owners.Value().publication->InspectEntry(output); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        auto builder = Navigation::CreateRecastDetourNavigationMeshBuilder();
        if (builder.HasError())
            return Return::Failure(builder.ErrorValue());
        return Return::Success(
            {.service = ServiceConfig(definition, owners.Value().cache, owners.Value().publication, output, std::move(builder).Value()),
             .compatibility = {.provider = Digest(HORO_NAVIGATION_BUILD_FINGERPRINT),
                               .schemas = Digest("horo.navigation.automation.v1;scene3;navdef1;hnt2;hns4;canonical-rh-y-up-meters"),
                               .settings = Digest("horo.navigation.command.v1;headless-null;recast_detour")},
             .captureSource = [files = owners.Value().source, compatibility = std::move(compatibility), closed = std::move(closed),
                               deadline](const NavigationBakeInvocation &invocation) {
            auto bounded = invocation;
            bounded.deadline = std::min(deadline, bounded.deadline);
            return CaptureNativeNavigationBakeSource(files, compatibility, closed, bounded);
        }});
    }
}  // namespace Horo::Application::Internal
