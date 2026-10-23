#include "WorldBakeInternal.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>

namespace Horo::Application::WorldBakeDetail {
    /** @brief Preserves the original typed cause and attaches the exact host-authorized page origin. */
    Error AtSource(Error error, const std::optional<DiagnosticSourceLocation> &source) {
        if (source)
            error.diagnostics.push_back({.code = DiagnosticCode{error.code.Value()},
                                         .message = error.message,
                                         .location = {source->absolutePath, source->line, source->column}});
        return error;
    }

    namespace {
        /** @brief Bounds every independently allocating stage, including empty-world admission. */
        bool ValidLimits(const WorldBakeLimits &value) {
            constexpr WorldBakeLimits hard;
            const auto ceilings = std::to_array<std::pair<std::size_t, std::size_t>>(
                {{value.maximumObjects, hard.maximumObjects},
                 {value.maximumCells, hard.maximumCells},
                 {value.maximumAssignments, hard.maximumAssignments},
                 {value.maximumCellsPerObject, hard.maximumCellsPerObject},
                 {value.maximumOutputBytes, hard.maximumOutputBytes},
                 {value.maximumSourceBytes, hard.maximumSourceBytes},
                 {value.cache.maximumCells, hard.cache.maximumCells},
                 {value.cache.maximumDependencies, hard.cache.maximumDependencies},
                 {value.cache.maximumDependencyEdges, hard.cache.maximumDependencyEdges},
                 {value.cache.maximumSchemas, hard.cache.maximumSchemas},
                 {value.cache.maximumKeyBytes, hard.cache.maximumKeyBytes},
                 {value.cache.maximumRetainedBytes, hard.cache.maximumRetainedBytes},
                 {value.codec.maximumEncodedBytes, hard.codec.maximumEncodedBytes},
                 {value.codec.maximumDecodeAllocationBytes, hard.codec.maximumDecodeAllocationBytes},
                 {value.codec.semantic.maximumEntities, hard.codec.semantic.maximumEntities},
                 {value.codec.semantic.maximumDependencies, hard.codec.semantic.maximumDependencies},
                 {value.codec.semantic.maximumRetainedBytes, hard.codec.semantic.maximumRetainedBytes},
                 {value.validation.maximumCells, hard.validation.maximumCells},
                 {value.validation.maximumObjects, hard.validation.maximumObjects},
                 {value.validation.maximumDiagnostics, hard.validation.maximumDiagnostics},
                 {value.validation.maximumSourceBytes, hard.validation.maximumSourceBytes},
                 {value.validation.maximumCellBytes, hard.validation.maximumCellBytes},
                 {value.validation.maximumWorldBytes, hard.validation.maximumWorldBytes}});
            return value.validation.maximumReferences <= hard.validation.maximumReferences &&
                   std::ranges::all_of(ceilings, [](const auto &ceiling) {
                return ceiling.first != 0 && ceiling.first <= ceiling.second;
            });
        }

        /** @brief Checks the immutable project authority independently from document ownership. */
        bool ValidProject(const WorldBakeProjectCapture &project) {
            return project.fence.project.IsValid() && project.fence.revision != 0 && project.fence.schemas != Sha256Digest{} &&
                   project.settings != Sha256Digest{};
        }

        /** @brief Checks the canonical document fence without touching the owning editor service. */
        bool ValidDocument(const Editor::PartitionedWorldSnapshotLease &document) {
            return document && document->fence.owner.IsValid() && document->fence.generation.IsValid() &&
                   document->fence.revision.IsValid();
        }

        /** @brief Root capability identity is independent from the mutable project/schema publication. */
        bool ValidSourceAuthority(const WorldBakeSourceAuthority &authority) {
            return !authority.files || (authority.owner.IsValid() && authority.generation != 0);
        }

        /** @brief Checks one exact verified asset identity; graph traversal remains with the existing Scene cache owner. */
        bool ValidDependency(const WorldBakeDependency &dependency) {
            return dependency.asset.id.IsValid() && !dependency.asset.expectedType.Value().empty() && dependency.revision != 0 &&
                   dependency.digest != Sha256Digest{};
        }

        /** @brief Validates inert schema identities without narrowing the owning cooker's supported-version set. */
        bool ValidSchemas(const WorldBakeProjectCapture &project) {
            return std::ranges::all_of(project.components, [](const auto &schema) {
                return !schema.type.Value().empty() && schema.version != 0;
            }) && std::ranges::all_of(project.behaviors, [](const auto &schema) {
                return !schema.type.Value().empty() && schema.version != 0;
            });
        }

