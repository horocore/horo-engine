#include "HeadlessProjectScene.h"

#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectAiSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectNetworkSettings.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Platform/NativeProjectIoOwners.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "ProjectSceneNavigationComposition.h"

#include <utility>

namespace Horo::Application::Internal {
    namespace Net = Network;

    namespace {

        /** @brief Retains a real source capture's content identity; this is not a cooked durable revision. */
        Runtime::SceneDefinitionRevision SourceCaptureRevision(const std::string_view bytes) {
            const auto digest = ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}));
            std::uint64_t value{};
            for (std::size_t index = 0; index < sizeof(value); ++index)
                value |= static_cast<std::uint64_t>(digest.bytes[index]) << (index * 8U);
            return {value == 0 ? 1 : value};
        }

        Result<void> CaptureProductSourceIo(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources,
                                            const ProjectMetadata &metadata) {
            resources.sourceContext = {.projectIdentity = metadata.projectId,
                                       .projectGeneration = 1,
                                       .cancellation = resources.cancellation.Token(),
                                       .deadline = resources.deadline};  // One explicit launch owner.
            if (launch.publicationNamespace.empty()) {
                auto source = Platform::CreateNativeProjectReadFiles(launch.projectRoot, metadata.projectId, 1);
                if (source.HasError())
                    return Result<void>::Failure(source.ErrorValue());
                resources.sourceFiles = std::move(source).Value();
                return Result<void>::Success();
            }
            auto io = Platform::CreateNativeProjectIoOwners(launch.projectRoot, metadata.projectId, 1, ".horo/headless/cache",
                                                            launch.publicationNamespace);
            if (io.HasError())
                return Result<void>::Failure(io.ErrorValue());
            resources.sourceFiles = io.Value().source;
            resources.publication = io.Value().publication;
            return Result<void>::Success();
        }

        /** @brief Uses root-owned native reads and canonical conversion; no second Scene parser or asset authority. */
        Result<Runtime::RuntimeSceneDefinition> ReadProductSceneSource(const HeadlessProjectSceneOptions &launch,
                                                                       HeadlessProjectSceneResources &resources) {
            const std::string_view path = std::string_view{launch.scene}.substr(std::string_view{"source:"}.size());
            const auto registry = resources.registry.Snapshot();
            const auto *record = registry.FindByPath(path);
            if (!record || record->type.Value() != "core.scene")
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid,
                              "The source Scene must be an exact registered core.scene project path."));
            auto read = resources.sourceFiles->SourceText(path, resources.sourceContext, {.maximumFileBytes = 16U * 1024U * 1024U});
            if (read.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(read.ErrorValue());
            auto source = SceneSource::DecodeSceneSource(*read.Value().text);
            if (source.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(source.ErrorValue());
            auto converted = SceneSource::ConvertSceneSourceToRuntime({source.Value().objects, source.Value().prefabInstances},
                                                                      SceneCook::SceneDefinitionIdForAsset(record->id),
                                                                      SourceCaptureRevision(*read.Value().text));
            if (converted.HasValue())
                resources.authoredSource.emplace(std::move(source).Value());
            return converted;
        }

        /** @brief Resolves only the selected actual Scene envelope; embedded revision is admitted by Scene's canonical decoder. */
        Result<Runtime::RuntimeSceneDefinition> ReadProductCookedScene(const HeadlessProjectSceneOptions &launch,
                                                                       HeadlessProjectSceneResources &resources,
                                                                       const CommittedSceneAssets *committed) {
            if (!committed)
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Cooked Scene selection requires --cooked-root and --cook-target."));
            auto id = Assets::AssetId::Parse(std::string_view{launch.scene}.substr(std::string_view{"cooked:"}.size()));
            if (id.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(id.ErrorValue());
            auto decoded = committed->DecodeScene(id.Value());
            if (decoded.HasValue())
                resources.sceneAsset = id.Value();
            return decoded;
        }

        /** @brief Publishes the complete project identity registry before admitting either authored or cooked Scenes. */
        Result<void> CaptureProductRegistry(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources) {
            auto candidate = Assets::PrepareAssetRegistryCandidate(launch.projectRoot);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            if (const auto published = resources.registry.Publish(std::move(candidate).Value());
                published.status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<void>::Failure(published.diagnostics.empty() ? MakeError(Net::NetworkErrors::NetworkModeInvalid)
                                                                           : published.diagnostics.front().error);
            return Result<void>::Success();
        }

        /** @brief Resolves the admitted Scene once before retaining its selected simulation payloads. */
        Result<void> CaptureProductDefinition(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources,
                                              const CommittedSceneAssets *committed) {
            auto definition = launch.scene.starts_with("source:") ? ReadProductSceneSource(launch, resources)
                                                                  : ReadProductCookedScene(launch, resources, committed);
            if (definition.HasError())
                return Result<void>::Failure(definition.ErrorValue());
            resources.definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition).Value());
            return Result<void>::Success();
        }

        /** @brief Keeps only reachable runtime bytes when the dedicated composition supplies a typed payload policy. */
        Result<void> CaptureProductAssets(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources) {
            if (launch.cookedRoot.empty())
                return Result<void>::Success();
            auto target = Assets::AssetCookTargetId::Parse(launch.cookTarget);
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            SceneAssetRetention retain;
            if (launch.simulationAssets) {
                retain = [&](const CommittedSceneAssets &committed) {
                    if (auto definition = CaptureProductDefinition(launch, resources, &committed); definition.HasError())
                        return Result<std::vector<Assets::AssetId>>::Failure(definition.ErrorValue());
                    return SelectHeadlessSimulationAssets(committed, *resources.definition, resources.sceneAsset, *launch.simulationAssets,
                                                          resources.prefabPolicy, resources.load.cancellation);
                };
            }
            const NativeDurableFileSystem cookFiles;
            auto committed = CommittedSceneAssets::Capture(cookFiles, {launch.cookedRoot, std::move(target).Value()},
                                                           resources.registry.Snapshot(), retain);
            if (committed.HasError())
                return Result<void>::Failure(committed.ErrorValue());
            resources.committed = std::move(committed).Value();
            return Result<void>::Success();
        }

        /** @brief Captures real project schema only for explicit Canonical policy; Null remains explicit. */
        Result<void> CaptureProductPhysics(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources,
                                           const std::string_view projectId) {
            auto physics = Physics::PhysicsWorldSettings::Capture({});
            auto character = Character::CharacterWorldSettings::Capture({});
            if (physics.HasError() || character.HasError())
                return Result<void>::Failure(physics.HasError() ? physics.ErrorValue() : character.ErrorValue());
            Physics::PhysicsWorldSimulationBinding simulation;
            if (launch.physicsMode == Physics::PhysicsRuntimeMode::Canonical) {
                auto collision = LoadProjectCollisionSettings(launch.projectRoot, projectId);
                if (collision.HasError())
                    return Result<void>::Failure(collision.ErrorValue());
                resources.collision = std::move(collision).Value().settings;
                auto captured = resources.collision->Capture(1);  // This CLI creates one explicit host generation.
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                simulation = std::move(captured).Value();
                auto surfaces = LoadProjectSurfaceSettings(launch.projectRoot, projectId);
                if (surfaces.HasError())
                    return Result<void>::Failure(std::move(surfaces).ErrorValue());
                auto semantic = surfaces.Value().settings->Capture(simulation.generation);
                if (semantic.HasError())
                    return Result<void>::Failure(std::move(semantic).ErrorValue());
                simulation.surfaces = std::move(semantic).Value();
            }
            resources.settings.emplace(std::move(physics).Value(), std::move(character).Value(), std::move(simulation));
            return Result<void>::Success();
        }

        /** @brief Creates the load services after the immutable definition and committed providers are captured. */
        void PrepareProductLoaders(HeadlessProjectSceneResources &resources) {
            resources.loads =
                std::make_unique<Assets::AssetLoadService>(resources.jobs,
                                                           resources.committed ? resources.committed->SceneProvider() : resources.empty);
            if (resources.committed)
                resources.prefabLoads = std::make_unique<Assets::AssetLoadService>(resources.jobs, resources.committed->ArtifactProvider());
        }

        /** @brief Captures the canonical project root and its selected owner policies before opening the Scene. */
        Result<ProjectMetadata> CaptureProductProject(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources) {
            using Metadata = Result<ProjectMetadata>;
            auto root = LoadProjectSourceDocument(launch.projectRoot);
            if (root.HasError())
                return Metadata::Failure(root.ErrorValue());
            const auto ai = ResolveProjectAiSettings(root.Value());
            if (ai.HasError())
                return Metadata::Failure(ai.ErrorValue());
            if (const auto &policy = ai.Value()) {
                resources.decisionScheduler = policy->Scheduler();
                resources.perceptionWorkUnitsLimit = policy->PerceptionWorkUnitsLimit();
                resources.developmentReplayAllowed =
                    policy->AllowsDebug(AiProjectRuntimeKind::Development, AiProjectDebugOperation::ReplayCapture);
            }
            if (launch.captureNetworkSettings) {
                auto settings = ResolveProjectNetworkSettings(root.Value());
                if (settings.HasError())
                    return Metadata::Failure(settings.ErrorValue());
                resources.networkSettings = std::move(settings).Value();
            }
            auto metadata = std::move(root).Value().metadata;
            if (auto sources = CaptureProductSourceIo(launch, resources, metadata); sources.HasError())
                return Metadata::Failure(sources.ErrorValue());
            const auto prefabSettings = LoadPrefabProjectSettings(launch.projectRoot);
            if (prefabSettings.HasError())
                return Metadata::Failure(prefabSettings.ErrorValue());
            resources.prefabPolicy = prefabSettings.Value().settings->Limits().Policy();
            return Metadata::Success(std::move(metadata));
        }

    }  // namespace

    /** @copydoc PrepareHeadlessProjectSceneResources */
    Result<std::unique_ptr<HeadlessProjectSceneResources>> PrepareHeadlessProjectSceneResources(const HeadlessProjectSceneOptions &launch) {
        auto resources = std::make_unique<HeadlessProjectSceneResources>(launch.cpuWorkerCount);
        // One absolute budget covers source/cook capture and all roles; no per-record reset.
        resources->deadline = std::chrono::steady_clock::now() + launch.loadTimeout;
        resources->load = {resources->cancellation.Token(), resources->deadline};
        auto metadata = CaptureProductProject(launch, *resources);
        if (metadata.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(metadata.ErrorValue());
        if (const auto registry = CaptureProductRegistry(launch, *resources); registry.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(registry.ErrorValue());
        if (const auto assets = CaptureProductAssets(launch, *resources); assets.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(assets.ErrorValue());
        if (!resources->definition)
            if (const auto definition = CaptureProductDefinition(launch, *resources, resources->committed.get()); definition.HasError())
                return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(definition.ErrorValue());
        if (!resources->definition->AssetDependencies().empty() && !resources->committed)
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(
                MakeError(Net::NetworkErrors::NetworkModeInvalid,
                          "This Scene requires committed asset payloads; provide --cooked-root and --cook-target."));
        if (!launch.perceptionPolicyPath.empty()) {
            auto policy = CaptureProjectScenePerception(*resources->sourceFiles, resources->sourceContext, launch.perceptionPolicyPath,
                                                        resources->sceneAsset, *resources->definition, resources->perceptionWorkUnitsLimit);
            if (policy.HasError())
                return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(policy.ErrorValue());
            resources->perceptionPolicy = std::move(policy).Value();
        }
        if (launch.navigationMode == HeadlessNavigationMode::RecastDetour) {
            const auto *artifact = resources->committed ? resources->committed->Find(resources->sceneAsset) : nullptr;
            if (!artifact)
                return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(
                    MakeError(Navigation::NavigationErrors::NoNavigationData));
            auto navigation =
                CaptureProjectSceneNavigation({*resources->sourceFiles, resources->sourceContext, resources->registry.Snapshot(),
                                               *resources->definition, resources->sceneAsset, artifact->target, resources->jobs});
            if (navigation.HasError())
                return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(navigation.ErrorValue());
            resources->navigation = std::move(navigation).Value();
        }
        PrepareProductLoaders(*resources);
        if (const auto settings = CaptureProductPhysics(launch, *resources, metadata.Value().projectId); settings.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(settings.ErrorValue());
        return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Success(std::move(resources));
    }

}  // namespace Horo::Application::Internal
