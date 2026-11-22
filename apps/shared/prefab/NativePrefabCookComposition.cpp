#include "NativePrefabCookComposition.h"

#include "HeadlessMeshCooker.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Platform/NativeProjectIoOwners.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using CookResult = Result<Assets::AssetCookReport>;
        using Candidate = Result<Assets::AssetRegistryCandidate>;

        /** @brief Issue a private publication-attempt UUID from native entropy. */
        Result<Assets::AssetId> NewOperation() {
            std::array<std::uint8_t, 16> bytes{};
            auto entropy = Platform::CreateNativeSecureRandomSource();
            if (auto filled = entropy->Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
                return Result<Assets::AssetId>::Failure(std::move(filled).ErrorValue());
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
            return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(bytes));
        }

        /** @brief Fail closed on degraded canonical sidecars instead of cooking a partial inventory. */
        Candidate CompleteInventory(const Assets::IAssetRegistrySource &source) {
            auto candidate = Assets::PrepareAssetRegistryCandidate(source);
            if (candidate.HasError())
                return candidate;
            const auto &snapshot = candidate.Value();
            if (snapshot.status == Assets::AssetRegistryBuildStatus::Complete)
                return candidate;
            if (!snapshot.diagnostics.empty())
                return Candidate::Failure(snapshot.diagnostics.front().error);
            return Candidate::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }

        /** @brief Re-evaluate the joined invocation's exact pinned inventory before canonical publication. */
        Result<void> CheckInventory(const Assets::IAssetRegistrySource &source, const Assets::AssetRegistrySnapshot &expected) {
            auto current = CompleteInventory(source);
            if (current.HasError())
                return Result<void>::Failure(std::move(current).ErrorValue());
            const auto &records = current.Value().records;
            if (records.size() != expected.Records().size() || std::ranges::any_of(records, [&expected](const auto &record) {
                const auto *prior = expected.Find(record.id);
                return !prior || prior->type != record.type || prior->sourcePath.String() != record.sourcePath.String() ||
                       prior->metadataPath.String() != record.metadataPath.String();
            }))
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc CookNativePrefabProject */
    Result<Assets::AssetCookReport> CookNativePrefabProject(const NativePrefabCookInvocation &invocation,
                                                            const CancellationToken &cancellation) {
        namespace fs = std::filesystem;
        if (invocation.failedSource)
            invocation.failedSource->reset();
        if (cancellation.IsCancellationRequested())
            return CookResult::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
        if (!invocation.project.is_absolute() || invocation.project != invocation.project.lexically_normal() ||
            !invocation.target.IsValid())
            return CookResult::Failure(MakeError(PrefabSceneCookErrors::Invalid));

        const RejectingCompatibilityProofVerifier proof;
        const ProjectCompatibilityInspector compatibility{BuiltInReleaseCompatibilityRegistry(), CurrentEngineReleaseVersion(), proof};
        const auto admitted = compatibility.Inspect(invocation.project);
        if (!admitted.metadata || admitted.markerUpdateRequired ||
            (admitted.status != ProjectCompatibilityStatus::Current &&
             admitted.status != ProjectCompatibilityStatus::CompatibleReleaseLine))
            return CookResult::Failure(admitted.diagnostic.value_or(MakeError(PrefabSceneCookErrors::Invalid)));

        const auto cache = (fs::path{".horo/cache/prefab"} / invocation.target.Value()).generic_string();
        const auto publication = (fs::path{".horo/cooked/prefab"} / invocation.target.Value()).generic_string();
        auto io = Platform::CreateNativeProjectIoOwners(invocation.project, admitted.metadata->projectId, 1, cache, publication);
        if (io.HasError())
            return CookResult::Failure(std::move(io).ErrorValue());
        auto registrySource = CreateProjectAssetRegistrySource(io.Value().source, {.projectIdentity = admitted.metadata->projectId,
                                                                                   .projectGeneration = 1,
                                                                                   .cancellation = cancellation,
                                                                                   .deadline = invocation.deadline});
        if (registrySource.HasError())
            return CookResult::Failure(std::move(registrySource).ErrorValue());
        auto inventory = CompleteInventory(*registrySource.Value());
        if (inventory.HasError())
            return CookResult::Failure(std::move(inventory).ErrorValue());
        Assets::AssetRegistry registry;
        if (registry.Publish(std::move(inventory).Value()).status != Assets::AssetRegistryBuildStatus::Complete)
            return CookResult::Failure(MakeError(PrefabSceneCookErrors::Invalid));

        Assets::CookerCatalog catalog;
        if (auto registered = Assets::RegisterHeadlessMeshCooker(catalog); registered.HasError())
            return CookResult::Failure(std::move(registered).ErrorValue());
        if (invocation.localPolicy) {
            if (invocation.localPolicy->packageTarget != invocation.target)
                return CookResult::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
            auto contribution = MakeLocalPolicyAssetCooker(*invocation.localPolicy);
            if (contribution.HasError())
                return CookResult::Failure(std::move(contribution).ErrorValue());
            if (auto registered = catalog.Register(std::move(contribution).Value()); registered.HasError())
                return CookResult::Failure(std::move(registered).ErrorValue());
        }
        auto published = catalog.Publish();
        if (published.HasError())
            return CookResult::Failure(std::move(published).ErrorValue());

        NativeDurableFileSystem projectMutations;
        SystemWallClock clock;
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 32, .ioWorkerCount = 1}};
        Editor::ProjectMutationCoordinator mutations{projectMutations};
        const Editor::ProjectMigrationTransactionService migrations{projectMutations, clock, mutations, jobs};
        PrefabSceneCookHost host{jobs, std::move(published).Value(), registry, compatibility, mutations, migrations};
        const auto expected = registry.Snapshot();
        const PrefabSceneCookRequest request{.assets = {.sourceRoot = invocation.project,
                                                        .cacheFiles = io.Value().cache,
                                                        .cookedRoot = invocation.project / publication,
                                                        .registry = expected,
                                                        .target = invocation.target,
                                                        .buildOutputStore = invocation.output,
                                                        .operationStore = invocation.operations,
                                                        .publicationFiles = io.Value().publication,
                                                        .newPublicationOperationId = NewOperation,
                                                        .validateHostInputs =
                                                            [&registrySource, &expected] {
            return CheckInventory(*registrySource.Value(), expected);
        },
                                                        .progress = invocation.progress},
                                             .runtimePrefabRoots = invocation.runtimeRoots,
                                             .failedSource = invocation.failedSource};
        return host.Cook(request, cancellation);
    }
}  // namespace Horo::Application::Internal