        /** @brief Admits one explicit dependency authority; selection and already captured evidence cannot compete. */
        bool ValidDependencySource(const WorldBakeProjectCapture &project) {
            return !project.dependencySource ||
                   (project.dependencies.empty() && project.dependencySource->files && project.dependencySource->targetRoot.is_absolute() &&
                    project.dependencySource->target.IsValid());
        }

        /** @brief Admits complete dependency identities and finite graph storage without resolving provider state. */
        Result<void> AdmitProject(const WorldBakeProjectCapture &project, const WorldBakeLimits &limits) {
            if (project.fence.replication != (project.replication ? project.replication->Fingerprint() : Sha256Digest{}))
                return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
            if (project.replication) {
                auto assessment = project.replication->Assess(Network::ReplicationDeclarationUse::Cook);
                if (assessment.HasError())
                    return Result<void>::Failure(assessment.ErrorValue());
                if (auto admitted = assessment.Value().RequireAdmission(); admitted.HasError())
                    return admitted;
            }
            if (project.dependencies.size() > limits.cache.maximumDependencies || project.components.size() > limits.cache.maximumSchemas ||
                project.behaviors.size() > limits.cache.maximumSchemas - project.components.size())
                return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
            if (!ValidDependencySource(project))
                return Result<void>::Failure(MakeError(WorldBakeErrors::Dependency));
            if (!ValidSchemas(project))
                return Result<void>::Failure(MakeError(WorldBakeErrors::Invalid));
            std::set<Assets::AssetId> dependencies;
            std::size_t edges{};
            for (const auto &dependency : project.dependencies) {
                if (!ValidDependency(dependency) || !dependencies.insert(dependency.asset.id).second)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Dependency));
                if (dependency.dependencies.size() > limits.cache.maximumDependencyEdges - edges)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
                edges += dependency.dependencies.size();
            }
            return Result<void>::Success();
        }

        /** @brief Checks navigable source membership and aggregate path storage before worker handoff. */
        Result<void> AdmitSources(const WorldBakeRequest &request) {
            std::set<Assets::AssetId> sources;
            std::size_t sourceBytes{};
            for (const auto &source : request.sources) {
                if (!source.page.IsValid() || !std::filesystem::path{source.source.absolutePath}.is_absolute() ||
                    !sources.insert(source.page.sourceAsset).second ||
                    std::ranges::find(request.document->document.pages, source.page, &Editor::PartitionedWorldPage::head) ==
                        request.document->document.pages.end() ||
                    (!source.relativePath.empty() && !Platform::IsSafeProjectReadPath(source.relativePath)))
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Invalid));
                if (source.source.absolutePath.size() > request.limits.validation.maximumSourceBytes - sourceBytes)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
                sourceBytes += source.source.absolutePath.size();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Validates immutable capture structure and all explicit budgets before worker allocation. */
    Result<void> Admit(const WorldBakeRequest &request) {
        if (!ValidDocument(request.document) || (request.mode != WorldBakeMode::Bake && request.mode != WorldBakeMode::Validate))
            return Result<void>::Failure(MakeError(WorldBakeErrors::Invalid));
        if (!ValidSourceAuthority(request.sourceAuthority))
            return Result<void>::Failure(MakeError(WorldBakeErrors::Invalid));
        if (!ValidLimits(request.limits) || request.document->document.pages.size() > 4096)
            return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
        if (auto project = ValidateWorldBakeProjectCapture(request.project, request.limits); project.HasError())
            return project;
        if (request.project.dependencySource && request.registry.Revision().value != request.project.fence.registryRevision)
            return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
        return AdmitSources(request);
    }

    /** @brief Resolves only host-captured exact page revisions, never paths supplied by diagnostic text. */
    std::optional<DiagnosticSourceLocation> SourceFor(const WorldBakeRequest &request, const Assets::AssetId page) {
        const auto source = std::ranges::find(request.sources, page, [](const auto &value) {
            return value.page.sourceAsset;
        });
        return source == request.sources.end() ? std::nullopt : std::optional{source->source};
    }

}  // namespace Horo::Application::WorldBakeDetail

namespace Horo::Application {
    /** @copydoc ValidateWorldBakeProjectCapture */
    Result<void> ValidateWorldBakeProjectCapture(const WorldBakeProjectCapture &project, const WorldBakeLimits &limits) {
        if (!WorldBakeDetail::ValidProject(project))
            return Result<void>::Failure(MakeError(WorldBakeErrors::Invalid));
        if (!WorldBakeDetail::ValidLimits(limits))
            return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
        return WorldBakeDetail::AdmitProject(project, limits);
    }
}  // namespace Horo::Application
