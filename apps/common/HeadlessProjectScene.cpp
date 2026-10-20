#include "HeadlessProjectScene.h"

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

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

        /** @brief Uses root-owned native reads and canonical conversion; no second Scene parser or asset authority. */
        Result<Runtime::RuntimeSceneDefinition> ReadProductSceneSource(const HeadlessProjectSceneOptions &launch,
                                                                       const HeadlessProjectSceneResources &resources,
                                                                       const ProjectMetadata &metadata) {
            auto files = Platform::CreateNativeProjectReadFiles(launch.projectRoot, metadata.projectId, 1);
            if (files.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(files.ErrorValue());
            const std::string_view path = std::string_view{launch.scene}.substr(std::string_view{"source:"}.size());
            const auto registry = resources.registry.Snapshot();
            const auto *record = registry.FindByPath(path);
            if (!record || record->type.Value() != "core.scene")
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid,
                              "The source Scene must be an exact registered core.scene project path."));
            const Platform::ProjectReadContext context{.projectIdentity = metadata.projectId,
                                                       .projectGeneration = 1,
                                                       .cancellation = resources.cancellation.Token(),
                                                       .deadline = resources.deadline};
            auto read = files.Value()->SourceText(path, context, {.maximumFileBytes = 16U * 1024U * 1024U});
            if (read.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(read.ErrorValue());
            auto source = SceneSource::DecodeSceneSource(*read.Value().text);
            if (source.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(source.ErrorValue());
            return SceneSource::ConvertSceneSourceToRuntime({source.Value().objects, source.Value().prefabInstances},
                                                            SceneCook::SceneDefinitionIdForAsset(record->id),
                                                            SourceCaptureRevision(*read.Value().text));
        }

        /** @brief Resolves only the selected actual Scene envelope; embedded revision is admitted by Scene's canonical decoder. */
        Result<Runtime::RuntimeSceneDefinition> ReadProductCookedScene(const HeadlessProjectSceneOptions &launch,
                                                                       const HeadlessProjectSceneResources &resources) {
            if (!resources.committed)
                return Result<Runtime::RuntimeSceneDefinition>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Cooked Scene selection requires --cooked-root and --cook-target."));
            auto id = Assets::AssetId::Parse(std::string_view{launch.scene}.substr(std::string_view{"cooked:"}.size()));
            if (id.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(id.ErrorValue());
            return resources.committed->DecodeScene(id.Value());
        }

        /** @brief Captures complete registry state and an explicitly selected verified cook generation. */
        Result<void> CaptureProductAssets(const HeadlessProjectSceneOptions &launch, HeadlessProjectSceneResources &resources) {
            auto candidate = Assets::PrepareAssetRegistryCandidate(launch.projectRoot);
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            if (const auto published = resources.registry.Publish(std::move(candidate).Value());
                published.status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<void>::Failure(published.diagnostics.empty() ? MakeError(Net::NetworkErrors::NetworkModeInvalid)
                                                                           : published.diagnostics.front().error);
            if (!launch.cookedRoot.empty()) {
                auto target = Assets::AssetCookTargetId::Parse(launch.cookTarget);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                const NativeDurableFileSystem cookFiles;
                auto committed =
                    CommittedSceneAssets::Capture(cookFiles, {launch.cookedRoot, std::move(target).Value()}, resources.registry.Snapshot());
                if (committed.HasError())
                    return Result<void>::Failure(committed.ErrorValue());
                resources.committed = std::move(committed).Value();
            }
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
            }
            resources.settings.emplace(std::move(physics).Value(), std::move(character).Value(), std::move(simulation));
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc PrepareHeadlessProjectSceneResources */
    Result<std::unique_ptr<HeadlessProjectSceneResources>> PrepareHeadlessProjectSceneResources(const HeadlessProjectSceneOptions &launch) {
        auto resources = std::make_unique<HeadlessProjectSceneResources>();
        // One absolute budget covers source/cook capture and all roles; no per-record reset.
        resources->deadline = std::chrono::steady_clock::now() + launch.loadTimeout;
        resources->load = {resources->cancellation.Token(), resources->deadline};
        auto metadata = LoadProjectMetadata(launch.projectRoot);
        if (metadata.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(metadata.ErrorValue());
        if (const auto assets = CaptureProductAssets(launch, *resources); assets.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(assets.ErrorValue());
        auto definition = launch.scene.starts_with("source:") ? ReadProductSceneSource(launch, *resources, metadata.Value())
                                                              : ReadProductCookedScene(launch, *resources);
        if (definition.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(definition.ErrorValue());
        if (!definition.Value().AssetDependencies().empty() && !resources->committed)
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(
                MakeError(Net::NetworkErrors::NetworkModeInvalid,
                          "This Scene requires committed asset payloads; provide --cooked-root and --cook-target."));
        resources->definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition).Value());
        resources->loads =
            std::make_unique<Assets::AssetLoadService>(resources->jobs,
                                                       resources->committed ? resources->committed->Provider() : resources->empty);
        if (resources->committed)
            resources->prefabLoads = std::make_unique<Assets::AssetLoadService>(resources->jobs, resources->committed->ArtifactProvider());
        if (const auto settings = CaptureProductPhysics(launch, *resources, metadata.Value().projectId); settings.HasError())
            return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Failure(settings.ErrorValue());
        return Result<std::unique_ptr<HeadlessProjectSceneResources>>::Success(std::move(resources));
    }

}  // namespace Horo::Application::Internal
