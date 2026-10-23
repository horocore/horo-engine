#include "Horo/Application/ProjectNetworkSettings.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "editor/gameplay/ProjectGameplayRegistry.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Copies only bounded inert descriptors while the actual gameplay code generation remains pinned locally. */
        Result<void> AppendSchemas(std::vector<Network::ReplicationSchemaDescriptor> &schemas,
                                   const Gameplay::GameplayReplicationLease &lease) {
            const auto contribution = lease.Descriptors()->Schemas();
            if (contribution.size() > Network::NetworkReplicationInventory::DescriptorLimits.maximumSchemas - schemas.size())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationCapacityExceeded));
            schemas.insert(schemas.end(), contribution.begin(), contribution.end());
            return Result<void>::Success();
        }

        /** @brief Reads the existing source watch identity; parsing and deep copies happen only on a changed publication. */
        auto PolicyStamp(const std::filesystem::path &root) {
            std::error_code error;
            const auto path = root / ".horo/project.json";
            const auto time = std::filesystem::last_write_time(path, error);
            const auto size = error ? 0 : std::filesystem::file_size(path, error);
            using Stamp = std::pair<std::filesystem::file_time_type, std::uintmax_t>;
            return error ? std::optional<Stamp>{} : std::optional<Stamp>{{time, size}};
        }

        /** @brief Copies actual native metadata under its exact acquired code lease, preserving all non-empty errors. */
        Result<void> AppendNative(const Gameplay::LoadedGameModule &loadedModule,
                                  std::vector<Network::ReplicationSchemaDescriptor> &schemas) {
            auto acquired = loadedModule.Replication().Acquire();
            if (acquired.HasValue())
                return AppendSchemas(schemas, acquired.Value());
            if (loadedModule.Replication().IsFrozen() &&
                acquired.ErrorValue().domain.Value() == Gameplay::GameplayErrors::InvalidReplicationRegistration.domain.Value() &&
                acquired.ErrorValue().code.Value() == Gameplay::GameplayErrors::InvalidReplicationRegistration.code.Value())
                return Result<void>::Success();
            return Result<void>::Failure(acquired.ErrorValue());
        }
    }  // namespace

    /** @copydoc ProjectGameplayRegistry::BuildReplicationMetadata */
    Result<Network::ReplicationMetadataCapturePtr> ProjectGameplayRegistry::BuildReplicationMetadata(
        const std::filesystem::path &projectRoot, const Gameplay::LuaBehaviorProgram *replacement,
        const std::size_t replacementIndex) const {
        auto source = Application::LoadProjectSourceDocument(projectRoot);
        if (source.HasError())
            return Result<Network::ReplicationMetadataCapturePtr>::Failure(source.ErrorValue());
        auto settings = Application::ResolveProjectNetworkSettings(source.Value());
        if (settings.HasError())
            return Result<Network::ReplicationMetadataCapturePtr>::Failure(settings.ErrorValue());
        std::optional<Network::NetworkReplicationInventory> inventory;
        if (settings.Value())
            inventory = settings.Value()->ReplicationInventory();
        std::vector<Network::ReplicationSchemaDescriptor> schemas;
        std::vector<ModuleId> modules;
        if (nativeModule_) {
            modules.emplace_back(nativeModuleId_);
            if (auto copied = AppendNative(*nativeModule_, schemas); copied.HasError())
                return Result<Network::ReplicationMetadataCapturePtr>::Failure(copied.ErrorValue());
        }
        for (std::size_t index = 0; index < luaPrograms_.size(); ++index) {
            const auto &program = replacement && index == replacementIndex ? *replacement : *luaPrograms_[index];
            const auto *declaration = program.ReplicationDeclaration();
            if (!declaration)
                continue;
            auto acquired = program.AcquireReplication();
            if (acquired.HasError())
                return Result<Network::ReplicationMetadataCapturePtr>::Failure(acquired.ErrorValue());
            if (auto copied = AppendSchemas(schemas, acquired.Value()); copied.HasError())
                return Result<Network::ReplicationMetadataCapturePtr>::Failure(copied.ErrorValue());
            modules.push_back(declaration->schema.owner);
        }
        return Network::ReplicationMetadataCapture::Build(inventory, schemas, modules);
    }

    /** @copydoc ProjectGameplayRegistry::CaptureReplicationMetadata */
    void ProjectGameplayRegistry::CaptureReplicationMetadata(const std::filesystem::path &projectRoot) {
        projectRoot_ = projectRoot;
        replicationSourceStamp_ = PolicyStamp(projectRoot);
        auto capture = BuildReplicationMetadata(projectRoot);
        if (capture.HasError()) {
            replicationError_ = capture.ErrorValue();
            diagnostics_.emplace_back(projectRoot / ".horo/project.json", capture.ErrorValue());
            return;
        }
        auto inspection = capture.Value()->Assess(Network::ReplicationDeclarationUse::EditorInspection);
        if (inspection.HasError()) {
            replicationError_ = inspection.ErrorValue();
            diagnostics_.emplace_back(projectRoot / ".horo/project.json", inspection.ErrorValue());
            return;
        }
        replicationMetadata_ = std::move(capture).Value();
        replicationInspection_ = std::move(inspection).Value();
        if (replicationInspection_->DiagnosticsOverflowed() ||
            std::ranges::any_of(replicationInspection_->Diagnostics(), &Network::ReplicationDeclarationDiagnostic::blocking)) {
            replicationError_ = MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible);
            diagnostics_.emplace_back(projectRoot / ".horo/project.json", *replicationError_);
        }
    }

    /** @copydoc ProjectGameplayRegistry::ReplicationMetadata */
    const Network::ReplicationMetadataCapturePtr &ProjectGameplayRegistry::ReplicationMetadata() const noexcept {
        return replicationMetadata_;
    }

    /** @copydoc ProjectGameplayRegistry::ReplicationInspection */
    const Network::ReplicationDeclarationAssessment *ProjectGameplayRegistry::ReplicationInspection() const noexcept {
        return replicationInspection_ ? &*replicationInspection_ : nullptr;
    }

    /** @copydoc ProjectGameplayRegistry::ReplicationMetadataError */
    const Error *ProjectGameplayRegistry::ReplicationMetadataError() const noexcept {
        return replicationError_ ? &*replicationError_ : nullptr;
    }

    /** @copydoc ProjectGameplayRegistry::RefreshReplicationSource */
    Result<void> ProjectGameplayRegistry::RefreshReplicationSource() {
        const auto stamp = PolicyStamp(projectRoot_);
        if (stamp == replicationSourceStamp_)
            return Result<void>::Success();
        replicationSourceStamp_ = stamp;
        auto capture = BuildReplicationMetadata(projectRoot_);
        if (capture.HasError()) {
            replicationError_ = capture.ErrorValue();
            return Result<void>::Failure(*replicationError_);
        }
        auto inspection = capture.Value()->Assess(Network::ReplicationDeclarationUse::EditorInspection);
        if (inspection.HasError()) {
            replicationError_ = inspection.ErrorValue();
            return Result<void>::Failure(*replicationError_);
        }
        if (inspection.Value().DiagnosticsOverflowed() ||
            std::ranges::any_of(inspection.Value().Diagnostics(), &Network::ReplicationDeclarationDiagnostic::blocking)) {
            replicationError_ = MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible);
            return Result<void>::Failure(*replicationError_);
        }
        if (!replicationMetadata_ || replicationMetadata_->Fingerprint() != capture.Value()->Fingerprint()) {
            replicationMetadata_ = std::move(capture).Value();
            replicationInspection_ = std::move(inspection).Value();
        }
        replicationError_.reset();
        std::erase_if(diagnostics_, [this](const auto &diagnostic) {
            return diagnostic.source == projectRoot_ / ".horo/project.json";
        });
        return Result<void>::Success();
    }

    /** @copydoc ProjectGameplayRegistry::ReplaceLuaGeneration */
    Result<void> ProjectGameplayRegistry::ReplaceLuaGeneration(const std::size_t index,
                                                               std::unique_ptr<Gameplay::LuaBehaviorProgram> candidate) {
        auto capture = BuildReplicationMetadata(projectRoot_, candidate.get(), index);
        if (capture.HasError())
            return Result<void>::Failure(capture.ErrorValue());
        auto inspection = capture.Value()->Assess(Network::ReplicationDeclarationUse::EditorInspection);
        if (inspection.HasError())
            return Result<void>::Failure(inspection.ErrorValue());
        if (inspection.Value().DiagnosticsOverflowed() ||
            std::ranges::any_of(inspection.Value().Diagnostics(), &Network::ReplicationDeclarationDiagnostic::blocking))
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
        if (auto replaced = luaPrograms_[index]->ReplaceCompatible(std::move(candidate)); replaced.HasError())
            return replaced;
        replicationMetadata_ = std::move(capture).Value();
        replicationInspection_ = std::move(inspection).Value();
        replicationError_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
