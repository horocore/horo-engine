#include "EditorTerrainPreparation.h"

#include "Horo/Platform/NativeImmutableFileStorage.h"
#include "Horo/Platform/SecureRandom.h"

#include <new>

namespace Horo::Editor {
    namespace {
        Result<Assets::AssetId> NewIdentity() {
            std::array<std::uint8_t, 16> bytes{};
            auto random = Platform::CreateNativeSecureRandomSource();
            if (auto filled = random->Fill(std::as_writable_bytes(std::span(bytes))); filled.HasError())
                return Result<Assets::AssetId>::Failure(std::move(filled).ErrorValue());
            const auto id = Assets::AssetId::FromBytes(bytes);
            if (!id.IsValid())
                return Result<Assets::AssetId>::Failure(MakeError(Terrain::TerrainErrors::IdentityInvalid));
            return Result<Assets::AssetId>::Success(id);
        }

        /** @brief Explicit offline authoring validation policy, with no renderer/native consumer activation claim. */
        Terrain::TerrainConfigurationSnapshot Configuration() {
            using namespace Terrain;
            return TerrainConfigurationSnapshot::Create({.configuration = TerrainConfigurationRevision::Create(1).Value(),
                                                         .capability = TerrainCapabilityRevision::Create(1).Value(),
                                                         .tier = TerrainFeatureTier::Ultra,
                                                         .limits = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value().limits})
                .Value();
        }

        Application::TerrainAuthoringCookProfile Profile() {
            const auto target = AssetCookTargetId::Parse("terrain-offline-v1").Value();
            const auto text = std::string_view("horo.terrain.authoring.geometry.v1");
            const auto targetText = std::string_view(target.Value());
            Terrain::TerrainSourceArtifactProfile artifacts;
            artifacts.tiles.tier = Terrain::TerrainFeatureTier::Ultra;
            artifacts.tiles.targetDigest = ComputeSha256(std::as_bytes(std::span(targetText)));
            artifacts.tiles.toolchainDigest = ComputeSha256(std::as_bytes(std::span(text)));
            artifacts.tiles.maximumWorkItems = Terrain::TerrainDescriptorHardLimits::WorkItems;
            artifacts.maximumWorkItems = Terrain::TerrainDescriptorHardLimits::WorkItems;
            Application::TerrainAuthoringCookProfile profile{Configuration(), artifacts, target, {}, {}};
            profile.assets.maximumSourceBytes = 64 * 1024 * 1024;
            profile.assets.maximumAssets = 4096;
            profile.assets.maximumConcurrentCooks = 1;
            return profile;
        }

        Result<Application::TerrainAuthoringCookSession> Session(const std::filesystem::path &root,
                                                                 const Application::TerrainAuthoringCookEnvironment &services) {
            using Return = Result<Application::TerrainAuthoringCookSession>;
            try {
                auto environment = services;
                auto cache = Platform::CreateNativeImmutableFileStorage(root, ".horo/terrain-authoring-cache");
                if (cache.HasError())
                    return Return::Failure(std::move(cache).ErrorValue());
                environment.cacheFiles = std::move(cache).Value();
                auto profile = Profile();
                auto cookedRoot = root / ".horo" / "terrain-authoring-cooked" / profile.target.Value();
                auto host = Application::TerrainAuthoringCookHost::Create(std::move(environment));
                if (host.HasError())
                    return Return::Failure(std::move(host).ErrorValue());
                return Return::Success({std::move(host).Value(), std::move(profile), std::move(cookedRoot)});
            } catch (const std::bad_alloc &) {
                return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
            } catch (...) {
                return Return::Failure(MakeError(Application::TerrainAuthoringCookErrors::Failed));
            }
        }
    }  // namespace

    /** @copydoc MakeEditorTerrainCookFactory */
    std::shared_ptr<const Application::TerrainAuthoringCookFactory> MakeEditorTerrainCookFactory(
        JobSystem &jobs, Assets::AssetRegistry &assets, ProjectMutationCoordinator &mutations, DurableFileSystem &files,
        OperationStore &operations, BuildOutputStore &output) {
        Application::TerrainAuthoringCookEnvironment environment{.jobs = &jobs,
                                                                 .registry = &assets,
                                                                 .mutations = &mutations,
                                                                 .publicationFiles =
                                                                     std::shared_ptr<DurableFileSystem>(&files,
                                                                                                        [](DurableFileSystem *) {
        }),
                                                                 .newPublicationOperationId = NewIdentity,
                                                                 .operations = &operations,
                                                                 .buildOutput = &output};
        return std::make_shared<const Application::TerrainAuthoringCookFactory>(
            [environment = std::move(environment)](const std::filesystem::path &root) {
            return Session(root, environment);
        });
    }

    /** @copydoc MakeEditorTerrainAssetFactory */
    std::shared_ptr<const Application::TerrainAuthoringAssetFactory> MakeEditorTerrainAssetFactory(Assets::AssetRegistry &assets,
                                                                                                   ProjectMutationCoordinator &mutations,
                                                                                                   DurableFileSystem &files) {
        return std::make_shared<const Application::TerrainAuthoringAssetFactory>(
            [&assets, &mutations, &files](const Application::TerrainAuthoringAssetRequest &request) {
            return Application::CreateTerrainAuthoringAsset({assets, mutations, files, NewIdentity, Configuration()}, request);
        });
    }
}  // namespace Horo::Editor
